// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

#include <cstdint>

// Public type definitions for emblogx — kept in their own header so any
// code that just needs the enums can include this without dragging the
// full facade and its dependencies.

namespace emblogx {

    // ---- Severity ----------------------------------------------------------
    enum class Level : uint8_t {
        Debug = 0,
        Info = 1,
        Warn = 2,
        Error = 3,
    };

    // ---- Routing targets (bitmask) -----------------------------------------
    // A record carries a target bitmask. Each registered sink declares the
    // capabilities it supports. The router walks the sinks and dispatches the
    // record only to those whose capabilities intersect the record's target.
    namespace Target {
        constexpr uint8_t LOG = 1U << 0;     // operational logs
        constexpr uint8_t AUDIT = 1U << 1;   // regulatory / FDA audit trail
        constexpr uint8_t STATUS = 1U << 2;  // state / event reporting
        constexpr uint8_t ALL = LOG | AUDIT | STATUS;
    }  // namespace Target

    // ---- Sink capability bitmask -------------------------------------------
    // Same shape as Target — a sink declares which targets it accepts.
    namespace Capability {
        constexpr uint8_t LOG = Target::LOG;
        constexpr uint8_t AUDIT = Target::AUDIT;
        constexpr uint8_t STATUS = Target::STATUS;
        constexpr uint8_t ALL = Target::ALL;
    }  // namespace Capability

    // ---- Synchronous vs asynchronous dispatch ------------------------------
    enum class Mode : uint8_t {
        Sync,   // direct call inside the producer task — fast, no queue
        Async,  // hand off to a worker task via a static queue — non-blocking
    };

    // ---- Internal record passed to sinks -----------------------------------
    // The record's text field is owned by the caller (a stack buffer inside
    // log_write). Sinks must consume / copy it before returning — the pointer
    // becomes invalid as soon as log_write returns.
    struct Record {
            uint8_t target;  // Target::* bitmask
            Level level;
            const char* module;  // stable string literal — never copied
            const char* line;    // pre-formatted line, no trailing newline
            uint16_t line_len;   // strlen(line), excluding any trailing newline
            // Bytes at the start of `line` taken by the optional
            // "[YYYY-MM-DD HH:MM:SS] " timestamp prefix (0 when no real
            // wall-clock source is available, or the format is disabled).
            // A sink with `show_timestamp() == false` skips this many
            // bytes when emitting `line` — see ISink::effective_line().
            uint16_t timestamp_prefix_len;
            int64_t timestamp;   // milliseconds — meaning depends on the
                                 // installed time-source provider:
                                 //   - default: monotonic since boot
                                 //   - bridged: Unix epoch ms (e.g. via
                                 //     `set_now_ms_provider(&TimeControl::now)`)
                                 // Signed 64-bit so it never wraps and so
                                 // diffs between two records produce a
                                 // sane signed result.
    };

    // ---- Helpers ------------------------------------------------------------
    inline const char* levelName(Level lvl) {
        switch (lvl) {
            case Level::Debug:
                return "DEBUG";
            case Level::Info:
                return "INFO";
            case Level::Warn:
                return "WARN";
            case Level::Error:
                return "ERROR";
        }
        return "INFO";
    }

}  // namespace emblogx
