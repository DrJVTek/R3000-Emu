#pragma once

#include "Components/SynthComponent.h"
#include <atomic>
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

protected:
    virtual bool Init(int32& SampleRate) override;
    virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;

private:
    // Lock-free SPSC ring buffer (int16 stereo interleaved).
    static constexpr int32 kRingSize = 65536; // must be power of 2 (~0.37s stereo @ 44100Hz)
    int16_t Ring_[kRingSize]{};
    std::atomic<uint32> WritePos_{0};
    std::atomic<uint32> ReadPos_{0};

    bool bMuted_{false};
};
