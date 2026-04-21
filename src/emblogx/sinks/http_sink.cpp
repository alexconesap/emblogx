// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "http_sink.h"

#if EMBLOGX_ENABLE_SINK_HTTP

#include <cstdio>
#include <cstring>

#include <http/http_client.h>

namespace emblogx {

    bool HttpSink::begin() {
        if (url_ == nullptr || url_[0] == '\0') {
            return false;
        }
        return dispatcher_.start(&HttpSink::on_record, this, "emblogx_http",
                                 /*stack*/ 8192, /*pri*/ 1, /*core*/ 1);
    }

    void HttpSink::write(const Record& rec) {
        dispatcher_.push(rec);
    }

    // Append one JSON-escaped character into `out` at position `*pos`. Returns
    // false if there isn't enough room for the worst-case 6-byte expansion of
    // a single source byte (\u00XX). The caller is responsible for stopping
    // the message early when this returns false.
    //
    // RFC 8259 §7: characters that MUST be escaped are " \ and any control
    // character in 0x00..0x1F. We use the short forms for the common ones
    // (\b \f \n \r \t) and the \u00XX form for everything else.
    static bool json_escape_byte(char* out, size_t cap, int* pos, char chr) {
        const int p = *pos;
        if (p + 6 > static_cast<int>(cap)) {
            return false;
        }
        auto put = [&](char b) { out[(*pos)++] = b; };
        switch (chr) {
            case '"':
                put('\\');
                put('"');
                return true;
            case '\\':
                put('\\');
                put('\\');
                return true;
            case '\b':
                put('\\');
                put('b');
                return true;
            case '\f':
                put('\\');
                put('f');
                return true;
            case '\n':
                put('\\');
                put('n');
                return true;
            case '\r':
                put('\\');
                put('r');
                return true;
            case '\t':
                put('\\');
                put('t');
                return true;
            default: {
                const auto byte = static_cast<unsigned char>(chr);
                if (byte < 0x20U) {
                    static const char kHex[] = "0123456789abcdef";
                    put('\\');
                    put('u');
                    put('0');
                    put('0');
                    put(kHex[(byte >> 4) & 0xFU]);
                    put(kHex[byte & 0xFU]);
                } else {
                    put(chr);
                }
                return true;
            }
        }
    }

    void HttpSink::on_record(const Record& rec, void* ctx) {
        auto* self = static_cast<HttpSink*>(ctx);

        // Build a small JSON envelope. We avoid any JSON library — the message
        // is escaped inline per RFC 8259, and the module field is escaped the
        // same way (a misbehaving caller could otherwise inject quotes there).
        //
        // Buffer sizing: EMBLOGX_LINE_MAX + 128 covers a typical log line with
        // its envelope. The worst-case JSON escape inflates each input byte to
        // 6 bytes (\u00XX), so a 256-byte input full of control characters
        // would need ~1.6 KiB. Real log lines never look like that, but if the
        // escape function detects it would overflow we drop the record rather
        // than write a partial / invalid JSON object. If you need to forward
        // payloads that legitimately contain large amounts of binary-ish text,
        // bump the buffer size here or do the encoding (base64 / hex) before
        // calling the logger.
        char body[EMBLOGX_LINE_MAX + 128];
        int n = std::snprintf(body, sizeof(body), "{\"target\":%u,\"level\":%u,\"module\":\"",
                              static_cast<unsigned>(rec.target), static_cast<unsigned>(rec.level));
        if (n < 0 || n >= static_cast<int>(sizeof(body))) {
            return;
        }

        const char* mod = rec.module != nullptr ? rec.module : "";
        for (size_t i = 0; mod[i] != '\0'; ++i) {
            if (!json_escape_byte(body, sizeof(body), &n, mod[i])) {
                return;
            }
        }

        int n2 = std::snprintf(body + n, sizeof(body) - static_cast<size_t>(n),
                               "\",\"timestamp\":%llu,\"message\":\"",
                               static_cast<unsigned long long>(rec.timestamp));
        if (n2 < 0 || n + n2 >= static_cast<int>(sizeof(body))) {
            return;
        }
        n += n2;

        for (uint16_t i = 0; i < rec.line_len; ++i) {
            if (!json_escape_byte(body, sizeof(body), &n, rec.line[i])) {
                return;
            }
        }
        if (n + 2 > static_cast<int>(sizeof(body))) {
            return;
        }
        body[n++] = '"';
        body[n++] = '}';

        // Fire and forget. We deliberately ignore the result code — the only
        // thing the caller can do with a failure here is log it, which would
        // recurse into us. The next status push from the host will retry.
        (void)ungula::http::httpPost(self->url_, body, n);
    }

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_HTTP
