# EmblogX

Embedded C++17 logging facade for ESP32 / STM32 / host. One printf-style
call formats a line once and routes it to every registered sink whose
capability bitmask intersects the record's target bitmask. Capabilities
are LOG (operational), AUDIT (FDA / regulatory trail), STATUS (state /
event reporting). Sinks declare sync or async dispatch; async sinks
own a fixed-capacity FreeRTOS queue. No heap after `init()`.

---

## Usage

### Use case: console-only, simplest possible setup

```cpp
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>

static emblogx::ConsoleSink console;

void setup() {
    emblogx::register_sink(&console);
    emblogx::init();

    log_info_force("[boot] firmware up");
    log_info_m("wifi", "ip=%s", "192.168.1.42");
    log_warn("battery low: %d%%", 12);
    log_error_m("ota", "image mismatch: have=%s want=%s", "1.0.0", "1.0.1");
}

void loop() {}
```

When to use this: bench prototypes, host tests, projects that only need
serial output. Console is `Capability::LOG` only — `audit_*` and
`status_*` calls won't hit it.

### Use case: per-module level filtering and global level

```cpp
#include <emblogx/logger.h>
#include <emblogx/logger_core.h>
#include <emblogx/sinks/console_sink.h>

static emblogx::ConsoleSink console;

void setup() {
    emblogx::register_sink(&console);
    emblogx::init();

    emblogx::set_global_level(emblogx::Level::Info);
    emblogx::set_module_level("ota", emblogx::Level::Debug);
    emblogx::set_module_level("wifi", emblogx::Level::Warn);

    log_debug_m("ota", "scanning partitions");      // shown
    log_info_m("wifi", "connecting...");            // dropped (below Warn)
    log_warn_m("wifi", "weak rssi=%d", -88);        // shown
}

void loop() {}
```

When to use this: production builds where one noisy subsystem must be
quieted without losing the others. Module strings must be string
literals — only the pointer is stored.

### Use case: AUDIT trail to SD plus operational log to console

```cpp
#define EMBLOGX_ENABLE_SINK_SD 1
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/sd_sink.h>
#include <ungula/sd.h>

extern ::ungula::sd::IFileSystem& host_sd_filesystem();

static emblogx::ConsoleSink console;
static emblogx::SdSink     audit(host_sd_filesystem(), "/sdcard", "audit");

void setup() {
    emblogx::register_sink(&console);
    emblogx::register_sink(&audit);
    emblogx::init();

    log_info("operator login id=%d", 42);                 // LOG only -> console
    audit_info_m("safety", "EMERGENCY_STOP_ARMED");       // AUDIT only -> sd
    log_audit_info_m("cycle", "PROGRAM_SELECT idx=%d", 3); // both targets -> both sinks
}

void loop() {}
```

When to use this: regulated devices needing a tamper-resistant trail.
Journal-mode SD rotates per boot session; the sink prunes oldest files
when free space drops below 10%. SD writes happen on a worker task —
producer never blocks on the file system.

### Use case: HTTP telemetry sink (asynchronous, non-blocking)

```cpp
#define EMBLOGX_ENABLE_SINK_HTTP 1
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/http_sink.h>

static emblogx::ConsoleSink console;
static emblogx::HttpSink    cloud("https://logs.example.com/ingest");

void setup() {
    emblogx::register_sink(&console);
    emblogx::register_sink(&cloud);
    emblogx::init();

    status_info_m("net", "rssi=%d state=%d", -55, 1);
    audit_warn_m("door", "open_after_cycle");
}

void loop() {}
```

When to use this: cloud forwarding. The HTTP sink accepts LOG, AUDIT,
and STATUS; producer task copies the record into the dispatcher's
static queue and returns in microseconds. Worker task POSTs JSON
through `lib_net`. JSON payload carries `timestamp` as a numeric
field so the sink suppresses the text timestamp prefix by default.

### Use case: in-RAM ring buffer for an on-device debug viewer

