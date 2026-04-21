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
    };

}  // namespace emblogx
