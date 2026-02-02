#pragma once

#include <cstdint>

namespace emu
{

enum class LogLevel : uint8_t
{
    error = 0,
    warn  = 1,
    info  = 2,
    debug = 3,
    trace = 4,
};

// Host-provided callback.  UE5 routes to UE_LOG, CLI routes to stderr.
using LogCallback = void (*)(LogLevel level, const char* tag, const char* msg, void* user);

struct Log
{
    LogCallback cb{nullptr};
    void*       user{nullptr};
    LogLevel    max_level{LogLevel::info};
};

// Set the global log sink (call once at startup, before any logf).
void log_init(Log* log);

// Printf-style logging.  tag = short component name ("GPU", "CD", "CPU", ...).
// If no Log has been set, falls back to fprintf(stderr).
void logf(LogLevel lvl, const char* tag, const char* fmt, ...);

// Parse a level string ("error","warn","info","debug","trace") → LogLevel.
LogLevel log_parse_level(const char* s);

} // namespace emu
