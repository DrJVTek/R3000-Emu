#include "R3000AudioComponent.h"

UR3000AudioComponent::UR3000AudioComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    NumChannels = 2;
}

bool UR3000AudioComponent::Init(int32& SampleRate)
{
    SampleRate = 44100;
    return true;
}

void UR3000AudioComponent::PushSamples(const int16_t* Samples, int32 Count)
{
    // Count = number of int16 values (L,R,L,R,...).
    const uint32 W = WritePos_.load(std::memory_order_relaxed);
    const uint32 R = ReadPos_.load(std::memory_order_acquire);

    for (int32 i = 0; i < Count; ++i)
    {
        const uint32 Next = (W + i) & (kRingSize - 1);
        // If full, drop oldest samples (overwrite).
        Ring_[Next] = Samples[i];
    }
    WritePos_.store((W + Count) & (kRingSize - 1), std::memory_order_release);
}

int32 UR3000AudioComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
    // NumSamples = number of float frames × NumChannels already accounted for by UE5.
    // Actually UE5 passes total float count = frames * channels.
    const uint32 W = WritePos_.load(std::memory_order_acquire);
    const uint32 R = ReadPos_.load(std::memory_order_relaxed);

    const uint32 Available = (W - R) & (kRingSize - 1);

    if (bMuted_ || Available == 0)
    {
        FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
        return NumSamples;
    }

    const int32 ToRead = FMath::Min((int32)Available, NumSamples);

    for (int32 i = 0; i < ToRead; ++i)
    {
        const uint32 Idx = (R + i) & (kRingSize - 1);
        OutAudio[i] = static_cast<float>(Ring_[Idx]) / 32768.0f;
    }

    // Zero-fill remainder (underrun).
    if (ToRead < NumSamples)
    {
        FMemory::Memzero(OutAudio + ToRead, (NumSamples - ToRead) * sizeof(float));
    }

    ReadPos_.store((R + ToRead) & (kRingSize - 1), std::memory_order_release);
    return NumSamples;
}

void UR3000AudioComponent::SetMuted(bool bMute)
{
    bMuted_ = bMute;
}