```cpp
#define EMBLOGX_ENABLE_SINK_MEMORY 1
#include <emblogx/logger.h>
#include <emblogx/sinks/console_sink.h>
#include <emblogx/sinks/memory_sink.h>

static emblogx::ConsoleSink console;
static emblogx::MemorySink  ring;

void setup() {
    emblogx::register_sink(&console);
    emblogx::register_sink(&ring);
    emblogx::init();

    log_info_force("boot");
}

void dump_recent(char* buf, size_t cap) {
    uint32_t cursor = 0;
    emblogx::memory_sink_seek_oldest(&cursor);
    size_t got = emblogx::memory_sink_read(buf, cap, &cursor);
    (void)got;
}

void loop() {}
```

When to use this: UI screens that show "last 100 lines", or external
JTAG/RTT debuggers that scan RAM for the `LOGBUF_V1` magic. The buffer
lives in linker section `.logbuf`. Capability is `LOG | STATUS`.

### Use case: structured event with numeric code

```cpp
#include <emblogx/logger.h>

void on_program_select(int idx, const char* name) {
    audit_event(101, "cycle", "PROGRAM_SELECT idx=%d name=%s", idx, name);
    status_event(7,  "wifi",  "state=disconnected reason=%d", 4);
}
```

When to use this: emit a stable numeric event ID alongside the human
text. Renders as `code=<n> <body>` so existing string sinks see only
text. Always Info level.

### Use case: rate-limit a hot loop

```cpp
#include <emblogx/logger.h>

void setup() {
    log_set_rate_limit_ms(1000);  // at most one line per fmt-pointer per second
    log_info_force("boot");        // bypass: must always emit
}

void loop() {
    log_info("tick rssi=%d", -42);   // throttled
    log_error("hw fault: 0x%x", 0);  // Error always passes
}
```

When to use this: the same `fmt` literal called every iteration. Keyed
by `fmt` pointer identity, not content. `_force` and `_force_m` always
bypass; Error level always bypasses.

### Use case: bridge a wall-clock time source (NTP / RTC)

```cpp
#include <emblogx/logger.h>
#include <emblogx/logger_core.h>
#include <ungula/core/time/time_control.h>

void setup() {
    emblogx::set_now_ms_provider(&ungula::TimeControl::now);
    // …register sinks, init()
}
```

When to use this: once a Unix-epoch source is available. The formatter
prepends `[YYYY-MM-DD HH:MM:SS]` (per `EMBLOGX_TIMESTAMP_FORMAT`) when
the provider returns a real wall-clock value. `Record::timestamp`
becomes Unix-epoch ms; otherwise it stays monotonic-since-boot.

---

## API

All wrappers are free `inline` functions in the global namespace. Core
runtime functions live in `namespace emblogx` and require
`<emblogx/logger_core.h>`.

### Header layout

| Include | Purpose |
| ------- | ------- |
| `<emblogx/logger.h>` | Printf wrappers (`log_*`, `audit_*`, `status_*`, `all_*`, `*_event`), `log_init`, `log_flush`, `log_set_rate_limit_ms` |
| `<emblogx/logger_core.h>` | Sink registry (`register_sink`, `init`, `flush_all`), level control, time source, `log_va` |
| `<emblogx/types.h>` | `Level`, `Target`, `Capability`, `Mode`, `Record`, `levelName` |
| `<emblogx/sinks/i_sink.h>` | `ISink` base class |
| `<emblogx/sinks/console_sink.h>` | `ConsoleSink` |
| `<emblogx/sinks/memory_sink.h>` | `MemorySink`, `memory_sink_*` readers |
| `<emblogx/sinks/http_sink.h>` | `HttpSink` |
| `<emblogx/sinks/sd_sink.h>` | `SdSink` |
| `<emblogx.h>` | Umbrella that pulls all of the above (Arduino discovery) |

### Wrapper family — naming pattern

For every base name `NAME` (`log_info`, `log_warn`, `log_error`,
`log_debug`, `audit_info`, `audit_warn`, `audit_error`, `audit_debug`,
`status_info`, `status_warn`, `status_error`, `log_audit_info`,
`log_audit_warn`, `log_audit_error`, `log_audit_debug`,
`log_status_info`, `log_status_warn`, `log_status_error`,
`audit_status_info`, `audit_status_warn`, `audit_status_error`,
`all_info`, `all_warn`, `all_error`), four overloads exist:

