// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "async_dispatcher.h"

#include <cstring>

namespace emblogx {

    AsyncDispatcher::~AsyncDispatcher() {
        stop();
    }

    // ---- Internal queue helpers (no locking outside the ESP-IDF mutex) ------

    bool AsyncDispatcher::pop(Slot& out) {
        if (count_ == 0) {
            return false;
        }
        out = slots_[head_];
        head_ = (head_ + 1) % SLOTS;
        --count_;
        return true;
    }

#ifdef ESP_PLATFORM

    bool AsyncDispatcher::start(Handler handler, void* ctx, const char* task_name,
                                uint32_t stack_size_words, uint8_t priority, int core) {
        if (running_) {
            return true;
        }
        if (handler == nullptr) {
            return false;
        }

        handler_ = handler;
        ctx_ = ctx;

        if (mutex_ == nullptr) {
            mutex_ = xSemaphoreCreateMutex();
            if (mutex_ == nullptr) {
                return false;
            }
        }

        running_ = true;
        BaseType_t res =
                xTaskCreatePinnedToCore(&AsyncDispatcher::task_entry, task_name, stack_size_words,
                                        this, priority, &task_handle_, core);
        if (res != pdPASS) {
            running_ = false;
            task_handle_ = nullptr;
            return false;
        }
        return true;
    }

    void AsyncDispatcher::stop() {
        if (!running_) {
            return;
        }
        running_ = false;
        if (task_handle_ != nullptr) {
            // Wake the task so it notices the running_=false flip and exits.
            xTaskNotifyGive(task_handle_);
            // Wait for the worker to actually self-delete (it sets task_handle_
            // back to nullptr just before vTaskDelete()). We MUST observe
            // task_handle_ == nullptr before touching the mutex below — the
            // worker holds the mutex inside its hot loop and freeing it under a
            // running task would be undefined behavior. Cap the wait at ~2s as a
            // safety net; if we time out we leak the mutex rather than risk a
            // use-after-free.
            for (int i = 0; i < 200 && task_handle_ != nullptr; ++i) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        if (task_handle_ == nullptr && mutex_ != nullptr) {
            vSemaphoreDelete(mutex_);
            mutex_ = nullptr;
        }
    }

    bool AsyncDispatcher::push(const Record& rec) {
        if (!running_ || mutex_ == nullptr) {
            return false;
        }

        if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(5)) != pdTRUE) {
            // Mutex contended — drop rather than block. Logging must never wait.
            return false;
        }

        // If full, drop the oldest record (advance head).
        if (count_ == SLOTS) {
            head_ = (head_ + 1) % SLOTS;
            --count_;
        }

        Slot& dst = slots_[tail_];
        dst.target = rec.target;
        dst.level = rec.level;
        dst.module = rec.module;
        dst.timestamp = rec.timestamp;

        uint16_t to_copy = rec.line_len;
        if (to_copy >= SLOT_BYTES) {
            to_copy = SLOT_BYTES - 1;
        }
        std::memcpy(dst.line, rec.line, to_copy);
        dst.line[to_copy] = '\0';
        dst.line_len = to_copy;

        tail_ = (tail_ + 1) % SLOTS;
        ++count_;

        xSemaphoreGive(mutex_);
        if (task_handle_ != nullptr) {
            xTaskNotifyGive(task_handle_);
        }
        return true;
    }

    void AsyncDispatcher::task_entry(void* param) {
        static_cast<AsyncDispatcher*>(param)->task_loop();
    }

    void AsyncDispatcher::task_loop() {
        while (running_) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

            while (running_) {
                Slot slot;
                bool got = false;
                if (xSemaphoreTake(mutex_, pdMS_TO_TICKS(100)) == pdTRUE) {
                    got = pop(slot);
                    xSemaphoreGive(mutex_);
                }
                if (!got) {
                    break;
                }

                Record rec{};
                rec.target = slot.target;
                rec.level = slot.level;
                rec.module = slot.module;
                rec.line = slot.line;
                rec.line_len = slot.line_len;
                rec.timestamp = slot.timestamp;

                if (handler_ != nullptr) {
                    handler_(rec, ctx_);
                }
            }
        }

        task_handle_ = nullptr;
        vTaskDelete(nullptr);
    }

#else  // ---- Host fallback (tests, dev computers) ---------------------------

    bool AsyncDispatcher::start(Handler handler, void* ctx, const char* /*task_name*/,
                                uint32_t /*stack*/, uint8_t /*pri*/, int /*core*/) {
        if (handler == nullptr) {
            return false;
        }
        handler_ = handler;
        ctx_ = ctx;
        running_ = true;
        return true;
    }

    void AsyncDispatcher::stop() {
        running_ = false;
        handler_ = nullptr;
        ctx_ = nullptr;
    }

    bool AsyncDispatcher::push(const Record& rec) {
        if (!running_ || handler_ == nullptr) {
            return false;
        }
        // Synchronous fallback — call the handler directly so test logic stays
        // deterministic without spawning a thread.
        Slot tmp{};
        tmp.target = rec.target;
        tmp.level = rec.level;
        tmp.module = rec.module;
        tmp.timestamp = rec.timestamp;
        uint16_t to_copy = rec.line_len;
        if (to_copy >= SLOT_BYTES) {
            to_copy = SLOT_BYTES - 1;
        }
        std::memcpy(tmp.line, rec.line, to_copy);
        tmp.line[to_copy] = '\0';
        tmp.line_len = to_copy;

        Record forward{};
        forward.target = tmp.target;
        forward.level = tmp.level;
        forward.module = tmp.module;
        forward.line = tmp.line;
        forward.line_len = tmp.line_len;
        forward.timestamp = tmp.timestamp;

        handler_(forward, ctx_);
        return true;
    }

#endif  // ESP_PLATFORM

}  // namespace emblogx
