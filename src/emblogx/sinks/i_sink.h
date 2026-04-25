// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

#include "../types.h"

namespace emblogx {

    // Sink interface — every concrete sink (console, memory, http, sd, ...)
    // implements this. The router holds a static array of pointers, no heap.
    //
    // Lifetime:
    //   sinks are static instances declared at file scope by the host project
    //   and registered once at boot. They live forever — there is no remove().
    //
    // Threading:
    //   write() is called from the producer task (whoever called log_*) for
    //   sync sinks, and from a dedicated worker task for async sinks. Each
    //   sink is responsible for its own internal locking if it needs any.
    class ISink {
        public:
            virtual ~ISink() = default;

            // Bitmask of Capability::* this sink accepts.
            virtual uint8_t capabilities() const = 0;

            // Sync vs Async dispatch policy.
            virtual Mode mode() const = 0;

            // Called once at boot, before any write(). Should be cheap and never
            // throw. Return false to disable the sink at runtime.
            virtual bool begin() {
                return true;
            }

            // Called by the router for sync sinks, or by the worker task for
            // async sinks. The Record::line pointer is only valid for the
            // duration of the call — sinks must copy if they need it later.
            virtual void write(const Record& rec) = 0;

            // Optional. Used by log_flush() at shutdown / reboot.
            virtual void flush() {}

            // Optional human-readable name for diagnostics.
            virtual const char* name() const {
                return "sink";
            }

            // ---- Timestamp prefix gating ----------------------------------
            //
            // The formatter in logger_core.cpp prepends
            // "[YYYY-MM-DD HH:MM:SS] " to `Record::line` whenever the
            // installed time source returns a real wall-clock value. By
            // default every sink emits that prefix; a sink can opt out at
            // runtime via `set_show_timestamp(false)` — useful for sinks
            // that already carry the timestamp out-of-band (e.g. the HTTP
            // sink encodes `Record::timestamp` as a JSON field, so a
            // duplicated text prefix would be noise).

            void set_show_timestamp(bool enabled) {
                show_timestamp_ = enabled;
            }

            virtual bool show_timestamp() const {
                return show_timestamp_;
            }

            // Helpers — every concrete sink writes `effective_line` /
            // `effective_line_len` instead of `rec.line` / `rec.line_len`
            // so the per-sink flag is honoured automatically. Constant-
            // time: just a pointer arithmetic / subtraction.
            const char* effective_line(const Record& rec) const {
                if (show_timestamp() || rec.timestamp_prefix_len == 0) {
                    return rec.line;
                }
                return rec.line + rec.timestamp_prefix_len;
            }
            uint16_t effective_line_len(const Record& rec) const {
                if (show_timestamp() || rec.timestamp_prefix_len == 0) {
                    return rec.line_len;
                }
                return static_cast<uint16_t>(rec.line_len - rec.timestamp_prefix_len);
            }

        protected:
            // Default true: every sink renders the timestamp prefix when
            // a wall-clock source is registered. The HTTP sink overrides
            // this to false in its constructor because its JSON payload
            // already carries `Record::timestamp` as a separate numeric
            // field.
            bool show_timestamp_ = true;
    };

}  // namespace emblogx