```cpp
NAME       (const char* fmt, ...);                       // no module
NAME##_m   (const char* module, const char* fmt, ...);   // with module
NAME##_force      (const char* fmt, ...);                // bypass rate limit
NAME##_force_m    (const char* module, const char* fmt, ...);
```

`fmt` and `module` MUST be string literals (or any pointer with static
storage). The library stores the pointers, never copies them.
`__attribute__((format(printf, ...)))` is enabled on every wrapper.

`log_debug*` is callable unconditionally; when `EMBLOGX_DEBUG_ENABLED`
is not defined it emits at `Level::Debug` and is filtered by the
runtime level (default `Info`).

### Structured events

```cpp
inline void audit_event (int code, const char* module, const char* fmt, ...);
inline void status_event(int code, const char* module, const char* fmt, ...);
```

Both render the body as `code=<code> <formatted-body>`, target is
`AUDIT` and `STATUS` respectively, level is always `Info`. Body is
formatted into a stack buffer of `EMBLOGX_LINE_MAX` bytes before
re-formatting through `emit()`.

---

## Public types

### `emblogx::Level` (enum class, uint8_t)

`Debug = 0`, `Info = 1`, `Warn = 2`, `Error = 3`. `levelName(Level)`
returns the uppercase string used in formatted output.

### `emblogx::Target` (constexpr bitmask, uint8_t)

| Constant | Value | Meaning |
| -------- | ----- | ------- |
| `Target::LOG`    | `0x01` | operational logs |
| `Target::AUDIT`  | `0x02` | regulatory / FDA audit trail |
| `Target::STATUS` | `0x04` | state / event reporting |
| `Target::ALL`    | `0x07` | all three |

### `emblogx::Capability` (constexpr bitmask, uint8_t)

Same values as `Target::*`. A sink declares its accepted targets via
`ISink::capabilities()`. The router dispatches a record only to sinks
whose capability bitmask AND the record's target is non-zero.

### `emblogx::Mode` (enum class, uint8_t)

`Sync` — `write()` runs in the producer task. `Async` — record is
copied into an `AsyncDispatcher` queue; worker task calls `write()`.

### `emblogx::Record` (POD struct)

```cpp
struct Record {
    uint8_t  target;                  // Target::* bitmask
    Level    level;
    const char* module;               // stable literal; never copied
    const char* line;                 // pre-formatted, no trailing newline
    uint16_t line_len;                // strlen(line)
    uint16_t timestamp_prefix_len;    // bytes consumed by "[YYYY-...] "
    int64_t  timestamp;               // ms; monotonic OR Unix epoch
};
```

`line` points into a stack buffer that is invalid after `write()`
returns. Sinks must copy if they need it later.

### `emblogx::LogTraceBuffer` (POD struct, memory sink only)

```cpp
struct LogTraceBuffer {
    char     magic[10];               // "LOGBUF_V1"
    uint32_t version;                 // 1
    uint32_t buffer_size;             // EMBLOGX_MEMSINK_SIZE
    uint32_t write_index;
    uint32_t total_bytes_written;     // monotonic, never resets
    char     data[EMBLOGX_MEMSINK_SIZE];
};
```

Lives in linker section `.logbuf`. Stable layout — append-only.

### `emblogx::ISink` (abstract)

```cpp
class ISink {
public:
    virtual uint8_t capabilities() const = 0;
    virtual Mode    mode()         const = 0;
    virtual bool    begin()                { return true; }
    virtual void    write(const Record&)   = 0;
    virtual void    flush()                {}
    virtual const char* name() const       { return "sink"; }

    void           set_show_timestamp(bool);
    virtual bool   show_timestamp() const;
    const char*    effective_line(const Record&) const;
    uint16_t       effective_line_len(const Record&) const;
};
```

Concrete sinks must `register_sink(&instance)` exactly once at boot.
There is no unregister. Use `effective_line` / `effective_line_len`
inside `write()` to honour the per-sink timestamp-prefix flag.

