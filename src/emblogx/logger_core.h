// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// Core API for emblogx — the single internal log() function and the runtime
// configuration knobs (level, sink registry, per-module filters).
//
// Most user code should NOT include this header — include <emblogx/logger.h>
// instead, which provides the printf-style wrappers (log_info, audit_warn,
// status_error, ...). This header is for sink implementations and for the
// host's setup code that registers sinks.

#include <cstdarg>
#include <cstdint>

#include "config.h"
#include "sinks/i_sink.h"
#include "types.h"

namespace emblogx {

    // ---- Sink registry ------------------------------------------------------

    // Register a sink. Returns false if the registry is full (EMBLOGX_MAX_SINKS).
    // Call this once per sink at boot, before producing any log records.
    bool register_sink(ISink* sink);

    // Number of currently registered sinks.
    uint8_t sink_count();

    // Initialise the registry — calls begin() on every registered sink that
    // has not been begun yet. Safe to call multiple times; sinks registered
    // after the first call are processed on the next call.
    void init();

    // Flush every sink. Use before reboot or shutdown.
    void flush_all();

    // ---- Runtime configuration ---------------------------------------------

    // Global minimum level. Records below this level are dropped before
    // routing. Default: Info (Debug needs both compile flag and runtime
    // promotion to be visible).
    void set_global_level(Level lvl);
    Level get_global_level();

    // Per-module override. Module strings must be stable literals — only the
    // pointer is stored, never copied. A NULL or unmatched module falls back
    // to the global level.
    // Returns false if the per-module table is full (small fixed cap).
    bool set_module_level(const char* module, Level lvl);

    // Toggle a sink at runtime by index (0..sink_count()-1).
    void set_sink_enabled(uint8_t index, bool enabled);
    bool is_sink_enabled(uint8_t index);

    // Get a sink pointer by index. Returns nullptr if out of range.
    ISink* sink_at(uint8_t index);

    // ---- Time source -------------------------------------------------------
    //
    // emblogx is intentionally independent of any time-management library —
    // a logger should keep working even when other subsystems are not
    // present or are broken. The default `now_ms()` implementation returns
    // monotonic milliseconds since boot (esp_timer on ESP-IDF,
    // clock_gettime(CLOCK_MONOTONIC) on POSIX).
    //
    // To carry wall-clock timestamps in log records (e.g. once NTP is up),
    // the host project plugs in its own time source via
    // `set_now_ms_provider()`. One-line bridge to UngulaCore's TimeControl:
    //
    //   emblogx::set_now_ms_provider(&ungula::core::time::TimeControl::now);
    //
    // The hook is `int64_t (*)()` — signed to match the rest of the
    // ecosystem (POSIX `time_t`, ESP-IDF timers, UngulaCore's TimeControl).

    using NowMsFn = int64_t (*)();

    // Replace the time source. Pass nullptr to revert to the built-in
    // monotonic-since-boot default. Register at boot, before producing
    // any log records — records emitted before and after the swap will
    // carry timestamps from different sources and won't be comparable.
    void set_now_ms_provider(NowMsFn fname);

    // Currently registered provider, or nullptr if the default is in use.
    NowMsFn get_now_ms_provider();

    // Returns a millisecond timestamp suitable for the Record::timestamp
    // field. Goes through the registered provider if one was installed,
    // otherwise the built-in monotonic source.
    int64_t now_ms();

    // ---- Rate limiting -----------------------------------------------------
    // Drops repeated calls from the same call site (same `fmt` pointer) when
    // they arrive faster than the configured interval. Useful when code in
    // loop() logs on every iteration and would otherwise flood the console
    // or a slow sink. Error-level records always go through. The `_force`
    // variants in logger.h also bypass the check — use them for boot banners
    // and one-shot state transitions that must not be dropped.
    //
    // Default: 0 ms (disabled). The rate check is keyed by the `fmt` pointer
    // so the format string MUST be a literal (same contract as the `module`
    // argument).
    void set_rate_limit_ms(uint32_t ms);
    uint32_t get_rate_limit_ms();

    // ---- The single log entry point ----------------------------------------
    // All printf-style wrappers ultimately call this. The `_force` variant
    // bypasses the rate limiter.
    void log_va(uint8_t target, Level lvl, const char* module, const char* fmt, va_list args);
    void log_va_force(uint8_t target, Level lvl, const char* module, const char* fmt, va_list args);

    // Convenience overload for non-printf callers.
    inline void log(uint8_t target, Level lvl, const char* module, const char* fmt, ...)
            __attribute__((format(printf, 4, 5)));
    inline void log(uint8_t target, Level lvl, const char* module, const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        log_va(target, lvl, module, fmt, args);
        va_end(args);
    }

}  // namespace emblogx
