// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "memory_sink.h"

#if EMBLOGX_ENABLE_SINK_MEMORY

#include <cstring>

// Concurrency model:
//   The memory sink is sync from the producer's perspective: write() runs on
//   whichever task is logging. Readers (UI viewer, debugger snapshot helper)
//   may run on a different task. The two indices `write_index` and
//   `total_bytes_written` MUST be updated together — a reader that sees one
//   updated and the other not yet would compute an inconsistent slice.
//
//   On ESP-IDF we wrap the writer's index update and the reader's snapshot
//   in a portMUX critical section. The `data[]` byte copy itself is left
//   outside the critical section to keep the locked region short — a reader
//   may briefly see a torn data byte under load, but that costs at most one
//   garbled character per concurrent read, never a missing or duplicated
//   line.
//
//   On host builds (unit tests, single-threaded) the lock is a no-op.
#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#define EMBLOGX_MEMSINK_LOCK_DECL static portMUX_TYPE g_lock = portMUX_INITIALIZER_UNLOCKED
#define EMBLOGX_MEMSINK_LOCK() portENTER_CRITICAL(&g_lock)
#define EMBLOGX_MEMSINK_UNLOCK() portEXIT_CRITICAL(&g_lock)
#else
#define EMBLOGX_MEMSINK_LOCK_DECL static int g_lock = 0
#define EMBLOGX_MEMSINK_LOCK() ((void)0)
#define EMBLOGX_MEMSINK_UNLOCK() ((void)0)
#endif

namespace emblogx {

    EMBLOGX_MEMSINK_LOCK_DECL;

    // Single global instance. By default it lives in normal .bss — the "used"
    // attribute prevents dead-code elimination, and external debuggers can
    // still find it by symbol name (`emblogx::g_log_buf`).
    //
    // Opt-in: define EMBLOGX_LOGBUF_SECTION at build time to place it in a
    // dedicated linker section (e.g. for fixed-RAM placement). This requires
    // the project linker script to declare the section, otherwise the linker
    // either drops it in flash (writes crash) or overlaps it with other data
    // (silent memory corruption) — which is why it's NOT the default.
    //
    // Example linker fragment:
    //   .logbuf : { KEEP(*(.logbuf)) } > RAM
#ifdef EMBLOGX_LOGBUF_SECTION
#define EMBLOGX_LOGBUF_ATTR __attribute__((used, section(".logbuf")))
#else
#define EMBLOGX_LOGBUF_ATTR __attribute__((used))
#endif

    EMBLOGX_LOGBUF_ATTR static LogTraceBuffer g_log_buf = {
            /* magic               */ {'L', 'O', 'G', 'B', 'U', 'F', '_', 'V', '1', '\0'},
            /* version             */ 1,
            /* buffer_size         */ EMBLOGX_MEMSINK_SIZE,
            /* write_index         */ 0,
            /* total_bytes_written */ 0,
            /* data                */ {0},
    };

    // ---- Internal helpers ---------------------------------------------------

    namespace {

        inline void put_byte(char chr) {
            // The byte copy is a single store of one byte and is already atomic;
            // only the index update needs the lock so the (write_index,
            // total_bytes_written) pair stays consistent for readers.
            g_log_buf.data[g_log_buf.write_index] = chr;
            EMBLOGX_MEMSINK_LOCK();
            g_log_buf.write_index = (g_log_buf.write_index + 1) % g_log_buf.buffer_size;
            ++g_log_buf.total_bytes_written;
            EMBLOGX_MEMSINK_UNLOCK();
        }

        void put_bytes(const char* src, size_t len) {
            // Hot path: bulk copy when the write window doesn't wrap.
            const uint32_t cap = g_log_buf.buffer_size;
            if (len >= cap) {
                // Source is bigger than the whole ring — only the tail will survive.
                src += (len - cap);
                len = cap;
            }
            uint32_t head = g_log_buf.write_index;
            uint32_t first = cap - head;
            if (len <= first) {
                std::memcpy(&g_log_buf.data[head], src, len);
            } else {
                std::memcpy(&g_log_buf.data[head], src, first);
                std::memcpy(&g_log_buf.data[0], src + first, len - first);
            }
            // Indices updated atomically together so readers always see a
            // consistent snapshot.
            EMBLOGX_MEMSINK_LOCK();
            g_log_buf.write_index = (head + len) % cap;
            g_log_buf.total_bytes_written += static_cast<uint32_t>(len);
            EMBLOGX_MEMSINK_UNLOCK();
        }

    }  // namespace

    // ---- Public reader API --------------------------------------------------

    const LogTraceBuffer* memory_sink_buffer() {
        return &g_log_buf;
    }

    size_t memory_sink_read(char* dst, size_t max_bytes, uint32_t* cursor) {
        if (dst == nullptr || cursor == nullptr || max_bytes == 0) {
            return 0;
        }
        // Snapshot the writer indices under the lock so we never compute the
        // read window from a torn (write_index, total_bytes_written) pair.
        const uint32_t cap = g_log_buf.buffer_size;
        uint32_t total;
        EMBLOGX_MEMSINK_LOCK();
        total = g_log_buf.total_bytes_written;
        EMBLOGX_MEMSINK_UNLOCK();

        // Oldest readable byte is total - cap (clamped at 0).
        const uint32_t oldest = (total > cap) ? (total - cap) : 0;
        if (*cursor < oldest) {
            *cursor = oldest;
        }
        if (*cursor >= total) {
            return 0;  // caller is up to date
        }

        uint32_t available = total - *cursor;
        if (available > max_bytes) {
            available = static_cast<uint32_t>(max_bytes);
        }

        // Map the cursor (absolute) to the ring position.
        uint32_t pos = (*cursor) % cap;
        uint32_t first = cap - pos;
        if (first >= available) {
            std::memcpy(dst, &g_log_buf.data[pos], available);
        } else {
            std::memcpy(dst, &g_log_buf.data[pos], first);
            std::memcpy(dst + first, &g_log_buf.data[0], available - first);
        }
        *cursor += available;
        return available;
    }

    void memory_sink_seek_oldest(uint32_t* cursor) {
        if (cursor == nullptr) {
            return;
        }
        const uint32_t cap = g_log_buf.buffer_size;
        EMBLOGX_MEMSINK_LOCK();
        const uint32_t total = g_log_buf.total_bytes_written;
        EMBLOGX_MEMSINK_UNLOCK();
        *cursor = (total > cap) ? (total - cap) : 0;
    }

    void memory_sink_seek_newest(uint32_t* cursor) {
        if (cursor == nullptr) {
            return;
        }
        EMBLOGX_MEMSINK_LOCK();
        *cursor = g_log_buf.total_bytes_written;
        EMBLOGX_MEMSINK_UNLOCK();
    }

    // ---- Sink ---------------------------------------------------------------

    bool MemorySink::begin() {
        return true;
    }

    void MemorySink::write(const Record& rec) {
        if (rec.line == nullptr || rec.line_len == 0) {
            return;
        }
        put_bytes(rec.line, rec.line_len);
        put_byte('\n');
    }

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_MEMORY
