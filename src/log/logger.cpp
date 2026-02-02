#include "logger.h"

#include <cctype>
#include <cstring>

namespace rlog
{

static const char* level_name(Level lvl)
{
    switch (lvl)
    {
        case Level::error:
            return "ERROR";
        case Level::warn:
            return "WARN";
        case Level::info:
            return "INFO";
        case Level::debug:
            return "DEBUG";
        case Level::trace:
            return "TRACE";
    }
    return "UNKNOWN";
}

void logger_vlogf(Logger* l, Level lvl, Category cat, const char* fmt, va_list args)
{
    if (!l)
        return;
    if (!logger_enabled(l, lvl, cat))
        return;

    if (l->cb)
    {
        // Callback path: format into buffer, call callback.
        char buf[1024];
        std::vsnprintf(buf, sizeof(buf), fmt, args);
        l->cb(lvl, cat, buf, l->cb_user);
    }
    else if (l->out)
    {
        std::fprintf(l->out, "[%s] ", level_name(lvl));
        std::vfprintf(l->out, fmt, args);
        std::fputc('\n', l->out);
        std::fflush(l->out);
    }
}

void logger_logf(Logger* l, Level lvl, Category cat, const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    logger_vlogf(l, lvl, cat, fmt, args);
    va_end(args);
}

static int streq_ci(const char* a, const char* b)
{
    if (!a || !b)
        return 0;
    while (*a && *b)
    {
        const unsigned char ca = (unsigned char)*a++;
        const unsigned char cb = (unsigned char)*b++;
        if (std::tolower(ca) != std::tolower(cb))
            return 0;
    }
    return *a == *b;
}

Level parse_level(const char* s)
{
    if (streq_ci(s, "error"))
        return Level::error;
    if (streq_ci(s, "warn") || streq_ci(s, "warning"))
        return Level::warn;
    if (streq_ci(s, "info"))
        return Level::info;
    if (streq_ci(s, "debug"))
        return Level::debug;
    if (streq_ci(s, "trace"))
        return Level::trace;
    return Level::info;
}

static uint32_t cat_from_token(const char* tok)
{
    if (streq_ci(tok, "fetch"))
        return cat_mask(Category::fetch);
    if (streq_ci(tok, "decode"))
        return cat_mask(Category::decode);
    if (streq_ci(tok, "exec"))
        return cat_mask(Category::exec);
    if (streq_ci(tok, "mem"))
        return cat_mask(Category::mem);
    if (streq_ci(tok, "exc"))
        return cat_mask(Category::exc);
    if (streq_ci(tok, "all"))
        return cat_mask(Category::all);
    return 0;
}

uint32_t parse_categories_csv(const char* s)
{
    if (!s || !*s)
        return cat_mask(Category::none);
    if (streq_ci(s, "all"))
        return cat_mask(Category::all);

    uint32_t out = 0;

    // Parser CSV in-place sur une copie locale petite.
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s", s);
    char* p = buf;
    while (*p)
    {
        while (*p == ' ' || *p == '\t' || *p == ',')
            ++p;
        if (!*p)
            break;
        char* start = p;
        while (*p && *p != ',')
            ++p;
        if (*p)
            *p++ = '\0';

        // Trim right
        char* end = start + std::strlen(start);
        while (end > start && (end[-1] == ' ' || end[-1] == '\t'))
        {
            end[-1] = '\0';
            --end;
        }

        out |= cat_from_token(start);
    }

    if (out == 0)
        out = cat_mask(Category::none);
    return out;
}

} // namespace rlog
