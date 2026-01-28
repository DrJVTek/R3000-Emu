#pragma once

#include <cstdarg>
#include <cstdint>
#include <cstdio>

#include <chrono>

namespace flog
{

// Petit logger "FILE*" avec timestamp (ms) pour logs de debug.
// Objectif: logs séparés (CD/GPU/system) faciles à relire, sans bruit CPU/GTE.
enum class Level : uint8_t
{
    error = 0,
    warn = 1,
    info = 2,
    debug = 3,
    trace = 4,
};

struct Clock
{
    std::chrono::steady_clock::time_point t0;
};

struct Sink
{
    std::FILE* f{nullptr};
    Level level{Level::info};
};

static inline Clock clock_start()
{
    Clock c{};
    c.t0 = std::chrono::steady_clock::now();
    return c;
}

static inline uint64_t ms_since(const Clock& c)
{
    const auto now = std::chrono::steady_clock::now();
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - c.t0);
    return (uint64_t)dt.count();
}

void vlogf(const Sink& s, const Clock& c, Level lvl, const char* tag, const char* fmt, va_list args);
void logf(const Sink& s, const Clock& c, Level lvl, const char* tag, const char* fmt, ...);

} // namespace flog

