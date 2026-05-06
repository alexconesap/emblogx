# EmblogX — embedded-first logging, audit and remote telemetry

> **High-performance embedded C++ libraries for ESP32, STM32 and other MCUs** — embedded-first logging with FDA-compliant audit router.

**Embedded-first** logging library for ESP32-class targets. One log call goes
to every place that needs it (serial console, RAM ring, HTTP server, MQTT
broker, SD card, ...) without the host project having to remember which sinks
are wired and which are not.

Designed from day one for three real-world embedded use cases:

- **Embedded systems** — zero heap, fixed-capacity buffers, constant-time
  routing, no `std::string` in hot paths, no recursion, no `new` after boot.
  Memory footprint and CPU cost are bounded at compile time and stay flat
  for the lifetime of the device. Tested on ESP32, ESP32-S3 and POSIX hosts.
- **Remote telemetry / cloud forwarding** — operational logs, structured
  status events and audit records can be forwarded to a remote endpoint
  (HTTP today, MQTT / AMQP / WebSocket / Kafka via your own sink) on a
  background worker task that never blocks the producer. The HTTP sink
  emits RFC 8259-compliant JSON envelopes ready to consume by any cloud
  log aggregator or device dashboard.
- **FDA / regulatory compliance** — first-class `AUDIT` target separate from
  the operational `LOG` target, append-only SD card sink for the legal
  record, append-only memory ring for the on-device viewer, single-call
  API that makes the "operator console *and* audit trail" pattern impossible
  to forget. Drop-in replacement for hand-rolled audit journals on med-tech,
  industrial-control and lab-instrument devices.

All declared sinks that are 'synchronous' are called immediately while those
declared 'asynchronous' are dispatched on a dedicated FreeRTOS worker task
with a static queue, so the producer task never blocks on flash, network
or filesystem I/O.

## Keywords

`embedded` `esp32` `esp-idf` `arduino` `freertos` `logging` `logger` `log`
`audit` `audit-trail` `fda` `fda-compliance` `medical-device` `iec-62304`
`telemetry` `remote-logging` `cloud-logging` `http` `mqtt` `dashboard`
`zero-heap` `no-malloc` `fixed-capacity` `deterministic` `real-time`
`rtt` `ring-buffer` `sink` `printf-style`

## In a nutshell

The simplest possible setup: drop this into a fresh `.ino` or ESP-IDF main file, compile and watch
your serial monitor.

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>

static emblogx::ConsoleSink console;

void setup() {
    emblogx::register_sink(&console);
    emblogx::init(); // This will internally initialize the engine, including the serial port

    log_info("Boot complete");
    log_info("Temperature: %d C", 42);
    log_warn("Battery low");
    log_error("Failed to mount SD: %d", -3);
}

void loop() {}
```

That is the whole API for the common case. `log_info`, `log_warn`, `log_error`
behave like a `printf`. The format string is the first argument, format
arguments follow, no module name needed.

## Rate limiting

`log_set_rate_limit_ms(ms)` — throttles non-error messages. Errors always go through. Really useful to avoid injecting dozens of messages every second to the Serial port or to any external/slow sink.

```cpp
log_set_rate_limit_ms(1000);