### Concrete sinks

| Class | Capabilities | Mode | Notes |
| ----- | ------------ | ---- | ----- |
| `ConsoleSink` | `LOG` | Sync | Backend selected at build: `EMBLOGX_BACKEND_ESP32` (UART0) or `EMBLOGX_BACKEND_STDIO`. Aliases `LOGGER_BACKEND_*` honoured. Gate: `EMBLOGX_ENABLE_SINK_CONSOLE` (default 1). |
| `MemorySink` | `LOG \| STATUS` | Sync | Ring buffer of `EMBLOGX_MEMSINK_SIZE` bytes. Gate: `EMBLOGX_ENABLE_SINK_MEMORY` (default 0). |
| `HttpSink(const char* url)` | `LOG \| AUDIT \| STATUS` | Async | JSON POST via `lib_net`. Default `show_timestamp(false)`. Gate: `EMBLOGX_ENABLE_SINK_HTTP` (default 0). |
| `SdSink(IFileSystem&, const char* path)` | `AUDIT` | Async | Single-file mode. Gate: `EMBLOGX_ENABLE_SINK_SD` (default 0). |
| `SdSink(IFileSystem&, const char* dir, const char* prefix)` | `AUDIT` | Async | Journal mode: `{dir}/{prefix}_{seq}.log` per boot, prunes oldest below 10% free. |

---

## Public functions / methods

### Sink registry — `<emblogx/logger_core.h>`

```cpp
bool    emblogx::register_sink(ISink* sink);   // false if registry full
uint8_t emblogx::sink_count();
void    emblogx::init();                       // calls begin() on every new sink
void    emblogx::flush_all();                  // calls flush() on every sink

void    emblogx::set_sink_enabled(uint8_t index, bool enabled);
bool    emblogx::is_sink_enabled(uint8_t index);
ISink*  emblogx::sink_at(uint8_t index);       // nullptr if oob
```

Capacity: `EMBLOGX_MAX_SINKS` (default 8). `register_sink()` MUST run
before any task calls a `log_*` wrapper. Calling it after `init()` once
producers are running is undefined behavior. There is no `unregister`.

### Level control

```cpp
void              emblogx::set_global_level(Level lvl);
emblogx::Level    emblogx::get_global_level();              // default Info
bool              emblogx::set_module_level(const char* module, Level lvl);
```

Per-module table is fixed-capacity; `set_module_level` returns false
when full. Module pointer is stored, never copied — must be a literal.
Records below the effective level are dropped before routing.

### Rate limiter

```cpp
void     emblogx::set_rate_limit_ms(uint32_t ms);   // 0 = disabled (default)
uint32_t emblogx::get_rate_limit_ms();

inline void log_set_rate_limit_ms(uint32_t ms);     // wrapper in logger.h
inline uint32_t log_get_rate_limit_ms();
```

Keyed by the `fmt` pointer (identity, not content). `Level::Error` and
the `_force` / `_force_m` wrappers always pass.

### Time source

```cpp
using NowMsFn = int64_t (*)();

void     emblogx::set_now_ms_provider(NowMsFn fn);   // nullptr -> default
NowMsFn  emblogx::get_now_ms_provider();
int64_t  emblogx::now_ms();
```

Default: monotonic since boot (`esp_timer` on ESP-IDF,
`CLOCK_MONOTONIC` on POSIX). Install once at boot. Mixing pre- and
post-swap timestamps yields incomparable values.

### Single entry point

```cpp
void emblogx::log_va      (uint8_t target, Level, const char* module,
                           const char* fmt, va_list);
void emblogx::log_va_force(uint8_t target, Level, const char* module,
                           const char* fmt, va_list);
inline void emblogx::log  (uint8_t target, Level, const char* module,
                           const char* fmt, ...);
```

Every wrapper resolves to one of these. Use them directly only when the
target / level is computed at runtime.

### Lifecycle wrappers — `<emblogx/logger.h>`

