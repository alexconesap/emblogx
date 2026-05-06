// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// SD card sink — asynchronous, capability AUDIT.
//
// Two usage modes:
//
//   1. Single-file (original): pass a full file path at construction.
//      All records are appended to that one file forever.
//
//        SdSink sink(fs, "/sdcard/audit.log");
//
//   2. Journal (rolling): pass a directory and a prefix. Each boot session
//      creates a new file named {prefix}_{seq}.log where seq is the next
//      available sequence number. At begin(), if free space drops below 10%,
//      the oldest journal files are deleted until space is recovered.
//
//        SdSink sink(fs, "/sdcard", "audit");
//
// Gated by EMBLOGX_ENABLE_SINK_SD — defaults OFF. Projects without SD
// hardware should leave it off so the file-IO code never gets linked in.

#ifndef EMBLOGX_ENABLE_SINK_SD
#define EMBLOGX_ENABLE_SINK_SD 0
#endif

#if EMBLOGX_ENABLE_SINK_SD

#include "../async_dispatcher.h"
#include "../config.h"
#include "i_sink.h"

#include <ungula/sd/i_file.h>
#include <ungula/sd/i_filesystem.h>

namespace emblogx {

    class SdSink : public ISink {
        public:
            // Single-file mode. fs and path are borrowed — both must outlive the sink.
            SdSink(::ungula::sd::IFileSystem& fs, const char* path)
                : fs_(fs), path_(path), dir_(nullptr), prefix_(nullptr) {}

            // Journal mode. dir and prefix are borrowed — must outlive the sink.
            // Creates files as {dir}/{prefix}_{seq}.log, one per boot session.
            SdSink(::ungula::sd::IFileSystem& fs, const char* dir, const char* prefix)
                : fs_(fs), path_(nullptr), dir_(dir), prefix_(prefix) {}

            uint8_t capabilities() const override {
                return Capability::AUDIT;
            }
            Mode mode() const override {
                return Mode::Async;
            }

            bool begin() override;
            void write(const Record& rec) override;
            void flush() override;
            const char* name() const override {
                return "sd";
            }

        private:
            ::ungula::sd::IFileSystem& fs_;
            const char* path_;    // single-file mode: full path, journal mode: nullptr
            const char* dir_;     // journal mode: directory path
            const char* prefix_;  // journal mode: filename prefix
            AsyncDispatcher dispatcher_;
            ::ungula::sd::IFile* file_ = nullptr;

            // Journal-mode generated path (static buffer, one SdSink per project)
            static constexpr int PATH_BUF_SIZE = 64;
            char path_buf_[PATH_BUF_SIZE] = {};

            bool is_journal_mode() const {
                return dir_ != nullptr && prefix_ != nullptr;
            }

            // Journal helpers
            int find_next_sequence() const;
            void cleanup_old_journals();

            static void on_record(const Record& rec, void* ctx);
    };

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_SD