void loop() {
  // Your stuff...

  log_info("This will log ONLY 1 time per second");
}
```

The check is keyed by the format-string pointer (format strings are literals), so every call site is tracked independently. Two different `log_info(...)` calls in the same loop do not throttle each other.

When a specific line must never be dropped — boot banners, one-shot state transitions, startup messages — use the `_force` / `_force_m` variants:

```cpp
log_info_force(">>>> Boot ok");
log_info_force_m("setup", ">>>> Firmware Version: %s", VERSION);
```

Default interval is `0` (rate limiter disabled). Set any non-zero value to enable it.

## When you need more

Want to tag the line with a subsystem name (so you can filter in the field)?
Add the `_m` suffix and pass the module as the first argument:

```cpp
log_info_m("wifi", "Connected, ip=%s", ip);
log_error_m("ota", "Firmware mismatch: have %s want %s", have, want);
```

Want a record to also land in the FDA audit trail (not just the regular log)?
Use a target-combined wrapper:

```cpp
log_audit_info("Pump started");          // → LOG sinks AND AUDIT sinks
log_audit_info_m("safety", "EMERGENCY_STOP_ARMED");
```

Want a structured event with a numeric code (for the cloud dashboard or for
machine parsing)?

```cpp
audit_event(101, "cycle", "PROGRAM_SELECT idx=%d name=%s", i, name);
status_event(7,  "wifi",  "state=disconnected reason=%d", reason);
```

That is it. Three flavours, one call site, no dual logging.

## Why this exists

Before EmblogX my projects had two separate libraries: a logger and an audit
journal designed for FDA compliance.
Any time we wanted to log something that needed to go *both* to the
operator console *and* to the FDA audit file, we had to write two calls:

```cpp
log_info("RBB1 home requested");          // operational
#ifdef ENABLE_AUDIT
if (audit_) audit_->logInfo("RBB1_HOME"); // regulatory
#endif
```

Easy to forget. Easy to drift. The two messages would slowly diverge over
time and nobody noticed until the auditor showed up. EmblogX folds the
destination into the function name so the wrong call stops being possible:
one call, one record, one shot at consistency.

Even worse, what was happening when I wanted to log to the console and to an HTTP server simultaneously? The serial console is blocking but pretty fast anyway, but to call HTTP may require much longer, so both calls in sequence would make the system extremely slow.

## Architecture

```text
host code
   │
   │ log_info / audit_warn / status_event / log_audit_info / ...
   ▼
emblogx::log_va(target, level, module, fmt, args)
   │
   ├─ level filter (global + per-module)
   ├─ format ONCE into a stack buffer
   │
   └─ for each registered sink:
        if (record.target & sink.capabilities) != 0:
            if sink.mode == Sync  → sink.write(rec)        // direct call
            if sink.mode == Async → dispatcher.push(rec)   // worker task drains
