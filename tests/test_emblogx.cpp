// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "emblogx/logger.h"
#include "emblogx/sinks/console_sink.h"
#include "emblogx/sinks/memory_sink.h"

// ----------------------------------------------------------------------------
// Test sink — captures every record it receives so tests can assert on the
// routing decisions made by the core.
// ----------------------------------------------------------------------------

namespace {

    struct CapturedRecord {
            uint8_t target;
            ::emblogx::Level level;
            std::string module;
            std::string line;
    };

    class CapturingSink : public ::emblogx::ISink {
        public:
            uint8_t caps_ = ::emblogx::Capability::ALL;
            ::emblogx::Mode mode_ = ::emblogx::Mode::Sync;
            std::vector<CapturedRecord> seen;

            uint8_t capabilities() const override {
                return caps_;
            }
            ::emblogx::Mode mode() const override {
                return mode_;
            }

            void write(const ::emblogx::Record& rec) override {
                seen.push_back({rec.target, rec.level,
                                rec.module ? std::string(rec.module) : std::string(),
                                std::string(rec.line, rec.line_len)});
            }
    };

    // Sink storage — fixed-size array so pointers stay stable across the
    // entire test process (the registry holds raw pointers).
    // The library is built with file-static state so we can't truly reset
    // it; capturing sinks accumulate across tests within one process and
    // assertions check ".back()".
    constexpr size_t MAX_TEST_SINKS = 16;
    CapturingSink g_test_sinks[MAX_TEST_SINKS];
    size_t g_test_sink_count = 0;

    CapturingSink* fresh_sink(uint8_t caps = ::emblogx::Capability::ALL,
                              ::emblogx::Mode mode = ::emblogx::Mode::Sync) {
        if (g_test_sink_count >= MAX_TEST_SINKS) {
            ADD_FAILURE() << "test sink pool exhausted";
            return nullptr;
        }
        auto* sink = &g_test_sinks[g_test_sink_count++];
        sink->caps_ = caps;
        sink->mode_ = mode;
        sink->seen.clear();
        ::emblogx::register_sink(sink);
        ::emblogx::init();
        return sink;
    }

}  // namespace

// ----------------------------------------------------------------------------
// Routing
// ----------------------------------------------------------------------------

TEST(Routing, LogTargetReachesLogSinkOnly) {
    auto* log_sink = fresh_sink(::emblogx::Capability::LOG);
    auto* audit_sink = fresh_sink(::emblogx::Capability::AUDIT);

    log_info("hello %d", 42);

    ASSERT_FALSE(log_sink->seen.empty());
    EXPECT_TRUE(log_sink->seen.back().line.find("hello 42") != std::string::npos);
    EXPECT_EQ(log_sink->seen.back().level, ::emblogx::Level::Info);

    // Audit sink must NOT have received the LOG-only record.
    size_t before = audit_sink->seen.size();
    log_info("only log");
    EXPECT_EQ(audit_sink->seen.size(), before);
}

TEST(Routing, LogAuditTargetReachesBoth) {
    auto* log_sink = fresh_sink(::emblogx::Capability::LOG);
    auto* audit_sink = fresh_sink(::emblogx::Capability::AUDIT);

    size_t log_before = log_sink->seen.size();
    size_t audit_before = audit_sink->seen.size();

    log_audit_info("EMERGENCY_STOP");

    EXPECT_EQ(log_sink->seen.size(), log_before + 1);
    EXPECT_EQ(audit_sink->seen.size(), audit_before + 1);
    EXPECT_TRUE(log_sink->seen.back().line.find("EMERGENCY_STOP") != std::string::npos);
}

TEST(Routing, StatusTargetReachesStatusSinkOnly) {
    auto* status_sink = fresh_sink(::emblogx::Capability::STATUS);
    auto* log_sink = fresh_sink(::emblogx::Capability::LOG);

    size_t log_before = log_sink->seen.size();
    size_t status_before = status_sink->seen.size();

    status_warn_m("wifi", "rssi=%d", -72);

    EXPECT_EQ(status_sink->seen.size(), status_before + 1);
    EXPECT_EQ(log_sink->seen.size(), log_before);
    EXPECT_EQ(status_sink->seen.back().module, "wifi");
}

// ----------------------------------------------------------------------------
// Level filter
// ----------------------------------------------------------------------------

