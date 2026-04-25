// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// Compile-time configuration. Override any of these with -D flags from the
// build system. Defaults are tuned for ESP32-class targets.
//
// Example (in your build_flags / .settings):
//   -DEMBLOGX_LINE_MAX=384
//   -DEMBLOGX_QUEUE_SLOTS=32
//   -DEMBLOGX_MEMSINK_SIZE=16384
//   -DEMBLOGX_MAX_SINKS=8
//   -DEMBLOGX_LOG_PREFIX='"[ICB] "'

#include <cstddef>
#include <cstdint>

// ---- Buffer sizes ---------------------------------------------------------

// Max length of one fully-formatted log line — prefix, level tag, module
// and message body, NOT including any trailing newline. The router builds
// the line into a stack buffer of this size and passes it to sinks; sinks
// append their own newline (or framing) when writing. Lines longer than
// this are truncated. Larger values consume more stack inside log_va().
#ifndef EMBLOGX_LINE_MAX
#define EMBLOGX_LINE_MAX 256
#endif

// Number of slots in each async sink's queue. Each slot holds one fully
// formatted line of EMBLOGX_LINE_MAX bytes. Total memory per async sink is
// EMBLOGX_QUEUE_SLOTS * EMBLOGX_LINE_MAX bytes (default 16 * 256 = 4 KiB).
#ifndef EMBLOGX_QUEUE_SLOTS
#define EMBLOGX_QUEUE_SLOTS 16
#endif

// Memory sink (RTT-style) ring buffer size in bytes. Older bytes are
// overwritten when full. Set to 0 to compile out the memory sink data array
// entirely (the symbol still exists for the linker section).
#ifndef EMBLOGX_MEMSINK_SIZE
#define EMBLOGX_MEMSINK_SIZE 8192
#endif

// Maximum number of registered sinks. Static array — no heap.
#ifndef EMBLOGX_MAX_SINKS
#define EMBLOGX_MAX_SINKS 8
#endif

// Per-host prefix prepended to every line. The formatter in logger_core.cpp
// adds the [LEVEL][module] tokens right after this prefix and a trailing
// space, so a non-spaced value like "[ICB]" renders as
// "[ICB][INFO][module] message".
#ifndef EMBLOGX_LOG_PREFIX
#define EMBLOGX_LOG_PREFIX "[LOG]"
#endif

// strftime spec used to render the per-record timestamp prefix that
// precedes EMBLOGX_LOG_PREFIX. Default is ISO-8601 UTC, wrapped in `[ ]`
// by the formatter so a fully-rendered line looks like:
//   "[2026-04-23 14:32:11][ICB][INFO][module] message"
//
// The prefix only appears when the registered time source returns a
// real wall-clock value (above the threshold defined in logger_core.cpp).
// Monotonic-since-boot values produce no prefix, so projects that haven't
// installed an NTP / wall-clock provider see exactly the same output as
// before. Define to "" to disable the prefix unconditionally — useful for
// unit tests that want byte-stable output.
#ifndef EMBLOGX_TIMESTAMP_FORMAT
#define EMBLOGX_TIMESTAMP_FORMAT "%Y-%m-%d %H:%M:%S"
#endif

// ---- Default visibility for debug -----------------------------------------
// Define EMBLOGX_DEBUG_ENABLED to compile log_debug() at level DEBUG. When
// not defined, log_debug() is still callable but elided to a no-op.
// (LOG_DEBUG_ENABLED kept as alias so existing build systems still work.)
#if defined(LOG_DEBUG_ENABLED) && !defined(EMBLOGX_DEBUG_ENABLED)
#define EMBLOGX_DEBUG_ENABLED
#endif
