// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// AsyncDispatcher — small fixed-capacity producer/consumer queue used by
// the async sinks (HTTP, SD, and any custom sink that opts into Mode::Async).
// Holds EMBLOGX_QUEUE_SLOTS records of up to EMBLOGX_LINE_MAX bytes each,
// all statically allocated. No heap.
//
// Behaviour:
//   * push(rec)        -> non-blocking, copies rec into the next free slot.
//                          Drops oldest record on overflow (head advances).
//   * pop(out_rec)     -> non-blocking, returns false when empty.
//   * On ESP-IDF the consumer runs in a dedicated FreeRTOS task that calls
//     a user-provided handler for each popped record.
//   * On host (tests) push() invokes the handler directly so test logic
//     remains deterministic without spawning threads.
//
// Each AsyncDispatcher instance owns its own task — keep them static at
// file scope so they live for the entire process lifetime.

#include <cstdint>

#include "config.h"
#include "types.h"

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#endif

namespace emblogx {

    class AsyncDispatcher {
        public:
            using Handler = void (*)(const Record& rec, void* ctx);

            AsyncDispatcher() = default;
            ~AsyncDispatcher();

            AsyncDispatcher(const AsyncDispatcher&) = delete;
            AsyncDispatcher& operator=(const AsyncDispatcher&) = delete;

            // Start the worker task. Returns false if the task could not be created
            // (e.g. out of memory). Safe to call multiple times — only the first
            // call has effect.
            //
            // task_name        FreeRTOS task name (must be a stable string literal)
            // stack_size_words FreeRTOS stack in words. Caller-chosen, depends on
            //                  what the handler does. 4096 is enough for the SD
            //                  sink (file I/O), 8192 is enough for the HTTP sink
            //                  (TCP/TLS through esp_http_client).
            // priority         FreeRTOS task priority (1 is fine for log workers)
            // core             core to pin to (1 = network core on dual-core ESP32)
            bool start(Handler handler, void* ctx, const char* task_name,
                       uint32_t stack_size_words = 4096, uint8_t priority = 1, int core = 1);

            // Stop the worker task. Drains all pending records first.
            void stop();

            // Enqueue a record. Non-blocking. Returns false only if start() was
            // never called. Records that overflow the queue evict the oldest
            // entry — newest data is always kept.
            bool push(const Record& rec);

        private:
            static constexpr uint32_t SLOT_BYTES = EMBLOGX_LINE_MAX;
            static constexpr uint32_t SLOTS = EMBLOGX_QUEUE_SLOTS;

            // One slot = one fully formatted line + the metadata fields we need
            // at consumption time. The timestamp is 64-bit so it matches the
            // Record::timestamp width and never wraps for the lifetime of the
            // device. Truncating to 32-bit here would silently wrap every ~49
            // days for async sinks, which is exactly what we changed Record to
            // avoid.
            struct Slot {
                    uint8_t target;
                    Level level;
                    const char* module;  // stable literal pointer
                    uint64_t timestamp;
                    uint16_t line_len;
                    char line[SLOT_BYTES];
            };

            Slot slots_[SLOTS] = {};
            uint32_t head_ = 0;   // next read
            uint32_t tail_ = 0;   // next write
            uint32_t count_ = 0;  // number of pending records

            Handler handler_ = nullptr;
            void* ctx_ = nullptr;
            bool running_ = false;

#ifdef ESP_PLATFORM
            TaskHandle_t task_handle_ = nullptr;
            SemaphoreHandle_t mutex_ = nullptr;

            static void task_entry(void* param);
            void task_loop();
#endif

            bool pop(Slot& out);
    };

}  // namespace emblogx
