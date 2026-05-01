// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// emblogx — single-call embedded logging facade.
//
// Include this from your application code. The wrappers below all resolve
// to a single internal log_va() call, which formats the line once and then
// routes it to every registered sink whose capabilities intersect the
// record's target bitmask.
//
// Quick start:
//
//   #include <emblogx/logger.h>
//   #include <emblogx/sinks/console_sink.h>
//
//   static emblogx::ConsoleSink console;
//
//   void setup() {
//       emblogx::register_sink(&console);
//       emblogx::init();
//
//       log_info("rbb1", "Boot complete, version %s", VERSION);
//       audit_info("safety", "EMERGENCY_STOP_ARMED");
//       status_warn("wifi", "rssi=%d state=%d", rssi, state);
//       log_audit_info("cycle", "PROGRAM_SELECT idx=%d", idx);
//   }
//
// Targets:
//
//   LOG     -> operational logs (console, memory, optionally cloud)
//   AUDIT   -> regulatory / FDA audit trail (SD card, HTTP)
//   STATUS  -> state / event reporting for dashboards
//
// One call routes to as many sinks as match. There is no need (and no API)
// for "dual logging" — combining LOG and AUDIT is just log_audit_info(...).
//
// Sinks declare their own capabilities and sync/async mode; the router
// decides at runtime where each record goes. The producer task pays the
// formatting cost exactly once regardless of how many sinks are wired.

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include "logger_core.h"
#include "types.h"

// ---- Module identifier ----------------------------------------------------
// The module argument MUST be a string literal or any other pointer with
// static storage duration — emblogx stores the pointer and never copies
// the string. Passing a temporary buffer (e.g. a stack-allocated `char[]`,
// the result of a String c_str(), or a heap allocation that will be freed)
// leaves dangling pointers in the per-module level table and in any
// queued async record. There is no runtime check for this.
//
// Typical values are subsystem names like "wifi", "ota", "rbb2". Pass
// nullptr or "" if you don't need a module identifier.

namespace emblogx {

    namespace detail {

        inline void emit(uint8_t target, Level lvl, const char* module, const char* fmt, ...)
                __attribute__((format(printf, 4, 5)));
        inline void emit(uint8_t target, Level lvl, const char* module, const char* fmt, ...) {
            va_list args;
            va_start(args, fmt);
            log_va(target, lvl, module, fmt, args);
            va_end(args);
        }

    }  // namespace detail

}  // namespace emblogx

// ---- Wrappers -------------------------------------------------------------
// The first argument can be either the format string or a module name.
// To keep things simple and avoid macro shadowing, we expose two flavours:
//
//   log_info(fmt, ...)         — no module (module = nullptr)
//   logm_info(module, fmt, ...) — with module (module = string literal)
//
// Same for warn/error/debug, audit_*, status_*, log_audit_*.
//
// In practice, host code can pass nullptr as module; the format-only flavour
// is provided as a convenience for the common case.

// ===== LOG (operational) ===================================================

#define EMBLOGX_DEFINE_WRAPPERS(NAME, TARGET_BITS, LEVEL)                                 \
    inline void NAME(const char* fmt, ...) __attribute__((format(printf, 1, 2)));         \
    inline void NAME(const char* fmt, ...) {                                              \
        va_list args;                                                                     \
        va_start(args, fmt);                                                              \
        ::emblogx::log_va((TARGET_BITS), (LEVEL), nullptr, fmt, args);                    \
        va_end(args);                                                                     \
    }                                                                                     \
    inline void NAME##_m(const char* module, const char* fmt, ...)                        \
            __attribute__((format(printf, 2, 3)));                                        \
    inline void NAME##_m(const char* module, const char* fmt, ...) {                      \
        va_list args;                                                                     \
        va_start(args, fmt);                                                              \
        ::emblogx::log_va((TARGET_BITS), (LEVEL), module, fmt, args);                     \
        va_end(args);                                                                     \
    }                                                                                     \
    inline void NAME##_force(const char* fmt, ...) __attribute__((format(printf, 1, 2))); \
    inline void NAME##_force(const char* fmt, ...) {                                      \
        va_list args;                                                                     \
        va_start(args, fmt);                                                              \
        ::emblogx::log_va_force((TARGET_BITS), (LEVEL), nullptr, fmt, args);              \
        va_end(args);                                                                     \
    }                                                                                     \
    inline void NAME##_force_m(const char* module, const char* fmt, ...)                  \
            __attribute__((format(printf, 2, 3)));                                        \
    inline void NAME##_force_m(const char* module, const char* fmt, ...) {                \
        va_list args;                                                                     \
        va_start(args, fmt);                                                              \
        ::emblogx::log_va_force((TARGET_BITS), (LEVEL), module, fmt, args);               \
        va_end(args);                                                                     \
    }

// LOG ----------------------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(log_info, ::emblogx::Target::LOG, ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(log_warn, ::emblogx::Target::LOG, ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(log_error, ::emblogx::Target::LOG, ::emblogx::Level::Error)
EMBLOGX_DEFINE_WRAPPERS(log_debug, ::emblogx::Target::LOG, ::emblogx::Level::Debug)