TEST(Level, GlobalLevelFiltersBelow) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    ::emblogx::set_global_level(::emblogx::Level::Warn);

    size_t before = sink->seen.size();
    log_info("dropped");
    log_warn("kept");

    EXPECT_EQ(sink->seen.size(), before + 1);
    EXPECT_TRUE(sink->seen.back().line.find("kept") != std::string::npos);

    ::emblogx::set_global_level(::emblogx::Level::Info);  // restore
}

TEST(Level, PerModuleLevelOverridesGlobal) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    ::emblogx::set_global_level(::emblogx::Level::Error);
    ::emblogx::set_module_level("noisy", ::emblogx::Level::Debug);

    size_t before = sink->seen.size();
    log_info_m("noisy", "should pass");
    log_info_m("quiet", "should drop");

    EXPECT_EQ(sink->seen.size(), before + 1);
    EXPECT_EQ(sink->seen.back().module, "noisy");

    ::emblogx::set_global_level(::emblogx::Level::Info);  // restore
}

// ----------------------------------------------------------------------------
// Memory sink (RTT-style ring buffer)
// ----------------------------------------------------------------------------

TEST(MemorySink, RoundTripsViaReader) {
    static ::emblogx::MemorySink memory;
    ::emblogx::register_sink(&memory);
    ::emblogx::init();

    // Snap to the writer head — only count NEW writes.
    uint32_t cursor = 0;
    ::emblogx::memory_sink_seek_newest(&cursor);

    log_info("ring write %d", 7);

    char buf[256] = {0};
    size_t got = ::emblogx::memory_sink_read(buf, sizeof(buf) - 1, &cursor);
    ASSERT_GT(got, 0u);
    buf[got] = '\0';
    EXPECT_NE(std::string(buf).find("ring write 7"), std::string::npos);
}

TEST(MemorySink, MagicAndVersionStable) {
    const auto* meta = ::emblogx::memory_sink_buffer();
    ASSERT_NE(meta, nullptr);
    EXPECT_EQ(0, std::memcmp(meta->magic, "LOGBUF_V1", 9));
    EXPECT_EQ(meta->magic[9], '\0');
    EXPECT_EQ(meta->version, 1u);
    EXPECT_GT(meta->buffer_size, 0u);
}

// ----------------------------------------------------------------------------
// Truncation
// ----------------------------------------------------------------------------

TEST(Format, LinesLongerThanLineMaxAreTruncatedNotCorrupted) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);

    // Generate a body bigger than EMBLOGX_LINE_MAX so the snprintf is forced
    // to truncate. The router must keep the line within bounds and never
    // overflow the stack buffer.
    char big[EMBLOGX_LINE_MAX * 2];
    std::memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';

    log_info("%s", big);

    ASSERT_FALSE(sink->seen.empty());
    EXPECT_LE(sink->seen.back().line.size(), static_cast<size_t>(EMBLOGX_LINE_MAX));
}

// ----------------------------------------------------------------------------
// Structured event helpers
// ----------------------------------------------------------------------------

TEST(StructuredEvents, AuditEventCarriesCode) {
    auto* sink = fresh_sink(::emblogx::Capability::AUDIT);

    audit_event(101, "cycle", "PROGRAM_SELECT idx=%d", 5);

    ASSERT_FALSE(sink->seen.empty());
    EXPECT_NE(sink->seen.back().line.find("code=101"), std::string::npos);
    EXPECT_NE(sink->seen.back().line.find("PROGRAM_SELECT idx=5"), std::string::npos);
}

TEST(StructuredEvents, StatusEventCarriesCode) {
    auto* sink = fresh_sink(::emblogx::Capability::STATUS);

    status_event(7, "wifi", "state=disconnected");

    ASSERT_FALSE(sink->seen.empty());
    EXPECT_NE(sink->seen.back().line.find("code=7"), std::string::npos);
}

// ----------------------------------------------------------------------------
// Sink toggle
// ----------------------------------------------------------------------------

TEST(SinkToggle, DisabledSinksDontReceive) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    uint8_t idx = ::emblogx::sink_count() - 1;

    size_t before = sink->seen.size();
    ::emblogx::set_sink_enabled(idx, false);
    log_info("dropped");
    EXPECT_EQ(sink->seen.size(), before);

    ::emblogx::set_sink_enabled(idx, true);
    log_info("kept");
    EXPECT_EQ(sink->seen.size(), before + 1);
}

