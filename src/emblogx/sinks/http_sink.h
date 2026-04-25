// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// HTTP sink — asynchronous, capability LOG | AUDIT | STATUS.
//
// Posts each record as a small JSON body to a configurable endpoint via
// lib_net's httpPost(). The dedicated worker task means the producer task
// never blocks on the network — log calls return in microseconds even
// when the cloud is unreachable.
//
// Gated by EMBLOGX_ENABLE_SINK_HTTP — defaults OFF so projects that don't
// need cloud forwarding don't pay the lib_net dependency.

#include "../async_dispatcher.h"
#include "../config.h"
#include "i_sink.h"

#ifndef EMBLOGX_ENABLE_SINK_HTTP
#define EMBLOGX_ENABLE_SINK_HTTP 0
#endif

#if EMBLOGX_ENABLE_SINK_HTTP

namespace emblogx {

    class HttpSink : public ISink {
        public:
            // endpoint_url is borrowed — must outlive the sink.
            explicit HttpSink(const char* endpoint_url) : url_(endpoint_url) {
                // The JSON payload encodes Record::timestamp as a numeric
                // field, so a duplicated text prefix inside `message`
                // would be noise. Hosts that want the prefix in the
                // `message` field too can call `set_show_timestamp(true)`.
                set_show_timestamp(false);
            }

            uint8_t capabilities() const override {
                return Capability::LOG | Capability::AUDIT | Capability::STATUS;
            }
            Mode mode() const override {
                return Mode::Async;
            }

            bool begin() override;
            void write(const Record& rec) override;
            const char* name() const override {
                return "http";
            }

        private:
            const char* url_;
            AsyncDispatcher dispatcher_;

            static void on_record(const Record& rec, void* ctx);
    };

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_HTTP
