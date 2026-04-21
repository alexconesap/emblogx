// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// Console sink — synchronous, capability LOG.
//
// Compile-time backend selection follows the same convention as the legacy
// logger:
//   -DEMBLOGX_BACKEND_ESP32   ESP-IDF UART driver on UART0
//   -DEMBLOGX_BACKEND_STDIO   POSIX stdout (host tests, dev machines)
//
// Aliases for the legacy LOGGER_BACKEND_* names are honoured so existing
// build systems keep working during the migration.
//
// Gated by EMBLOGX_ENABLE_SINK_CONSOLE — defaults ON because a host with
// no other sink configured still wants its log output to go somewhere.

#include "../config.h"
#include "i_sink.h"

#if defined(LOGGER_BACKEND_ESP32) && !defined(EMBLOGX_BACKEND_ESP32)
#define EMBLOGX_BACKEND_ESP32
#endif
#if defined(LOGGER_BACKEND_STDIO) && !defined(EMBLOGX_BACKEND_STDIO)
#define EMBLOGX_BACKEND_STDIO
#endif

#ifndef EMBLOGX_ENABLE_SINK_CONSOLE
#define EMBLOGX_ENABLE_SINK_CONSOLE 1
#endif

#if EMBLOGX_ENABLE_SINK_CONSOLE

namespace emblogx {

    class ConsoleSink : public ISink {
        public:
            uint8_t capabilities() const override {
                return Capability::LOG;
            }
            Mode mode() const override {
                return Mode::Sync;
            }

            bool begin() override;
            void write(const Record& rec) override;
            void flush() override;
            const char* name() const override {
                return "console";
            }
    };

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_CONSOLE
