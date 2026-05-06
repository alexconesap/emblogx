// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Alex Conesa
// See LICENSE file for details.

#pragma once

// Umbrella header — triggers Arduino library discovery via #include scanning.
// Real code should include <emblogx/logger.h> directly (and one of the sink
// headers from <emblogx/sinks/*.h> for the host setup file).

#include "emblogx/logger.h"
#include "emblogx/sinks/console_sink.h"
#include "emblogx/sinks/http_sink.h"
#include "emblogx/sinks/memory_sink.h"

// The SD sink depends on lib_sd (IFileSystem / IFile interfaces). Pull in
// its umbrella header first so the Arduino build system discovers lib_sd
// before sd_sink.h tries to include <ungula/sd/i_file.h>.
#if EMBLOGX_ENABLE_SINK_SD
#include <ungula/sd.h>
#endif
#include "emblogx/sinks/sd_sink.h"
