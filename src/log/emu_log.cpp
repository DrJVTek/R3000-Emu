#include "emu_log.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace emu
{

static Log* g_log = nullptr;

void log_init(Log* log)
{
    g_log = log;
}

void logf(LogLevel lvl, const char* tag, const char* fmt, ...)
{
    // Fast level check
    if (g_log && (uint8_t)lvl > (uint8_t)g_log->max_level)
        return;

    char buf[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_log && g_log->cb)
    {
        g_log->cb(lvl, tag ? tag : "LOG", buf, g_log->user);
    }
    else
    {
        // Fallback: stderr
        std::fprintf(stderr, "[%s] %s\n", tag ? tag : "LOG", buf);
        std::fflush(stderr);
    }
}

LogLevel log_parse_level(const char* s)
{
    if (!s) return LogLevel::info;
    if (std::strcmp(s, "error") == 0) return LogLevel::error;
    if (std::strcmp(s, "warn")  == 0) return LogLevel::warn;
    if (std::strcmp(s, "info")  == 0) return LogLevel::info;
    if (std::strcmp(s, "debug") == 0) return LogLevel::debug;
    if (std::strcmp(s, "trace") == 0) return LogLevel::trace;
    return LogLevel::info;
}

} // namespace emu