// ----------------------------------------------------------------------------
// SD sink (with mock filesystem)
// ----------------------------------------------------------------------------
#if EMBLOGX_ENABLE_SINK_SD
#include "emblogx/sinks/sd_sink.h"
#include "sd/i_file.h"
#include "sd/i_filesystem.h"

namespace {

    class MockSdFile : public ::ungula::sd::IFile {
        public:
            std::string data;
            int flush_count = 0;

            size_t write(const void* buf, size_t len) override {
                data.append(static_cast<const char*>(buf), len);
                return len;
            }
            size_t read(void* /*buf*/, size_t /*len*/) override {
                return 0;
            }
            bool flush() override {
                ++flush_count;
                return true;
            }
            void close() override {}
    };

    class MockSdFs : public ::ungula::sd::IFileSystem {
        public:
            bool mounted = false;
            MockSdFile* last_file = nullptr;

            bool mount() override {
                mounted = true;
                return true;
            }
            void unmount() override {
                mounted = false;
            }
            bool is_mounted() const override {
                return mounted;
            }

            ::ungula::sd::IFile* open(const char* /*path*/,
                                      ::ungula::sd::OpenMode /*mode*/) override {
                if (!mounted) {
                    return nullptr;
                }
                last_file = new MockSdFile();
                return last_file;
            }

            bool free_space(::ungula::sd::SpaceInfo& /*out*/) const override {
                return false;
            }
            bool remove(const char* /*path*/) override {
                return false;
            }
            int list_dir(const char* /*dir_path*/, ::ungula::sd::DirEntryCallback /*cb*/,
                         void* /*ctx*/) override {
                return 0;
            }
    };

}  // namespace

TEST(SdSink, AuditRecordsReachMockFile) {
    static MockSdFs fs;
    fs.mount();

    static ::emblogx::SdSink sd_sink(fs, "/sdcard/audit.log");
    ::emblogx::register_sink(&sd_sink);
    ::emblogx::init();

    // SdSink only accepts AUDIT target. Use the audit macro.
    audit_info("SD_TEST_EVENT");

    // On the host build the async dispatcher is synchronous, so the mock
    // file should already contain the record.
    ASSERT_NE(fs.last_file, nullptr);
    EXPECT_NE(fs.last_file->data.find("SD_TEST_EVENT"), std::string::npos);
    EXPECT_GE(fs.last_file->flush_count, 1);
}

TEST(SdSink, LogRecordsDontReachSdSink) {
    // LOG-only records must NOT reach the SdSink (AUDIT capability only).
    static MockSdFs fs2;
    fs2.mount();

    static ::emblogx::SdSink sd_sink2(fs2, "/sdcard/audit.log");
    ::emblogx::register_sink(&sd_sink2);
    ::emblogx::init();

    // begin() opens the file and writes a session marker — capture the
    // byte count before sending a non-audit record.
    ASSERT_NE(fs2.last_file, nullptr);
    size_t bytes_after_begin = fs2.last_file->data.size();

    log_info("not an audit record");

    // The file should not have grown — LOG records don't reach an AUDIT-only sink.
    EXPECT_EQ(fs2.last_file->data.size(), bytes_after_begin);
}
#endif  // EMBLOGX_ENABLE_SINK_SD

// ----------------------------------------------------------------------------
// Async dispatcher (host fallback)
//
// On the host build the dispatcher pushes synchronously into the registered
// handler — no FreeRTOS task. We can still cover the contract bits that the
// ESP-IDF path also has to honour: handler invocation, slot copy, ordering,
// the 64-bit timestamp pass-through and start/stop lifecycle.
// ----------------------------------------------------------------------------
#include "emblogx/async_dispatcher.h"

namespace {
    struct AsyncCapture {
            std::vector<std::string> lines;
            std::vector<uint64_t> timestamps;
            std::vector<uint8_t> targets;
    };

    void async_handler(const ::emblogx::Record& rec, void* ctx) {
        auto* cap = static_cast<AsyncCapture*>(ctx);
        cap->lines.emplace_back(rec.line, rec.line_len);
        cap->timestamps.push_back(rec.timestamp);
        cap->targets.push_back(rec.target);
    }
}  // namespace