// AUDIT --------------------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(audit_info, ::emblogx::Target::AUDIT, ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(audit_warn, ::emblogx::Target::AUDIT, ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(audit_error, ::emblogx::Target::AUDIT, ::emblogx::Level::Error)
EMBLOGX_DEFINE_WRAPPERS(audit_debug, ::emblogx::Target::AUDIT, ::emblogx::Level::Debug)

// STATUS -------------------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(status_info, ::emblogx::Target::STATUS, ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(status_warn, ::emblogx::Target::STATUS, ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(status_error, ::emblogx::Target::STATUS, ::emblogx::Level::Error)

// LOG | AUDIT --------------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(log_audit_info, ::emblogx::Target::LOG | ::emblogx::Target::AUDIT,
                        ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(log_audit_warn, ::emblogx::Target::LOG | ::emblogx::Target::AUDIT,
                        ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(log_audit_error, ::emblogx::Target::LOG | ::emblogx::Target::AUDIT,
                        ::emblogx::Level::Error)
EMBLOGX_DEFINE_WRAPPERS(log_audit_debug, ::emblogx::Target::LOG | ::emblogx::Target::AUDIT,
                        ::emblogx::Level::Debug)

// LOG | STATUS -------------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(log_status_info, ::emblogx::Target::LOG | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(log_status_warn, ::emblogx::Target::LOG | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(log_status_error, ::emblogx::Target::LOG | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Error)

// AUDIT | STATUS -----------------------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(audit_status_info, ::emblogx::Target::AUDIT | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(audit_status_warn, ::emblogx::Target::AUDIT | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(audit_status_error, ::emblogx::Target::AUDIT | ::emblogx::Target::STATUS,
                        ::emblogx::Level::Error)

// ALL = LOG | AUDIT | STATUS ----------------------------------------------
EMBLOGX_DEFINE_WRAPPERS(all_info, ::emblogx::Target::ALL, ::emblogx::Level::Info)
EMBLOGX_DEFINE_WRAPPERS(all_warn, ::emblogx::Target::ALL, ::emblogx::Level::Warn)
EMBLOGX_DEFINE_WRAPPERS(all_error, ::emblogx::Target::ALL, ::emblogx::Level::Error)

#undef EMBLOGX_DEFINE_WRAPPERS

// ---- Structured event helpers --------------------------------------------
//
// Convenience for structured records that carry a numeric event code in
// addition to the message. The code is rendered as the first token of the
// formatted body so existing string-based sinks just see it as text. New
// sinks (e.g. binary memory tracer) can later look at it via the Record
// struct directly.

inline void audit_event(int code, const char* module, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));
inline void audit_event(int code, const char* module, const char* fmt, ...) {
    char body[EMBLOGX_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    ::emblogx::detail::emit(::emblogx::Target::AUDIT, ::emblogx::Level::Info, module, "code=%d %s",
                            code, body);
}

inline void status_event(int code, const char* module, const char* fmt, ...)
        __attribute__((format(printf, 3, 4)));
inline void status_event(int code, const char* module, const char* fmt, ...) {
    char body[EMBLOGX_LINE_MAX];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(body, sizeof(body), fmt, args);
    va_end(args);
    ::emblogx::detail::emit(::emblogx::Target::STATUS, ::emblogx::Level::Info, module, "code=%d %s",
                            code, body);
}

// ---- Rate limiting --------------------------------------------------------
// Throttles repeated calls from the same call site (same format-string
// pointer) so that logs inside a tight loop() do not flood the console or
// slow sinks. Error-level records always go through; the `_force` / `_force_m`
// variants above also bypass the check for boot banners and one-shot events
// that must never be dropped.
//
// Default: 0 (disabled). Set a non-zero interval at boot to enable:
//
//   log_set_rate_limit_ms(1000);   // at most one line per fmt per second
//
// The check is keyed by the `fmt` pointer. The format string must be a
// string literal (it already has to be for the printf format attribute).

inline void log_set_rate_limit_ms(uint32_t interval_ms) {
    ::emblogx::set_rate_limit_ms(interval_ms);
}

inline uint32_t log_get_rate_limit_ms() {
    return ::emblogx::get_rate_limit_ms();
}

// ---- Lifecycle ------------------------------------------------------------
//
// Threading contract:
//   The host project MUST register all sinks and call emblogx::init() (or
//   log_init()) from a single boot context — typically setup() or the
//   ESP-IDF app_main() — BEFORE spawning any task that calls log_*. The
//   sink registry, the per-module level table and the lazy first-call
//   init() path are not protected by a mutex on the producer hot path
//   because that would add a critical section to every single log line.
//   Once the boot phase is complete, log_*, set_global_level() and
//   set_module_level() are safe to call from any number of tasks.
//
//   Calling register_sink() after init() — i.e. while another task may
//   already be inside log_va() — is undefined behavior. There is no
//   "unregister" by design.

inline void log_init() {
    ::emblogx::init();
}

inline void log_flush() {
    ::emblogx::flush_all();
}
