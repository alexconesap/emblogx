// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "logger_core.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_timer.h>
#else
#include <ctime>
#endif

namespace emblogx {

    // ---- Static state -------------------------------------------------------
    // Everything here is process-wide and lock-free for the producer hot path.
    // Sink list is fixed-capacity, written only at boot, read every log call.

    namespace {
        constexpr uint8_t MODULE_LEVEL_SLOTS = 8;

        struct ModuleLevel {
                const char* name;  // stable literal pointer
                Level level;
        };

        ISink* g_sinks[EMBLOGX_MAX_SINKS] = {};
        bool g_sink_enabled[EMBLOGX_MAX_SINKS] = {};
        bool g_sink_begun[EMBLOGX_MAX_SINKS] = {};
        uint8_t g_sink_count = 0;
        bool g_initialized = false;

        Level g_global_level = Level::Info;

        ModuleLevel g_module_levels[MODULE_LEVEL_SLOTS] = {};
        uint8_t g_module_count = 0;

        // ---- Rate limiter state --------------------------------------------
        // One entry per known call site. The `fmt` pointer identifies the
        // site (format strings are literals with static storage). On overflow
        // the oldest entry is evicted.
        constexpr uint8_t RATE_SLOTS = 16;

        struct RateEntry {
                const char* fmt;
                uint64_t last_ms;
        };

        RateEntry g_rate_entries[RATE_SLOTS] = {};
        uint32_t g_rate_limit_ms = 0;

        // Returns true when the record at this call site should be dropped.
        bool rate_limited(const char* fmt, uint64_t now) {
            if (g_rate_limit_ms == 0 || fmt == nullptr) {
                return false;
            }
            uint8_t oldest = 0;
            uint64_t oldest_ts = UINT64_MAX;
            uint8_t free_slot = RATE_SLOTS;
            for (uint8_t i = 0; i < RATE_SLOTS; ++i) {
                if (g_rate_entries[i].fmt == fmt) {
                    if (now - g_rate_entries[i].last_ms < g_rate_limit_ms) {
                        return true;
                    }
                    g_rate_entries[i].last_ms = now;
                    return false;
                }
                if (g_rate_entries[i].fmt == nullptr && free_slot == RATE_SLOTS) {
                    free_slot = i;
                }
                if (g_rate_entries[i].last_ms < oldest_ts) {
                    oldest_ts = g_rate_entries[i].last_ms;
                    oldest = i;
                }
            }
            const uint8_t target_slot = (free_slot < RATE_SLOTS) ? free_slot : oldest;
            g_rate_entries[target_slot] = RateEntry{fmt, now};
            return false;
        }

        // Effective level for a record — checks the per-module override first,
        // then falls back to the global level.
        Level effective_level(const char* module) {
            if (module != nullptr) {
                for (uint8_t i = 0; i < g_module_count; ++i) {
                    if (g_module_levels[i].name == module ||  // pointer match (literal interning)
                        std::strcmp(g_module_levels[i].name, module) == 0) {
                        return g_module_levels[i].level;
                    }
                }
            }
            return g_global_level;
        }
    }  // namespace

    // ---- Registry -----------------------------------------------------------

    bool register_sink(ISink* sink) {
        if (sink == nullptr) {
            return false;
        }
        if (g_sink_count >= EMBLOGX_MAX_SINKS) {
            return false;
        }
        g_sinks[g_sink_count] = sink;
        g_sink_enabled[g_sink_count] = true;
        ++g_sink_count;
        return true;
    }

    uint8_t sink_count() {
        return g_sink_count;
    }

    void init() {
        // Allow re-entry: sinks registered after the first init() call (like SD
        // sink registered after mount, while lazy init already ran for an earlier
        // log call) must still get their begin() called. The g_sink_begun[] guard
        // prevents double-begin on sinks that were already processed.
        g_initialized = true;
        for (uint8_t i = 0; i < g_sink_count; ++i) {
            if (g_sinks[i] != nullptr && g_sink_enabled[i] && !g_sink_begun[i]) {
                g_sink_begun[i] = true;
                if (!g_sinks[i]->begin()) {
                    g_sink_enabled[i] = false;
                }
            }
        }
    }

    void flush_all() {
        for (uint8_t i = 0; i < g_sink_count; ++i) {
            if (g_sinks[i] != nullptr && g_sink_enabled[i]) {
                g_sinks[i]->flush();
            }
        }
    }

    // ---- Runtime config -----------------------------------------------------

    void set_global_level(Level lvl) {
        g_global_level = lvl;
    }

    Level get_global_level() {
        return g_global_level;
    }

