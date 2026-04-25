// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "console_sink.h"

#if EMBLOGX_ENABLE_SINK_CONSOLE

// Backend: write to stdout. On ESP-IDF (including ESP32-S3 with USB Serial/
// JTAG console), newlib routes stdout to whichever console the IDF startup
// has configured — UART0, USB-Serial-JTAG, or HWCDC — so a single fwrite()
// path covers every board variant without touching the UART driver directly
// (which fights with the framework's own console wiring) and without pulling
// in any Arduino headers.
//
// EMBLOGX_BACKEND_ESP32 / EMBLOGX_BACKEND_STDIO are accepted for backwards
// compatibility but no longer change behaviour: both resolve to stdout.

#include <cstdio>

namespace emblogx {

    bool ConsoleSink::begin() {
        return true;
    }

    void ConsoleSink::write(const Record& rec) {
        std::fwrite(effective_line(rec), 1, effective_line_len(rec), stdout);
        std::fputc('\n', stdout);
    }

    void ConsoleSink::flush() {
        std::fflush(stdout);
    }

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_CONSOLE
