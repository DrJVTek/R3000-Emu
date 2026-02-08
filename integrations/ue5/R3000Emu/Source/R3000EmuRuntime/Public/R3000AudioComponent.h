#pragma once

#include "Components/SynthComponent.h"
#include <atomic>
#include <cstdint>
#include "R3000AudioComponent.generated.h"

/**
 * Streams PS1 SPU audio to UE5's AudioMixer via a lock-free ring buffer.
 * Place on the same Actor as UR3000EmuComponent.
 */
UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000AudioComponent : public USynthComponent
{
    GENERATED_BODY()

public:
    UR3000AudioComponent(const FObjectInitializer& ObjectInitializer = FObjectInitializer::Get());

    /** Called by SPU callback (game thread). Interleaved stereo int16. */
    void PushSamples(const int16_t* Samples, int32 Count);

    UFUNCTION(BlueprintCallable, Category = "R3000Emu|Audio")
    void SetMuted(bool bMute);

    UFUNCTION(BlueprintCallable, Category = "R3000Emu|Audio")
    bool IsMuted() const { return bMuted_; }

    // Clears buffered audio so old samples (e.g. BIOS jingle) cannot replay on next run.
    UFUNCTION(BlueprintCallable, Category = "R3000Emu|Audio")
    void ResetBuffer(bool bZeroMemory = false);

    // Output gain applied to generated audio (post int16->float conversion).
    // Use this to compensate for low SPU mix levels without touching emulation.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Audio", meta = (ClampMin = "0.0", ClampMax = "8.0"))
    float OutputGain{4.0f};

    // Debug counters (thread-safe). Counts are in *samples* (int16 for pushed, float for generated).
    uint64 GetTotalPushedSamples() const { return TotalPushedSamples_.load(std::memory_order_relaxed); }
    uint64 GetTotalGeneratedSamples() const { return TotalGeneratedSamples_.load(std::memory_order_relaxed); }
    uint64 GetTotalDroppedSamples() const { return TotalDroppedSamples_.load(std::memory_order_relaxed); }
    uint64 GetTotalSilenceSamples() const { return TotalSilenceSamples_.load(std::memory_order_relaxed); }
    uint32 GetBufferedSamples() const
    {
        const uint64_t R = ReadPos_.load(std::memory_order_acquire);
        const uint64_t W = WritePos_.load(std::memory_order_acquire);
        const uint64_t Avail64 = (W >= R) ? (W - R) : 0;
        const uint64_t AvailClamped = (Avail64 > (uint64_t)kRingSize) ? (uint64_t)kRingSize : Avail64;
        return (uint32)AvailClamped;
    }

protected:
    virtual bool Init(int32& SampleRate) override;
    virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;

private:
    // Lock-free SPSC ring buffer (int16 stereo interleaved).
    static constexpr int32 kRingSize = 65536; // must be power of 2 (~0.37s stereo @ 44100Hz)
    int16_t Ring_[kRingSize]{};
    // Use monotonic counters (not masked indices) so we can detect overrun safely.
    std::atomic<uint64_t> WritePos_{0};
    std::atomic<uint64_t> ReadPos_{0};

    std::atomic<uint64_t> TotalPushedSamples_{0};
    std::atomic<uint64_t> TotalGeneratedSamples_{0};
    std::atomic<uint64_t> TotalDroppedSamples_{0};
    std::atomic<uint64_t> TotalSilenceSamples_{0};

    bool bMuted_{false};
};