    bool set_module_level(const char* module, Level lvl) {
        if (module == nullptr) {
            return false;
        }
        // Update existing entry first
        for (uint8_t i = 0; i < g_module_count; ++i) {
            if (g_module_levels[i].name == module ||
                std::strcmp(g_module_levels[i].name, module) == 0) {
                g_module_levels[i].level = lvl;
                return true;
            }
        }
        if (g_module_count >= MODULE_LEVEL_SLOTS) {
            return false;
        }
        g_module_levels[g_module_count++] = ModuleLevel{module, lvl};
        return true;
    }

    void set_sink_enabled(uint8_t index, bool enabled) {
        if (index >= g_sink_count) {
            return;
        }
        g_sink_enabled[index] = enabled;
    }

    bool is_sink_enabled(uint8_t index) {
        if (index >= g_sink_count) {
            return false;
        }
        return g_sink_enabled[index];
    }

    void set_rate_limit_ms(uint32_t ms) {
        g_rate_limit_ms = ms;
    }

    uint32_t get_rate_limit_ms() {
        return g_rate_limit_ms;
    }

    ISink* sink_at(uint8_t index) {
        if (index >= g_sink_count) {
            return nullptr;
        }
        return g_sinks[index];
    }

    // ---- Wall clock ---------------------------------------------------------

    uint64_t now_ms() {
#ifdef ESP_PLATFORM
        return static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);
#else
        timespec tsp{};
        clock_gettime(CLOCK_MONOTONIC, &tsp);
        return (static_cast<uint64_t>(tsp.tv_sec) * 1000ULL) +
               (static_cast<uint64_t>(tsp.tv_nsec) / 1000000ULL);
#endif
    }

    // ---- Internal log entry point ------------------------------------------
    //
    // Format ONCE into a stack buffer, then walk the sink list. The producer
    // pays the formatting cost exactly once regardless of how many sinks are
    // registered. Async sinks copy the bytes into their own queue; sync sinks
    // consume the buffer in place.

    namespace {
        void log_va_impl(uint8_t target, Level lvl, const char* module, const char* fmt,
                         va_list args, bool force) {
            if (!g_initialized) {
                init();
            }

            // Drop on level filter — also drops Debug entirely if not compiled in.
#ifndef EMBLOGX_DEBUG_ENABLED
            if (lvl == Level::Debug) {
                return;
            }
#endif
            if (static_cast<uint8_t>(lvl) < static_cast<uint8_t>(effective_level(module))) {
                return;
            }
            if (target == 0 || g_sink_count == 0) {
                return;
            }

            // Rate limiter — errors and forced calls always go through.
            if (!force && lvl != Level::Error && rate_limited(fmt, now_ms())) {
                return;
            }

            // ---- Format once ------------------------------------------------
            char line[EMBLOGX_LINE_MAX];
            const char* level_str = levelName(lvl);
            const char* mod_str = (module != nullptr && module[0] != '\0') ? module : "-";

            int header_len = std::snprintf(line, sizeof(line), "%s[%s][%s] ",
                                           EMBLOGX_LOG_PREFIX, level_str, mod_str);
            if (header_len < 0) {
                return;
            }
            if (header_len >= static_cast<int>(sizeof(line))) {
                header_len = static_cast<int>(sizeof(line)) - 1;
            }

            // Forward via va_copy. Strictly the current call sites only consume
            // the va_list once so passing `args` directly would also work, but
            // va_copy is the textbook-correct pattern for any function that takes
            // a forwarded va_list — it makes the implementation safe against future
            // code paths (level filters, retries, fallback formatting) that might
            // want to walk the args a second time.
            va_list args_copy;
            va_copy(args_copy, args);
            int body_len = std::vsnprintf(line + header_len,
                                          sizeof(line) - static_cast<size_t>(header_len),
                                          fmt, args_copy);
            va_end(args_copy);
            if (body_len < 0) {
                return;
            }

            int total = header_len + body_len;
            if (total >= static_cast<int>(sizeof(line))) {
                // Truncated — keep within bounds, no trailing newline guarantee.
                total = static_cast<int>(sizeof(line)) - 1;
                line[total] = '\0';
            }

            // ---- Build record and dispatch ----------------------------------
            Record rec{};
            rec.target = target;
            rec.level = lvl;
            rec.module = mod_str;
            rec.line = line;
            rec.line_len = static_cast<uint16_t>(total);
            rec.timestamp = now_ms();

            for (uint8_t i = 0; i < g_sink_count; ++i) {
                ISink* sink = g_sinks[i];
                if (sink == nullptr || !g_sink_enabled[i]) {
                    continue;
                }
                if ((rec.target & sink->capabilities()) == 0) {
                    continue;
                }
                sink->write(rec);
            }
        }
    }  // namespace

    void log_va(uint8_t target, Level lvl, const char* module, const char* fmt, va_list args) {
        log_va_impl(target, lvl, module, fmt, args, false);
    }

    void log_va_force(uint8_t target, Level lvl, const char* module, const char* fmt,
                      va_list args) {
        log_va_impl(target, lvl, module, fmt, args, true);
    }

}  // namespace emblogx