```

A few things to note:

- **The producer formats the line exactly once.** Three sinks does not mean
  three `vsnprintf` calls. The cost of "I want this on the console AND on
  SD AND posted to the cloud" is one snprintf plus three pointer copies.
- **Routing is constant time.** The sink registry is a fixed-size array
  (default 8 slots). No hash maps, no list traversal, no allocation.
- **Sync vs async is per-sink, not per-call.** The console writes
  immediately because UART is fast. The SD sink and the HTTP sink hand the
  record to a dedicated FreeRTOS worker that owns its own static queue, so
  the producer task never blocks on flash or network.
- **No heap, no `std::string`, no recursion, no dynamic registration.**
  Everything is sized at compile time.

## Public API

Each wrapper in the table below comes in four flavours that combine a module
suffix with a force suffix. The format string is always the last positional
argument.

```cpp
log_info(fmt, ...);                    // no module, rate-limited
log_info_m("wifi", fmt, ...);          // with module, rate-limited
log_info_force(fmt, ...);              // no module, bypass rate limiter
log_info_force_m("wifi", fmt, ...);    // with module, bypass rate limiter
```

`_force` / `_force_m` exist for lines that must never be dropped — boot banners,
firmware version stamp, one-shot state transitions. Use them sparingly; the
non-force variants are the default for any line that runs more than once.

The full list (each row also has `_m`, `_force`, `_force_m` flavours):

| Wrapper                | Routes to              | Level |
| ---------------------- | ---------------------- | ----- |
| `log_info`             | LOG                    | Info  |
| `log_warn`             | LOG                    | Warn  |
| `log_error`            | LOG                    | Error |
| `log_debug`            | LOG                    | Debug |
| `audit_info`           | AUDIT                  | Info  |
| `audit_warn`           | AUDIT                  | Warn  |
| `audit_error`          | AUDIT                  | Error |
| `status_info`          | STATUS                 | Info  |
| `status_warn`          | STATUS                 | Warn  |
| `status_error`         | STATUS                 | Error |
| `log_audit_info`       | LOG + AUDIT            | Info  |
| `log_audit_warn`       | LOG + AUDIT            | Warn  |
| `log_audit_error`      | LOG + AUDIT            | Error |
| `log_status_info`      | LOG + STATUS           | Info  |
| `log_status_warn`      | LOG + STATUS           | Warn  |
| `log_status_error`     | LOG + STATUS           | Error |
| `audit_status_info`    | AUDIT + STATUS         | Info  |
| `audit_status_warn`    | AUDIT + STATUS         | Warn  |
| `audit_status_error`   | AUDIT + STATUS         | Error |
| `all_info`             | LOG + AUDIT + STATUS   | Info  |
| `all_warn`             | LOG + AUDIT + STATUS   | Warn  |
| `all_error`            | LOG + AUDIT + STATUS   | Error |

Plus the two structured helpers:

```cpp
audit_event(code, module, fmt, ...);   // Info, target = AUDIT
status_event(code, module, fmt, ...);  // Info, target = STATUS
```

## Sinks

Each sink declares two things at construction time: which targets it accepts
(`capabilities`) and whether it dispatches synchronously or asynchronously.
Each sink is gated by its own compile flag so projects only pay for the ones
they enable.

| Sink         | Mode  | Capabilities          | Compile flag                  | Default |
| ------------ | ----- | --------------------- | ----------------------------- | ------- |
| `ConsoleSink`| Sync  | LOG                   | `EMBLOGX_ENABLE_SINK_CONSOLE` | ON      |
| `MemorySink` | Sync  | LOG + STATUS          | `EMBLOGX_ENABLE_SINK_MEMORY`  | OFF     |
| `HttpSink`   | Async | LOG + AUDIT + STATUS  | `EMBLOGX_ENABLE_SINK_HTTP`    | OFF     |
| `SdSink`     | Async | AUDIT                 | `EMBLOGX_ENABLE_SINK_SD`      | OFF     |

If no sink is enabled at compile time, the console sink is the safe default.
The library is never useless.

**Need MQTT, AMQP, WebSocket, gRPC, syslog, Kafka, or your own proprietary
transport?** Implement the `ISink` interface (one header, four virtual
methods: `capabilities()`, `mode()`, `begin()`, `write()`) and register it
the same way as the built-ins. Async sinks get a free FreeRTOS worker task
and a static record queue from `AsyncDispatcher` — you only write the
"how do I send one record" function. Look at `HttpSink` for a 60-line
reference implementation.

## Recipes

A handful of ready-to-use setup snippets, ordered from simplest to richest.
Pick the one that matches what your device actually does.

### 1. Console only — bench testing, prototypes

The minimum. Use this for development boards or anything that does not need
to keep history beyond the serial monitor.

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>

static emblogx::ConsoleSink console;

void setup() {
    emblogx::register_sink(&console);
    emblogx::init();
    log_info("Hello from %s", "ICB");
}
```

Build flags:

```text
-DEMBLOGX_BACKEND_ESP32   (or -DEMBLOGX_BACKEND_STDIO for PC host tests)
-DEMBLOGX_LOG_PREFIX="[ICB]"
```

### 2. Console + memory ring — UI device that shows recent logs

Add a `MemorySink` and the on-device UI can read recent log lines back
without any external connection. Same pattern as the old `log_buffer` but
without the LogEntry struct gymnastics.

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/memory_sink.h>

static emblogx::ConsoleSink console;
static emblogx::MemorySink  memory;

void setup() {
    emblogx::register_sink(&console);
    emblogx::register_sink(&memory);
    emblogx::init();
}

