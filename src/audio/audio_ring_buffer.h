#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>

namespace audio
{

// Lock-free Single Producer Single Consumer ring buffer for audio samples.
// Producer (SPU/XA thread) writes, consumer (UE5 audio callback) reads.
// No mutex, no lock — just two atomic indices.
class AudioRingBuffer
{
  public:
    // Capacity in sample PAIRS (L+R). Must be power of 2.
    static constexpr uint32_t kCapacity = 8192;
    static constexpr uint32_t kMask = kCapacity - 1;

    AudioRingBuffer() = default;

    // Producer: write sample pairs. Returns number of pairs actually written.
    uint32_t write(const int16_t* left, const int16_t* right, uint32_t count)
    {
        const uint32_t w = write_pos_.load(std::memory_order_relaxed);
        const uint32_t r = read_pos_.load(std::memory_order_acquire);
        const uint32_t available = kCapacity - (w - r);
        const uint32_t to_write = (count < available) ? count : available;

        for (uint32_t i = 0; i < to_write; ++i)
        {
            const uint32_t idx = (w + i) & kMask;
            buf_l_[idx] = left[i];
            buf_r_[idx] = right[i];
        }

        write_pos_.store(w + to_write, std::memory_order_release);
        return to_write;
    }

    // Producer: write a single sample pair.
    bool write_one(int16_t left, int16_t right)
    {
        const uint32_t w = write_pos_.load(std::memory_order_relaxed);
        const uint32_t r = read_pos_.load(std::memory_order_acquire);
        if ((w - r) >= kCapacity) return false; // full

        const uint32_t idx = w & kMask;
        buf_l_[idx] = left;
        buf_r_[idx] = right;
        write_pos_.store(w + 1, std::memory_order_release);
        return true;
    }

    // Consumer: read sample pairs. Returns number of pairs actually read.
    uint32_t read(int16_t* left, int16_t* right, uint32_t count)
    {
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);
        const uint32_t w = write_pos_.load(std::memory_order_acquire);
        const uint32_t available = w - r;
        const uint32_t to_read = (count < available) ? count : available;

        for (uint32_t i = 0; i < to_read; ++i)
        {
            const uint32_t idx = (r + i) & kMask;
            left[i] = buf_l_[idx];
            right[i] = buf_r_[idx];
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    // Consumer: read interleaved (L,R,L,R,...) for UE5 audio buffers.
    uint32_t read_interleaved(int16_t* out, uint32_t max_pairs)
    {
        const uint32_t r = read_pos_.load(std::memory_order_relaxed);
        const uint32_t w = write_pos_.load(std::memory_order_acquire);
        const uint32_t available = w - r;
        const uint32_t to_read = (max_pairs < available) ? max_pairs : available;

        for (uint32_t i = 0; i < to_read; ++i)
        {
            const uint32_t idx = (r + i) & kMask;
            out[i * 2 + 0] = buf_l_[idx];
            out[i * 2 + 1] = buf_r_[idx];
        }

        read_pos_.store(r + to_read, std::memory_order_release);
        return to_read;
    }

    uint32_t available() const
    {
        return write_pos_.load(std::memory_order_acquire) -
               read_pos_.load(std::memory_order_relaxed);
    }

    void reset()
    {
        write_pos_.store(0, std::memory_order_relaxed);
        read_pos_.store(0, std::memory_order_relaxed);
    }

  private:
    int16_t buf_l_[kCapacity]{};
    int16_t buf_r_[kCapacity]{};
    std::atomic<uint32_t> write_pos_{0};
    std::atomic<uint32_t> read_pos_{0};
};

} // namespace audio