TEST(AsyncDispatcher, HandlerSeesEveryPushedRecordInOrder) {
    ::emblogx::AsyncDispatcher dispatcher;
    AsyncCapture cap;
    ASSERT_TRUE(dispatcher.start(async_handler, &cap, "test"));

    for (int i = 0; i < 5; ++i) {
        ::emblogx::Record rec{};
        std::string body = "msg-" + std::to_string(i);
        rec.target = ::emblogx::Target::LOG;
        rec.level = ::emblogx::Level::Info;
        rec.module = "test";
        rec.line = body.c_str();
        rec.line_len = static_cast<uint16_t>(body.size());
        rec.timestamp = 1000ULL + static_cast<uint64_t>(i);
        EXPECT_TRUE(dispatcher.push(rec));
    }

    ASSERT_EQ(cap.lines.size(), 5u);
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(cap.lines[i], "msg-" + std::to_string(i));
        EXPECT_EQ(cap.timestamps[i], 1000ULL + static_cast<uint64_t>(i));
        EXPECT_EQ(cap.targets[i], ::emblogx::Target::LOG);
    }

    dispatcher.stop();
}

TEST(AsyncDispatcher, PushBeforeStartIsRejected) {
    ::emblogx::AsyncDispatcher dispatcher;
    ::emblogx::Record rec{};
    rec.target = ::emblogx::Target::LOG;
    rec.level = ::emblogx::Level::Info;
    rec.line = "ignored";
    rec.line_len = 7;
    EXPECT_FALSE(dispatcher.push(rec));
}

TEST(AsyncDispatcher, TimestampSurvivesAbove32BitWrap) {
    // The whole point of the uint64 Slot::timestamp fix — make sure a value
    // that does NOT fit in 32 bits comes out the other side intact.
    ::emblogx::AsyncDispatcher dispatcher;
    AsyncCapture cap;
    ASSERT_TRUE(dispatcher.start(async_handler, &cap, "test_64"));

    constexpr uint64_t kBig = 0x1'0000'1234ULL;  // > 2^32

    ::emblogx::Record rec{};
    rec.target = ::emblogx::Target::LOG;
    rec.level = ::emblogx::Level::Info;
    rec.module = "t";
    rec.line = "wide";
    rec.line_len = 4;
    rec.timestamp = kBig;
    ASSERT_TRUE(dispatcher.push(rec));

    ASSERT_EQ(cap.timestamps.size(), 1u);
    EXPECT_EQ(cap.timestamps[0], kBig);

    dispatcher.stop();
}

// ----------------------------------------------------------------------------
// Rate limiting
// ----------------------------------------------------------------------------