// Somewhere in your "Logs" UI screen:
void render_recent_logs() {
    char     buf[256];
    uint32_t cursor = 0;
    emblogx::memory_sink_seek_oldest(&cursor);
    while (size_t n = emblogx::memory_sink_read(buf, sizeof(buf) - 1, &cursor)) {
        buf[n] = '\0';
        // draw `buf` on screen, split on '\n', etc.
    }
}
```

Build flags:

```text
-DEMBLOGX_BACKEND_ESP32
-DEMBLOGX_ENABLE_SINK_MEMORY=1
-DEMBLOGX_MEMSINK_SIZE=8192      (default, raise/lower to taste)
```

### 3. Console + HTTP — remote / cloud telemetry

Adds an `HttpSink` that POSTs each record as a fully RFC 8259-compliant
JSON envelope to a remote endpoint. The worker task does this off the
producer's hot path, so log calls return in **microseconds** even when the
cloud is unreachable, the WiFi is flapping, or the server is slow. Failed
records are dropped, never retried indefinitely — the producer never
blocks and the device never falls behind.

Use this for: device fleet dashboards, cloud log aggregators
(ELK / Loki / Datadog / CloudWatch / Grafana), remote monitoring, status
reporting to a backend API, or live debugging of devices already deployed
in the field.

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/http_sink.h>

static emblogx::ConsoleSink console;
static emblogx::HttpSink    cloud("https://api.example.com/v1/logs");

void setup() {
    // Bring WiFi STA up first — the HTTP sink relies on it.
    wifi_connect();

    emblogx::register_sink(&console);
    emblogx::register_sink(&cloud);
    emblogx::init();

    log_info("boot, version %s", VERSION);
    status_info_m("wifi", "ip=%s rssi=%d", ip, rssi);  // also forwarded
}
```

Build flags:

```text
-DEMBLOGX_BACKEND_ESP32
-DEMBLOGX_ENABLE_SINK_HTTP=1
```

The HTTP sink fires for any record whose target intersects `LOG | AUDIT |
STATUS`, which is all of them. If you only want STATUS messages forwarded
(typical case for a dashboard), give the sink a narrower capability mask in
its constructor — see the source for the exact knob.

### 4. Console + memory + SD — full FDA audit trail (regulatory compliance)

This is the configuration we use on production med-tech devices that must
comply with **FDA 21 CFR Part 11** electronic-records requirements and the
**IEC 62304** software-lifecycle standard. The audit pattern below also
applies to industrial-control devices that need ISO 13485 or pharma GxP
traceability.

- Console — for the operator and the field service engineer
- Memory ring — for the on-device "Recent events" UI screen and for
  Segger J-Link / OpenOCD post-mortem snapshots
- SD card — append-only legal record, never overwritten on the device,
  rotated by the host service when the card is collected
- Optional: a private/intranet HTTP sink can mirror the audit trail to a
  hospital server in real time

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/memory_sink.h>
#include <emblogx/sinks/sd_sink.h>
#include <ungula/sd/platform/esp/esp_sd_filesystem.h>

using namespace ungula::sd;

// Adjust pins to match your board's schematic.
static EspSdSpiConfig sd_cfg = {
    .pin_miso = 13, .pin_mosi = 11,
    .pin_clk  = 12, .pin_cs   = 10,
};
static EspSdFilesystem sd_fs(sd_cfg);

static emblogx::ConsoleSink console;
static emblogx::MemorySink  memory;
static emblogx::SdSink      audit_sd(sd_fs, "/sdcard/audit.log");

void setup() {
    sd_fs.mount();

    emblogx::register_sink(&console);
    emblogx::register_sink(&memory);
    emblogx::register_sink(&audit_sd);
    emblogx::init();

    log_audit_info("BOOT version=%s", VERSION);  // both console and SD
    log_info("Battery: %d%%", battery_pct);      // console only
    audit_event(201, "safety", "INTERLOCK_ARMED");  // SD only (AUDIT target)
}
```

Build flags:

```text
-DEMBLOGX_BACKEND_ESP32
-DEMBLOGX_ENABLE_SINK_MEMORY=1
-DEMBLOGX_ENABLE_SINK_SD=1
-DEMBLOGX_ENABLE_AUDIT       # host-side flag — gates code that should only
                             # run when audit is wired
