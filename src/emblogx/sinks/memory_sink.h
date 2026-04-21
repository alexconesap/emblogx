// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// Memory sink — synchronous, capability LOG | STATUS.
//
// Stores log lines in a single contiguous ring buffer with a stable layout
// (LogTraceBuffer) so external debuggers can scan device RAM for the
// "LOGBUF_V1" magic and dump the recent history without any agent on the
// device. The on-device UI can also read it back via memory_sink_read().
//
// Buffer placement:
//   The buffer lives in a dedicated linker section ".logbuf" so users that
//   need a fixed RAM address can place it deterministically. We mark it as
//   used so the linker keeps it even when no symbol references it directly.
//
// Storage policy:
//   - circular overwrite (oldest bytes lost first)
//   - records stored as raw text bytes followed by '\n'
//   - no per-record metadata in the buffer (timestamps are part of the line
//     prefix written by logger_core.cpp)

#include "../config.h"
#include "i_sink.h"

#ifndef EMBLOGX_ENABLE_SINK_MEMORY
#define EMBLOGX_ENABLE_SINK_MEMORY 0
#endif

#if EMBLOGX_ENABLE_SINK_MEMORY

#include <cstdint>

namespace emblogx {

    // Public layout of the memory ring. POD only — no pointers, no virtuals.
    // Stable across firmware versions; new fields must be appended after the
    // existing ones (and version bumped) so external tooling does not break.
    struct LogTraceBuffer {
            char magic[10];                // "LOGBUF_V1"
            uint32_t version;              // 1
            uint32_t buffer_size;          // EMBLOGX_MEMSINK_SIZE
            uint32_t write_index;          // next write position in data[]
            uint32_t total_bytes_written;  // monotonic, never resets
            char data[EMBLOGX_MEMSINK_SIZE];
    };

    // Access the underlying buffer struct (for debugger / external tooling).
    // The pointer is stable for the entire process lifetime.
    const LogTraceBuffer* memory_sink_buffer();

    // Read up to max_bytes from the ring into dst, starting from *cursor.
    // The cursor is a monotonic byte offset (matches total_bytes_written) so
    // callers can do incremental reads across calls. Returns the number of
    // bytes copied; on return *cursor is advanced by that amount.
    //
    // If the cursor lags more than buffer_size bytes behind the writer, the
    // oldest readable position is silently used and *cursor jumps forward.
    //
    // Concurrency: the writer indices are updated under a portMUX critical
    // section, and this reader takes a snapshot of them under the same lock,
    // so the (write_index, total_bytes_written) pair is always consistent.
    // The data byte copy itself runs without the lock held — under heavy
    // concurrent writing the reader may briefly observe one or two torn
    // bytes, never a missing or duplicated line. Non-blocking, no allocation,
    // safe to call from any task.
    size_t memory_sink_read(char* dst, size_t max_bytes, uint32_t* cursor);

    // Reset the cursor to the start of the readable history.
    void memory_sink_seek_oldest(uint32_t* cursor);

    // Reset the cursor to the writer head (i.e. only show new lines).
    void memory_sink_seek_newest(uint32_t* cursor);

    class MemorySink : public ISink {
        public:
            uint8_t capabilities() const override {
                return Capability::LOG | Capability::STATUS;
            }
            Mode mode() const override {
                return Mode::Sync;
            }

            bool begin() override;
            void write(const Record& rec) override;
            const char* name() const override {
                return "memory";
            }
    };

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_MEMORY
