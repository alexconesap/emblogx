// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include "sd_sink.h"

#if EMBLOGX_ENABLE_SINK_SD

#include <cstdio>
#include <cstring>

namespace emblogx {

    // Minimum free space threshold (10%). Below this, old journals are deleted.
    static constexpr uint8_t FREE_SPACE_THRESHOLD_PCT = 10;

    // Maximum journal files to keep. Safety limit to avoid scanning thousands
    // of entries. Oldest files beyond this count are deleted regardless of space.
    static constexpr int MAX_JOURNAL_FILES = 100;

    // --- Journal helpers -------------------------------------------------------

    // Context for list_dir callback that finds the highest sequence number.
    struct SeqScanCtx {
            const char* prefix;
            int prefix_len;
            int max_seq;
    };

    static bool seq_scan_cb(const char* path, void* ctx) {
        auto* c = static_cast<SeqScanCtx*>(ctx);
        const char* name = strrchr(path, '/');
        name = (name != nullptr) ? name + 1 : path;

        if (strncmp(name, c->prefix, c->prefix_len) != 0) {
            return true;
        }
        if (name[c->prefix_len] != '_') {
            return true;
        }
        int seq = 0;
        const char* p = name + c->prefix_len + 1;
        while (*p >= '0' && *p <= '9') {
            seq = seq * 10 + (*p - '0');
            ++p;
        }
        if (seq > c->max_seq) {
            c->max_seq = seq;
        }
        return true;
    }

    int SdSink::find_next_sequence() const {
        SeqScanCtx ctx;
        ctx.prefix = prefix_;
        ctx.prefix_len = static_cast<int>(strlen(prefix_));
        ctx.max_seq = 0;
        fs_.list_dir(dir_, seq_scan_cb, &ctx);
        return ctx.max_seq + 1;
    }

    // Context for collecting journal file paths for cleanup (sorted by name).
    struct CleanupCtx {
            const char* prefix;
            int prefix_len;
            char paths[MAX_JOURNAL_FILES][64];
            int count;
    };

    static bool cleanup_scan_cb(const char* path, void* ctx) {
        auto* c = static_cast<CleanupCtx*>(ctx);
        const char* name = strrchr(path, '/');
        name = (name != nullptr) ? name + 1 : path;

        if (strncmp(name, c->prefix, c->prefix_len) != 0) {
            return true;
        }
        if (name[c->prefix_len] != '_') {
            return true;
        }
        if (c->count < MAX_JOURNAL_FILES) {
            strncpy(c->paths[c->count], path, 63);
            c->paths[c->count][63] = '\0';
            ++c->count;
        }
        return true;
    }

    // Insertion sort — alphabetical = chronological (zero-padded sequence numbers).
    static void sort_paths(char paths[][64], int count) {
        for (int i = 1; i < count; ++i) {
            char tmp[64];
            memcpy(tmp, paths[i], 64);
            int j = i - 1;
            while (j >= 0 && strcmp(paths[j], tmp) > 0) {
                memcpy(paths[j + 1], paths[j], 64);
                --j;
            }
            memcpy(paths[j + 1], tmp, 64);
        }
    }

    void SdSink::cleanup_old_journals() {
        ::ungula::sd::SpaceInfo space;
        if (!fs_.free_space(space) || space.total_bytes == 0) {
            return;
        }

        uint8_t free_pct = static_cast<uint8_t>((space.free_bytes * 100) / space.total_bytes);
        if (free_pct >= FREE_SPACE_THRESHOLD_PCT) {
            return;
        }

        static CleanupCtx ctx;  // static — only one SdSink per project
        ctx.prefix = prefix_;
        ctx.prefix_len = static_cast<int>(strlen(prefix_));
        ctx.count = 0;
        fs_.list_dir(dir_, cleanup_scan_cb, &ctx);

        if (ctx.count == 0) {
            return;
        }

        sort_paths(ctx.paths, ctx.count);

        for (int i = 0; i < ctx.count - 1; ++i) {  // never delete the newest
            fs_.remove(ctx.paths[i]);

            if (fs_.free_space(space) && space.total_bytes > 0) {
                free_pct = static_cast<uint8_t>((space.free_bytes * 100) / space.total_bytes);
                if (free_pct >= FREE_SPACE_THRESHOLD_PCT) {
                    break;
                }
            }
        }
    }

    // --- Sink interface --------------------------------------------------------

    bool SdSink::begin() {
        // Resolve the file path
        if (is_journal_mode()) {
            if (dir_ == nullptr || dir_[0] == '\0' || prefix_ == nullptr || prefix_[0] == '\0') {
                return false;
            }
            int seq = find_next_sequence();
            snprintf(path_buf_, PATH_BUF_SIZE, "%s/%s_%05d.log", dir_, prefix_, seq);
            path_ = path_buf_;
            cleanup_old_journals();
        } else {
            if (path_ == nullptr || path_[0] == '\0') {
                return false;
            }
        }

        // Open the file NOW, blocking. If this fails, the sink is disabled
        // and the host sees it immediately via is_sink_enabled().
        if (!fs_.is_mounted()) {
            return false;
        }
        file_ = fs_.open(path_, ::ungula::sd::OpenMode::AppendBinary);
        if (file_ == nullptr) {
            return false;
        }

        // Write a session marker and flush immediately. This forces the FAT
        // directory entry to be committed to the physical SD card — without an
        // explicit write+fsync the entry can remain in the FatFs sector cache
        // and disappear if no audit records arrive before the next power cycle.
        static constexpr const char SESSION_MARKER[] = "--- session start ---\n";
        file_->write(SESSION_MARKER, sizeof(SESSION_MARKER) - 1);
        file_->flush();

        // Start the async worker for record dispatch.
        // Stack 8192: SD I/O goes through VFS → FatFs → SPI driver, which
        // needs more stack than a typical task. 4096 can overflow silently.
        if (!dispatcher_.start(&SdSink::on_record, this, "emblogx_sd",
                               /*stack*/ 8192, /*pri*/ 1, /*core*/ 1)) {
            file_->close();
            delete file_;
            file_ = nullptr;
            return false;
        }

        return true;
    }

    void SdSink::write(const Record& rec) {
        dispatcher_.push(rec);
    }

    void SdSink::flush() {
        // No-op. on_record() flushes after every record (audit durability).
        // Calling flush() from another task would race with the worker.
    }

    void SdSink::on_record(const Record& rec, void* ctx) {
        auto* self = static_cast<SdSink*>(ctx);
        if (self->file_ == nullptr) {
            return;
        }

        self->file_->write(self->effective_line(rec), self->effective_line_len(rec));
        const char newline = '\n';
        self->file_->write(&newline, 1);
        // Flush every record — fflush + fsync pushes through FAT block cache
        // to physical SD card. Audit durability over throughput.
        self->file_->flush();
    }

}  // namespace emblogx

#endif  // EMBLOGX_ENABLE_SINK_SD
