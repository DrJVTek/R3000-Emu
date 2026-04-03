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

// Consumer callback type: called from the log thread for each entry.
// If not set, entries go to stderr (CLI default).
using AsyncLogConsumer = void(*)(uint64_t ts_ns, LogLevel level,
                                  const char* tag, const char* msg, void* user);

// Start the async log background thread.
// max_level: filter, same as emu::Log::max_level.
// consumer/user: optional output callback (called from log thread, not emu thread).
//   If nullptr, entries go to stderr.
void async_log_init(LogLevel max_level, int ring_cap_bits = 14,
                     AsyncLogConsumer consumer = nullptr, void* consumer_user = nullptr);

// Flush remaining entries and stop the background thread.
// Blocks until all queued messages have been written.
void async_log_shutdown();

} // namespace emu
