#pragma once

// Logger "C+" : style C (printf/FILE*) avec juste assez de C++ (namespaces, enum class).
// Pas de streams, pas de std::string obligatoire, pas d'allocations à chaque log.

#include <cstdarg>
#include <cstdint>
#include <cstdio>

namespace rlog
{

enum class Level : uint8_t
{
    error = 0,
    warn = 1,
    info = 2,
    debug = 3,
    trace = 4,
};

enum class Category : uint32_t
{
    none = 0,
    fetch = 1u << 0,
    decode = 1u << 1,
    exec = 1u << 2,
    mem = 1u << 3,
    exc = 1u << 4,
    all = 0xFFFF'FFFFu,
};

static inline Category cat_or(Category a, Category b)
{
    return (Category)((uint32_t)a | (uint32_t)b);
}
static inline uint32_t cat_mask(Category c)
{
    return (uint32_t)c;
}

struct Logger
{
    FILE* out;
    Level level;
    uint32_t cats_mask;
};

static inline void logger_init(Logger* l, FILE* out)
{
    l->out = out ? out : stdout;
    l->level = Level::info;
    l->cats_mask = cat_mask(Category::all);
}

static inline void logger_set_level(Logger* l, Level lvl)
{
    l->level = lvl;
}
static inline void logger_set_cats(Logger* l, uint32_t cats_mask)
{
    l->cats_mask = cats_mask;
}

static inline int logger_enabled(const Logger* l, Level lvl, Category cat)
{
    if ((uint8_t)lvl > (uint8_t)l->level)
        return 0;
    return (l->cats_mask & cat_mask(cat)) != 0;
}

void logger_logf(Logger* l, Level lvl, Category cat, const char* fmt, ...);
void logger_vlogf(Logger* l, Level lvl, Category cat, const char* fmt, va_list args);

// Parsing simple (C-style) pour CLI.
Level parse_level(const char* s);
uint32_t parse_categories_csv(const char* s);

} // namespace rlog