```

The `SdSink` receives an `IFileSystem` reference from
[UngulaSd](https://github.com/alexconesap/ungula-sd.git) — it never
touches ESP-IDF SD/SPI APIs directly. The host project creates the filesystem
(SPI or SDMMC) and injects it into the sink at construction time.

The SD sink only accepts the `AUDIT` target by design — operational
chatter and status pings would wear the SD out for no benefit. Anything
that needs to land on the card has to go through `audit_*` or
`log_audit_*` (so it shows up on console too) or `audit_event()`.

### 5. Everything wired — kitchen sink

Use this when bringing up a fully featured node and you want to see the
complete picture. Same pattern as recipe 4 plus HTTP forwarding to the cloud.

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/memory_sink.h>
#include <emblogx/sinks/sd_sink.h>
#include <emblogx/sinks/http_sink.h>
#include <ungula/sd/platform/esp/esp_sd_filesystem.h>

using namespace ungula::sd;

// Adjust pins to match your board's schematic.
static EspSdSpiConfig sd_cfg = { .pin_miso = 13, .pin_mosi = 11,
    .pin_clk = 12, .pin_cs = 10 };
static EspSdFilesystem sd_fs(sd_cfg);

static emblogx::ConsoleSink console;
static emblogx::MemorySink  memory;
static emblogx::SdSink      audit_sd(sd_fs, "/sdcard/audit.log");
static emblogx::HttpSink    cloud("https://api.example.com/v1/logs");

void setup() {
    sd_fs.mount();
    wifi_connect();

    emblogx::register_sink(&console);
    emblogx::register_sink(&memory);
    emblogx::register_sink(&audit_sd);
    emblogx::register_sink(&cloud);
    emblogx::init();
}
```

Every record is formatted exactly once and dispatched to the matching
sinks. The console gets it immediately, the memory ring gets it
immediately, the SD and HTTP workers get it queued.

## Memory sink — RTT-style trace buffer

The memory sink replaces the old `log_buffer`. It stores raw text bytes in a
single contiguous struct in RAM with a stable layout. External debuggers can
find it by scanning the device RAM for the `LOGBUF_V1` magic string and dump
the recent history with no agent on the device:

```cpp
struct LogTraceBuffer {
    char     magic[10];        // "LOGBUF_V1"
    uint32_t version;
    uint32_t buffer_size;
    uint32_t write_index;
    uint32_t total_bytes_written;
    char     data[EMBLOGX_MEMSINK_SIZE];
};
```

On ELF targets the struct is placed in a dedicated `.logbuf` linker section,
so projects that need a fixed RAM address can pin it from their linker
script:

```ld
.logbuf : { KEEP(*(.logbuf)) } > RAM
```

The reader API is non-blocking and incremental — pass the same cursor
across calls to read new bytes only:

```cpp
char     buf[256];
uint32_t cursor = 0;
emblogx::memory_sink_seek_oldest(&cursor);   // start from history beginning
// or:
emblogx::memory_sink_seek_newest(&cursor);   // tail mode — only new lines

while (size_t n = emblogx::memory_sink_read(buf, sizeof(buf), &cursor)) {
    // consume `n` bytes from `buf`
}
```

If the cursor lags more than `buffer_size` bytes behind the writer, the
oldest readable position is used silently — no errors, no truncation in the
middle of a line.

## Configuration

Override any of the defaults with `-D` flags from your build system:

| Define                       | Default                       | Purpose |
| ---------------------------- | ----------------------------- | ------- |
| `EMBLOGX_LINE_MAX`           | 256                           | Maximum bytes per formatted line |
| `EMBLOGX_QUEUE_SLOTS`        | 16                            | Async queue depth per sink |
| `EMBLOGX_MEMSINK_SIZE`       | 8192                          | Memory ring size in bytes |
| `EMBLOGX_MAX_SINKS`          | 8                             | Sink registry capacity |
| `EMBLOGX_LOG_PREFIX`         | `"[LOG]"`                     | Per-host prefix prepended to every line |
| `EMBLOGX_DEBUG_ENABLED`      | unset                         | Compile `log_debug()` at level Debug |
| `EMBLOGX_BACKEND_ESP32`      | auto on ESP-IDF               | Console sink uses ESP-IDF UART driver |
| `EMBLOGX_BACKEND_STDIO`      | auto on host                  | Console sink uses POSIX `stdout` |
| `EMBLOGX_LOG_UART_PORT`      | `UART_NUM_0`                  | UART port for the ESP-IDF backend |
| `EMBLOGX_ENABLE_SINK_*`      | console + memory ON, rest OFF | One toggle per sink |
| `EMBLOGX_ENABLE_AUDIT`       | unset                         | Host-side flag to switch on audit code paths |

