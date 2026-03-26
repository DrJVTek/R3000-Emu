#pragma once
// Async (threaded) log backend for emu::logf.
//
// Usage:
//   async_log_init(max_level);   // call once at startup, before any logf
//   // ... emulation runs ...
//   async_log_shutdown();        // call at exit to flush remaining entries
//
// When initialised, emu::logf pushes entries to a lockless SPSC ring buffer.
// A background thread pops and writes them to stderr with their timestamp.
// The emulation thread spends ~10-30 ns per log call (atomic store only).

#include <cstdint>
#include "emu_log.h"

namespace emu
{

// Start the async log background thread.
// max_level: filter, same as emu::Log::max_level.
// if ring_cap_bits >= 10 && <= 17: ring buffer has 2^n entries (default=14 = 16384).
void async_log_init(LogLevel max_level, int ring_cap_bits = 14);

// Flush remaining entries and stop the background thread.
// Blocks until all queued messages have been written.
void async_log_shutdown();

} // namespace emu