TEST(RateLimit, DropsRepeatedCallsWithinInterval) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);  // 10 s — effectively drops repeats in-test

    for (int i = 0; i < 5; ++i) {
        log_info("rate-limited %d", i);
    }

    EXPECT_EQ(sink->seen.size(), baseline + 1);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ErrorLevelAlwaysGoesThrough) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);

    for (int i = 0; i < 3; ++i) {
        log_error("should always show %d", i);
    }

    EXPECT_EQ(sink->seen.size(), baseline + 3);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ForceVariantsBypassTheLimiter) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);

    log_info_force("forced one %d", 1);
    log_info_force("forced one %d", 2);
    log_info_force_m("setup", "forced two %d", 3);

    EXPECT_EQ(sink->seen.size(), baseline + 3);
    EXPECT_EQ(sink->seen.back().module, "setup");
    EXPECT_NE(sink->seen.back().line.find("forced two 3"), std::string::npos);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, DifferentCallSitesDontThrottleEachOther) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);

    log_info("site A %d", 1);
    log_info("site B %d", 2);
    log_warn("site C %d", 3);

    EXPECT_EQ(sink->seen.size(), baseline + 3);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ReopensAfterIntervalElapses) {
    // After the configured interval has passed, the same call site must be
    // allowed through again. Uses a short interval + real sleep so the
    // monotonic clock used by the core actually advances.
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(50);

    log_info("reopen test %d", 1);  // 1st: lands
    log_info("reopen test %d", 2);  // 2nd: dropped (within window)
    EXPECT_EQ(sink->seen.size(), baseline + 1);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    log_info("reopen test %d", 3);  // window expired: lands
    log_info("reopen test %d", 4);  // immediately after: dropped
    EXPECT_EQ(sink->seen.size(), baseline + 2);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    log_info("reopen test %d", 5);  // window expired again: lands
    EXPECT_EQ(sink->seen.size(), baseline + 3);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ChangingIntervalAtRuntimeTakesEffect) {
    // Lowering the interval while the limiter is running must let calls
    // through that would have been blocked under the previous, longer
    // interval — and vice versa.
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);
    log_info("interval change %d", 1);  // lands
    log_info("interval change %d", 2);  // dropped
    EXPECT_EQ(sink->seen.size(), baseline + 1);

    // Drop the limit to a very small value, sleep just past it, and the
    // same call site should pass again.
    log_set_rate_limit_ms(20);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    log_info("interval change %d", 3);  // lands
    EXPECT_EQ(sink->seen.size(), baseline + 2);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, TableEvictionStillThrottlesActiveSites) {
    // The rate table is fixed-size (16 slots). Once it is full the oldest
    // entry is evicted to make room for new sites. The freshly-eviced site
    // becomes "unknown" and a single call from it goes through, but a site
    // that has been kept warm by recent calls must still be throttled.
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);

    // Warm one site we want to stay throttled.
    log_info("warm site");                          // lands
    EXPECT_EQ(sink->seen.size(), baseline + 1);

    // Flood the table with 30 distinct fmt pointers — each is a separate
    // string literal, so each gets its own slot (and forces evictions).
    log_info("flood %d", 1);
    log_info("flood %d %d", 1, 2);
    log_info("flood %d %d %d", 1, 2, 3);
    log_info("flood A");
    log_info("flood B");
    log_info("flood C");
    log_info("flood D");
    log_info("flood E");
    log_info("flood F");
    log_info("flood G");
    log_info("flood H");
    log_info("flood I");
    log_info("flood J");
    log_info("flood K");
    log_info("flood L");
    log_info("flood M");
    log_info("flood N");
    log_info("flood O");
    log_info("flood P");
    log_info("flood Q");
    log_info("flood R");
    // 21 distinct sites pushed through a 16-slot table. Each is a first
    // sighting so each lands once.
    EXPECT_EQ(sink->seen.size(), baseline + 1 + 21);

    // The "warm site" entry was the oldest by timestamp and got evicted
    // somewhere along the way. Re-firing it now is treated as a new site
    // and goes through once...
    log_info("warm site");
    EXPECT_EQ(sink->seen.size(), baseline + 1 + 21 + 1);

    // ...but immediately re-firing the same site must be throttled again.
    log_info("warm site");
    log_info("warm site");
    EXPECT_EQ(sink->seen.size(), baseline + 1 + 21 + 1);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ModuleVariantsAreThrottledByFmtNotByModule) {
    // The limiter keys on the fmt pointer, not the module — so the same
    // fmt called from two different modules in a row hits the same slot
    // and only the first one lands. This is intentional: a `loop()` that
    // logs the same line for two subsystems still floods the console.
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(10'000);

    log_info_m("modA", "shared %d", 1);  // lands
    log_info_m("modB", "shared %d", 2);  // same fmt pointer: dropped
    EXPECT_EQ(sink->seen.size(), baseline + 1);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ForceVariantDoesNotResetTheRegularSlot) {
    // A `_force` call must not refresh the limiter's "last seen" timestamp
    // for the regular call site, because that would let a single force
    // call extend the throttle window indefinitely. Verify that after a
    // burst of forced calls, a normal call from the same fmt still lands
    // once the original interval has passed.
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(50);

    log_info("mixed %d", 0);          // lands, opens window
    log_info_force("mixed %d", 1);    // forced: lands, must NOT touch slot
    log_info_force("mixed %d", 2);    // forced: lands
    log_info("mixed %d", 3);          // within window: dropped
    EXPECT_EQ(sink->seen.size(), baseline + 3);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    log_info("mixed %d", 4);          // window expired: lands
    EXPECT_EQ(sink->seen.size(), baseline + 4);

    log_set_rate_limit_ms(0);
}

TEST(RateLimit, ZeroDisablesLimiter) {
    auto* sink = fresh_sink(::emblogx::Capability::LOG);
    const size_t baseline = sink->seen.size();

    log_set_rate_limit_ms(0);
    for (int i = 0; i < 4; ++i) {
        log_info("no limit %d", i);
    }

    EXPECT_EQ(sink->seen.size(), baseline + 4);
}