The legacy `LOGGER_BACKEND_ESP32`, `LOGGER_BACKEND_STDIO`, and
`LOG_DEBUG_ENABLED` aliases are still recognised for backwards compatibility.

## Runtime control

Debug records are filtered at runtime, not just compile time, so you can
turn verbose logging on in the field without re-flashing:

```cpp
emblogx::set_global_level(emblogx::Level::Debug);
emblogx::set_module_level("ota", emblogx::Level::Info);

// Stop sending audit records to SD for a while (sink index from registration order)
emblogx::set_sink_enabled(2, false);
```

## Wall-clock timestamps via a pluggable time source

`Record::timestamp` is `int64_t` ms — but its **meaning** depends on which
time source the host project plugged in:

- **Default** — monotonic since boot. `now_ms()` calls `esp_timer_get_time()`
  (ESP-IDF) or `clock_gettime(CLOCK_MONOTONIC)` (POSIX). emblogx works
  out of the box, no extra wiring.
- **Bridged** — Unix epoch ms. Register a function pointer once at boot
  and every subsequent record carries wall-clock time:

```cpp
#include <emblogx/logger_core.h>
#include <ungula/core/time/time_control.h>      // UngulaCore — only the host project
                                    // needs this; emblogx itself stays
                                    // independent of UngulaCore.

void setup() {
    // ... NTP init, TimeControl::setTimeProvider(...), etc. ...

    // One-line bridge — TimeControl::now is a static method whose
    // address is just an `int64_t (*)()` pointer.
    emblogx::set_now_ms_provider(&ungula::TimeControl::now);
}
```

The hook is `int64_t (*)()` — function-pointer, not interface — because
the time source has no state to hold. Cost: one indirect call per log
record, only when a provider is registered.

### Caveats worth knowing

- **Register before the first log call.** Records emitted across the
  swap point carry timestamps from different sources and aren't
  comparable. Boot order: register sinks → register time source →
  start logging.
- **`TimeControl::now()` falls back to monotonic when no provider
  reports valid.** If NTP loses sync mid-run, log timestamps revert to
  monotonic-since-boot for the duration. The boundary is the host's
  call to handle, not emblogx's.
- **Test injection works the same way.** Tests register a scripted
  function pointer instead of `&TimeControl::now`. No interface to
  mock, no virtual dispatch.

```cpp
// In tests:
int64_t fake_clock() { return 1'700'000'000'123LL; }
emblogx::set_now_ms_provider(&fake_clock);
// ... log calls now produce records with timestamp == 1700000000123 ...
emblogx::set_now_ms_provider(nullptr);   // teardown
```

### Timestamp prefix in `Record::line`

When the registered time source returns a real wall-clock value, the
formatter automatically prepends the timestamp to every line:

```text
[2026-04-23 14:32:11][ICB][INFO][module] message text
```

So a project that already calls `set_now_ms_provider(&TimeControl::now)`
gets readable audit-log timestamps in the SD file, the memory ring, and
stdout — **with zero changes** to its sinks. Before NTP syncs (or with no
provider registered) the prefix is omitted, so monotonic-since-boot
values never produce a misleading "[1970-…]" string.

The strftime spec lives in `EMBLOGX_TIMESTAMP_FORMAT` (default
`"%Y-%m-%d %H:%M:%S"`, UTC). Override at compile time with
`-DEMBLOGX_TIMESTAMP_FORMAT='"%H:%M:%S.%f"'` for a different shape, or
`-DEMBLOGX_TIMESTAMP_FORMAT='""'` to disable the prefix unconditionally
(handy for byte-stable test output).

#### Per-sink opt-out