```cpp
inline void log_init();    // == emblogx::init()
inline void log_flush();   // == emblogx::flush_all()
```

### Memory sink readers — `<emblogx/sinks/memory_sink.h>`

```cpp
const LogTraceBuffer* emblogx::memory_sink_buffer();
size_t                emblogx::memory_sink_read(char* dst, size_t max,
                                                uint32_t* cursor);
void                  emblogx::memory_sink_seek_oldest(uint32_t* cursor);
void                  emblogx::memory_sink_seek_newest(uint32_t* cursor);
```

Cursor is a monotonic byte offset matching `total_bytes_written`.
Lagging more than `buffer_size` bytes silently jumps forward to the
oldest readable position. Reader takes a consistent snapshot of write
indices under a portMUX critical section; data copy itself is lockless,
so under heavy concurrent writing the reader may observe one or two
torn bytes — never a missing or duplicated line.

---

## Lifecycle

1. Construct sinks as static instances at file scope.
2. (Optional) `emblogx::set_now_ms_provider(...)` to install a
   wall-clock source.
3. `emblogx::register_sink(&sink)` for each sink, in any order.
4. `emblogx::init()` (or `log_init()`) — calls `begin()` on every new
   sink and starts async dispatcher tasks.
5. (Optional) `emblogx::set_global_level`, `emblogx::set_module_level`,
   `log_set_rate_limit_ms`.
6. From any task: `log_*` / `audit_*` / `status_*`.
7. Before reboot: `log_flush()` to drain async queues and flush sinks.

`init()` is idempotent — re-calling processes only newly-registered
sinks. There is no shutdown path for the registry; sinks live forever.

---

## Error handling

- `register_sink` returns `false` when more than `EMBLOGX_MAX_SINKS`
  attempts are made. Caller is expected to handle this at boot
  configuration; there is no recovery.
- `set_module_level` returns `false` when the per-module table is full.
- `ISink::begin()` returning `false` disables that sink for the
  process lifetime; subsequent `write()` calls are skipped by the
  router.
- Lines longer than `EMBLOGX_LINE_MAX` are silently truncated.
- Async sink queue overflow drops the OLDEST record (head advances)
  to keep newest data — non-blocking, no error returned.
- No exceptions are thrown anywhere. No allocation after `init()`.

---

## Threading / timing / hardware notes

- **Boot phase only**: `register_sink`, first `init`, and
  `set_now_ms_provider` are NOT mutex-protected. Run them from a
  single boot context (`setup()` / `app_main()`) before any task that
  calls `log_*` is created.
- **After boot**: all `log_*` wrappers, `set_global_level`,
  `set_module_level`, `set_sink_enabled` are safe from any number of
  concurrent tasks.
- **Sync sinks**: `write()` runs in the producer task. Keep them
  short. Console UART writes are bounded.
- **Async sinks**: `push()` is non-blocking and copies the formatted
  line into a static slot. The worker task (created in `begin()`,
  pinned via `AsyncDispatcher::start()` parameters) calls `write()`.
  HTTP sink uses a 1-priority task on core 1 by default; SD sink
  similar with a 4096-word stack.
- **Memory sink**: writer takes a portMUX critical section to update
  ring indices; reader snapshots indices under the same lock but
  copies bytes lock-free.
- **No heap after `init()`**: every queue, slot buffer, and per-module
  level row is a fixed-size static array.
- **ISR safety**: do NOT call `log_*` from an ISR. The producer path
  formats with `vsnprintf` into a stack buffer and walks the registry
  — neither is ISR-safe.

---

## Compile-time configuration (`config.h`)

