#include "filelog.h"

#include <cinttypes>

namespace flog
{

void vlogf(const Sink& s, const Clock& c, Level lvl, const char* tag, const char* fmt, va_list args)
{
    if (!s.f || !fmt)
        return;
    if ((uint8_t)lvl > (uint8_t)s.level)
        return;

    const uint64_t ms = ms_since(c);
    std::fprintf(s.f, "[%8" PRIu64 " ms] [%s] ", ms, (tag && *tag) ? tag : "LOG");
    std::vfprintf(s.f, fmt, args);
    std::fputc('\n', s.f);
    std::fflush(s.f);
}

void logf(const Sink& s, const Clock& c, Level lvl, const char* tag, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vlogf(s, c, lvl, tag, fmt, args);
    va_end(args);
}

} // namespace flog