`ISink` carries a `show_timestamp` flag, default `true`. Toggle it on the
specific sink that should not include the text prefix — typically a sink
that already carries the timestamp out-of-band, e.g. the JSON-emitting
HTTP sink:

```cpp
my_console_sink.set_show_timestamp(false);   // strip the prefix from this sink only
```

The HTTP sink defaults the flag to `false` already because its JSON
payload encodes `Record::timestamp` as a separate numeric field — having
the prefix in `message` too would be noise. Every other sink defaults to
`true` so the prefix appears everywhere readable.

The flag is honoured automatically by all sinks via `effective_line()` /
`effective_line_len()` helpers on `ISink` — the formatter writes one
buffer, sinks pick which slice to emit. Constant-time, no second format
pass.

## Troubleshooting

**Nothing shows up on the serial console.**
You forgot `emblogx::init()`. Without it the sinks never get their
`begin()` call and the console one in particular needs that to install
its UART driver.

**`log_debug()` calls are silently ignored.**
Either you did not pass `-DEMBLOGX_DEBUG_ENABLED` (compile-time off) or
your global level is above `Debug` (runtime off). Both gates have to
agree before a Debug record reaches the sinks. Use
`emblogx::set_global_level(emblogx::Level::Debug)` at boot to flip the
runtime gate.

**`SdSink` does not write anything.**
The file is opened lazily on the first write. Make sure the `IFileSystem`
you injected is mounted (call `mount()` on your `EspSdFilesystem` or
`EspSdmmcFilesystem`) before the first log call that targets `AUDIT`.
The sink retries on each record until the open succeeds.

**`HttpSink` calls return instantly but the server never sees anything.**
That is the design — async sinks queue records on a worker task. Check
the worker is alive (`emblogx::sink_count()` reports the right number)
and that WiFi STA is up at the time the worker pops a record. The HTTP
sink ignores transient failures so the producer never blocks.

**My log lines are getting truncated.**
`EMBLOGX_LINE_MAX` defaults to 256 bytes including the level/module/prefix
header. If you need bigger payloads (typical for status JSON), bump it:
`-DEMBLOGX_LINE_MAX=512`. Each async sink slot is sized to this value, so
the queue memory grows linearly.

**I see two copies of every line.**
You registered the same sink twice, or you registered both the console
sink and another sink that also writes to UART. Walk
`emblogx::sink_count()` after `init()` and confirm.

## Testing

### Local development (sibling repos available)

If you have the full workspace with `lib_emblogx/` and `lib_sd/` as siblings,
just build and run:

```shell
cd lib_emblogx/tests
chmod +x *.sh
./1_build.sh
./2_run.sh
```

This is the default — CMake uses the sibling directories directly.

### Standalone (no sibling repos)

If you cloned only `lib_emblogx`, pass `-DUSE_LOCAL_DEPS=OFF` to fetch
dependencies from GitHub automatically:

```shell
cd lib_emblogx/tests
mkdir build && cd build
cmake .. -DUSE_LOCAL_DEPS=OFF
cmake --build .
ctest --output-on-failure
```

CMake will download [ungula-sd](https://github.com/alexconesap/ungula-sd.git)
into the `vendor/` folder.

The test build uses the STDIO console backend and the synchronous fallback
for async sinks so test assertions stay deterministic — no FreeRTOS, no
threads, no flaky timing.

## Dependencies

| Library | Repo | Used for |
| ------- | ---- | -------- |
| UngulaSd | [ungula-sd](https://github.com/alexconesap/ungula-sd.git) | Required when `EMBLOGX_ENABLE_SINK_SD=1` — provides `IFileSystem` and `IFile` interfaces injected into `SdSink` |
| UngulaCore | [ungula-core](https://github.com/alexconesap/ungula-core.git) | Platform abstractions reused by optional sinks (time, system control). The core router does not need it |
| UngulaNet | [ungula-net](https://github.com/alexconesap/ungula-net.git) | Required only when `EMBLOGX_ENABLE_SINK_HTTP=1` |

## Acknowledgements

Thanks to Claude and ChatGPT for helping on generating this documentation.

## License

MIT — see `LICENSE` for details.
