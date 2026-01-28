#pragma once

#include <cstdint>
#include <functional>

namespace audio
{

// Abstract interface for audio output
// Allows different backends: WAV file, SDL audio, UE5 streaming, etc.
class AudioOutput
{
  public:
    virtual ~AudioOutput() = default;

    // Initialize audio output
    // sample_rate: typically 44100
    // channels: 1 for mono, 2 for stereo
    // Returns true on success
    virtual bool init(int sample_rate, int channels) = 0;

    // Submit audio samples for playback
    // samples: interleaved PCM samples (16-bit signed)
    // count: number of samples (not bytes) - for stereo, L+R = 1 sample pair
    virtual void submit(const int16_t* samples, int count) = 0;

    // Flush any buffered audio
    virtual void flush() = 0;

    // Shutdown audio output
    virtual void shutdown() = 0;

    // Get current latency in samples (for sync)
    virtual int latency_samples() const { return 0; }
};

// Callback-based audio output for streaming to external systems (e.g., UE5)
// The callback receives batches of audio samples
class CallbackAudioOutput : public AudioOutput
{
  public:
    using Callback = std::function<void(const int16_t* samples, int count, int sample_rate)>;

    explicit CallbackAudioOutput(Callback cb) : callback_(std::move(cb)) {}

    bool init(int sample_rate, int channels) override
    {
        sample_rate_ = sample_rate;
        channels_ = channels;
        return true;
    }

    void submit(const int16_t* samples, int count) override
    {
        if (callback_)
            callback_(samples, count, sample_rate_);
    }

    void flush() override {}
    void shutdown() override {}

  private:
    Callback callback_;
    int sample_rate_{44100};
    int channels_{2};
};

// Null audio output (discards all samples)
class NullAudioOutput : public AudioOutput
{
  public:
    bool init(int, int) override { return true; }
    void submit(const int16_t*, int) override {}
    void flush() override {}
    void shutdown() override {}
};

} // namespace audio
