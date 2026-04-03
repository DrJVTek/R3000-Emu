#include "async_log.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace emu
{

// ─── Ring buffer entry ───────────────────────────────────────────────────────

static constexpr int kMaxTag = 20;
static constexpr int kMaxMsg = 220;

struct LogEntry
{
    uint64_t ts_ns;            // nanoseconds since epoch (for ordering)
    uint8_t  level;
    char     tag[kMaxTag];
    char     msg[kMaxMsg];
};

// ─── SPSC lock-free ring buffer ───────────────────────────────────────────────
// head_ is written by the producer (emulation thread).
// tail_ is written by the consumer (log thread).
// Both are read by both sides.

struct AsyncLogState
{
    // Ring buffer (heap-allocated, power-of-two size)
    LogEntry*                ring{nullptr};
    uint32_t                 mask{0};             // cap - 1

    std::atomic<uint32_t>    head{0};             // producer writes here
    std::atomic<uint32_t>    tail{0};             // consumer reads from here
    char                     _pad1[64 - 2*4]{};   // separate cache lines

    LogLevel                 max_level{LogLevel::info};

    // Consumer callback (if set, used instead of stderr)
    AsyncLogConsumer         consumer{nullptr};
    void*                    consumer_user{nullptr};

    // Consumer thread wakeup
    std::mutex               mtx;
    std::condition_variable  cv;
    bool                     running{false};
    std::thread              worker;

    // Entry drop counter (visible via stats)
    std::atomic<uint64_t>    dropped{0};
};

static AsyncLogState* g_alog = nullptr;

// ─── Consumer thread ─────────────────────────────────────────────────────────

static void consumer_loop(AsyncLogState* s)
{
    static const char* level_str[] = {"ERROR","WARN ","INFO ","DEBUG","TRACE"};

    while (true)
    {
        // Wait for entries or shutdown signal.
        {
            std::unique_lock<std::mutex> lk(s->mtx);
            s->cv.wait_for(lk, std::chrono::milliseconds(20),
                [s]{ return !s->running || s->head.load(std::memory_order_acquire)
                                          != s->tail.load(std::memory_order_relaxed); });
        }

        // Drain all available entries.
        while (true)
        {
            const uint32_t t = s->tail.load(std::memory_order_relaxed);
            const uint32_t h = s->head.load(std::memory_order_acquire);
            if (t == h)
                break;

            const LogEntry& e = s->ring[t & s->mask];

            if (s->consumer)
            {
                s->consumer(e.ts_ns, static_cast<LogLevel>(e.level),
                            e.tag, e.msg, s->consumer_user);
            }
            else
            {
                const uint8_t lvl = (e.level < 5) ? e.level : 4;
                std::fprintf(stderr, "[%s] [%s] %s\n",
                    level_str[lvl], e.tag, e.msg);
            }

            s->tail.store(t + 1, std::memory_order_release);
        }

        // Check shutdown after draining.
        if (!s->running &&
            s->head.load(std::memory_order_acquire) == s->tail.load(std::memory_order_relaxed))
            break;
    }

    std::fflush(stderr);
}

// ─── Producer-side callback (called by emu::logf) ────────────────────────────

static void async_log_cb(LogLevel level, const char* tag, const char* msg, void* /*user*/)
{
    AsyncLogState* s = g_alog;
    if (!s)
        return;

    const uint32_t h = s->head.load(std::memory_order_relaxed);
    const uint32_t t = s->tail.load(std::memory_order_acquire);

    // Drop if full (non-blocking).
    if (h - t > s->mask)
    {
        s->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    LogEntry& e  = s->ring[h & s->mask];
    e.ts_ns      = (uint64_t)std::chrono::steady_clock::now().time_since_epoch().count();
    e.level      = (uint8_t)level;

    const size_t tlen = tag ? std::strlen(tag) : 0;
    const size_t tcap = kMaxTag - 1;
    std::memcpy(e.tag, tag ? tag : "", (tlen < tcap ? tlen : tcap));
    e.tag[tlen < tcap ? tlen : tcap] = '\0';

    const size_t mlen = msg ? std::strlen(msg) : 0;
    const size_t mcap = kMaxMsg - 1;
    std::memcpy(e.msg, msg ? msg : "", (mlen < mcap ? mlen : mcap));
    e.msg[mlen < mcap ? mlen : mcap] = '\0';

    // Commit entry to ring (release).
    s->head.store(h + 1, std::memory_order_release);

    // Wake consumer every 64 entries to avoid too-frequent notifications.
    if ((h & 63u) == 0u)
        s->cv.notify_one();
}

// ─── Public API ──────────────────────────────────────────────────────────────

void async_log_init(LogLevel max_level, int ring_cap_bits,
                     AsyncLogConsumer consumer, void* consumer_user)
{
    if (g_alog)
        return; // already initialised

    if (ring_cap_bits < 10) ring_cap_bits = 10;
    if (ring_cap_bits > 17) ring_cap_bits = 17;
    const uint32_t cap = 1u << ring_cap_bits;

    auto* s = new AsyncLogState();
    s->ring          = new LogEntry[cap];
    s->mask          = cap - 1;
    s->max_level     = max_level;
    s->consumer      = consumer;
    s->consumer_user = consumer_user;
    s->running       = true;
    g_alog           = s;

    // Install as emu::Log sink.
    static emu::Log log_obj;
    log_obj.cb        = async_log_cb;
    log_obj.user      = nullptr;
    log_obj.max_level = max_level;
    emu::log_init(&log_obj);

    // Start consumer thread.
    s->worker = std::thread(consumer_loop, s);
}

void async_log_shutdown()
{
    AsyncLogState* s = g_alog;
    if (!s)
        return;

    // Signal shutdown and wake consumer.
    {
        std::lock_guard<std::mutex> lk(s->mtx);
        s->running = false;
    }
    s->cv.notify_all();

    if (s->worker.joinable())
        s->worker.join();

    const uint64_t dropped = s->dropped.load();
    if (dropped > 0)
        std::fprintf(stderr, "[WARN ] [ASYNCLOG] %llu entries dropped (ring full)\n",
            (unsigned long long)dropped);

    delete[] s->ring;
    delete s;
    g_alog = nullptr;
}

} // namespace emu