| Macro | Default | Effect |
| ----- | ------- | ------ |
| `EMBLOGX_LINE_MAX`           | 256       | Max formatted line length, bytes. |
| `EMBLOGX_QUEUE_SLOTS`        | 16        | Slots per async dispatcher (× LINE_MAX). |
| `EMBLOGX_MEMSINK_SIZE`       | 8192      | Memory ring size in bytes. |
| `EMBLOGX_MAX_SINKS`          | 8         | Sink registry capacity. |
| `EMBLOGX_LOG_PREFIX`         | `"[LOG]"` | Per-host prefix. |
| `EMBLOGX_TIMESTAMP_FORMAT`   | `"%Y-%m-%d %H:%M:%S"` | strftime spec; empty disables. |
| `EMBLOGX_DEBUG_ENABLED`      | undef     | When defined, debug records are emitted at `Level::Debug` (still filtered at runtime). |
| `EMBLOGX_BACKEND_ESP32`      | undef     | Console sink uses ESP-IDF UART driver. |
| `EMBLOGX_BACKEND_STDIO`      | undef     | Console sink uses POSIX stdout. |
| `EMBLOGX_ENABLE_SINK_CONSOLE`| 1         | Compile in `ConsoleSink`. |
| `EMBLOGX_ENABLE_SINK_MEMORY` | 0         | Compile in `MemorySink`. |
| `EMBLOGX_ENABLE_SINK_HTTP`   | 0         | Compile in `HttpSink` (depends on `lib_net`). |
| `EMBLOGX_ENABLE_SINK_SD`     | 0         | Compile in `SdSink` (depends on `lib_sd`). |

Aliases preserved for legacy build systems: `LOGGER_BACKEND_ESP32`,
`LOGGER_BACKEND_STDIO`, `LOG_DEBUG_ENABLED`.

---

## Output format

Sinks receive a pre-formatted line:

```
[2026-04-23 14:32:11][LOG][INFO][wifi] connected ip=192.168.1.42
^^^^^^^^^^^^^^^^^^^^^^                                              optional, only when wall-clock provider returns a real timestamp
                       ^^^^^^                                       EMBLOGX_LOG_PREFIX
                              ^^^^^^                                level tag from levelName()
                                     ^^^^^^                         module (omitted when nullptr / "")
                                            ^^^^^^^^^^^^^^^^^^^^^^^ formatted body, no trailing newline
```

The router formats the line ONCE per call; each sink emits the same
bytes (modulo the per-sink timestamp opt-out). Sinks append their own
newline / framing.

---

## Internals not part of the public API

- `emblogx::detail::emit` — implementation helper for `audit_event`
  / `status_event`. Don't call directly; use the wrappers.
- `EMBLOGX_DEFINE_WRAPPERS` — macro is `#undef`d at the end of
  `logger.h`. Don't redefine in user code.
- `AsyncDispatcher` (`<emblogx/async_dispatcher.h>`) — exists for
  sink authors only. Don't push records into it from application
  code; use the `log_*` wrappers, which go through the router.
- `AsyncDispatcher::Slot` — internal queue layout. May change.
- `logger_core.cpp` formatting helpers (line builder, rate-limit
  table, per-module table) — not exposed.
- `.logbuf` linker section — referenced by external tooling, not by
  application code.
- `LOGGER_BACKEND_*` / `LOG_DEBUG_ENABLED` legacy aliases — exist for
  build-system migration; new code should use the `EMBLOGX_*` names.

---

## LLM usage rules

- Use only the documented public API. The `log_*` / `audit_*` /
  `status_*` wrappers cover every operational case — don't call
  `emblogx::log_va` unless target/level is computed at runtime.
- Module strings and `fmt` strings MUST be string literals (or any
  pointer with static storage). Passing a stack-allocated `char[]`,
  `String::c_str()`, or freed heap is undefined behavior — there is
  no runtime check.
- Pick the wrapper whose name encodes the intended targets
  (`audit_info`, `log_audit_info`, `status_warn`, ...). Don't OR
  `Target::*` bitmasks by hand unless going through `emblogx::log`.
- Don't read implementation files (`*.cpp`) to figure out usage. If
  it's not in this file or the headers above, it's not public.
- Don't add a logging call inside another `lib_*` library. Logging is
  the host project's concern. Existing calls in `lib_motor`,
  `lib_net`, `lib_ota` are tolerated debt, not a pattern to copy.
- Don't allocate after `init()`. Don't call `log_*` from an ISR.
- Preserve the project terminology: LOG / AUDIT / STATUS targets,
  Sync / Async modes, "sink", "module", "record".
