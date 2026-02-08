#include "R3000AudioComponent.h"

UR3000AudioComponent::UR3000AudioComponent(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer)
{
    NumChannels = 2;
}

bool UR3000AudioComponent::Init(int32& SampleRate)
{
    SampleRate = 44100;
    ResetBuffer(false);
    return true;
}

void UR3000AudioComponent::ResetBuffer(bool bZeroMemory)
{
    // Stop read-side first, then drop any written data.
    const uint64_t W = WritePos_.load(std::memory_order_acquire);
    ReadPos_.store(W, std::memory_order_release);

    if (bZeroMemory)
        FMemory::Memzero(Ring_, sizeof(Ring_));

    TotalPushedSamples_.store(0, std::memory_order_relaxed);
    TotalGeneratedSamples_.store(0, std::memory_order_relaxed);
    TotalDroppedSamples_.store(0, std::memory_order_relaxed);
    TotalSilenceSamples_.store(0, std::memory_order_relaxed);
}

void UR3000AudioComponent::PushSamples(const int16_t* Samples, int32 Count)
{
    // Count = number of int16 values (L,R,L,R,...).
    if (!Samples || Count <= 0)
        return;

    // If a huge burst arrives, keep the most recent window only.
    if (Count > kRingSize)
    {
        const int32 Skip = Count - kRingSize;
        Samples += Skip;
        Count = kRingSize;
    }

    const uint64_t W = WritePos_.load(std::memory_order_relaxed);
    const uint64_t R = ReadPos_.load(std::memory_order_acquire);
    const uint64_t NewW = W + static_cast<uint64_t>(Count);

    // If we'd overrun the ring, drop oldest by advancing ReadPos_.
    const uint64_t UsedAfter = NewW - R;
    if (UsedAfter > static_cast<uint64_t>(kRingSize))
    {
        const uint64_t NewR = NewW - static_cast<uint64_t>(kRingSize);
        const uint64_t Dropped = (NewR > R) ? (NewR - R) : 0;
        if (Dropped)
            TotalDroppedSamples_.fetch_add(Dropped, std::memory_order_relaxed);
        ReadPos_.store(NewR, std::memory_order_release);
    }

    for (int32 i = 0; i < Count; ++i)
    {
        const uint32_t Idx = static_cast<uint32_t>(W + static_cast<uint64_t>(i)) & (kRingSize - 1);
        Ring_[Idx] = Samples[i];
    }

    WritePos_.store(NewW, std::memory_order_release);
    TotalPushedSamples_.fetch_add((uint64_t)Count, std::memory_order_relaxed);
}

int32 UR3000AudioComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
    // NumSamples = number of float frames × NumChannels already accounted for by UE5.
    // Actually UE5 passes total float count = frames * channels.
    const uint64_t R = ReadPos_.load(std::memory_order_acquire);
    const uint64_t W = WritePos_.load(std::memory_order_acquire);
    const uint64_t Available64 = (W >= R) ? (W - R) : 0;
    const int32 Available = static_cast<int32>(FMath::Min<uint64_t>(Available64, static_cast<uint64_t>(kRingSize)));

    if (bMuted_ || Available == 0)
    {
        FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
        TotalGeneratedSamples_.fetch_add((uint64_t)NumSamples, std::memory_order_relaxed);
        TotalSilenceSamples_.fetch_add((uint64_t)NumSamples, std::memory_order_relaxed);
        return NumSamples;
    }

    const int32 ToRead = FMath::Min(Available, NumSamples);

    for (int32 i = 0; i < ToRead; ++i)
    {
        const uint32_t Idx = static_cast<uint32_t>(R + static_cast<uint64_t>(i)) & (kRingSize - 1);
        const float s = (static_cast<float>(Ring_[Idx]) / 32768.0f) * OutputGain;
        OutAudio[i] = FMath::Clamp(s, -1.0f, 1.0f);
    }

    // Zero-fill remainder (underrun).
    if (ToRead < NumSamples)
    {
        FMemory::Memzero(OutAudio + ToRead, (NumSamples - ToRead) * sizeof(float));
        TotalSilenceSamples_.fetch_add((uint64_t)(NumSamples - ToRead), std::memory_order_relaxed);
    }

    ReadPos_.store(R + static_cast<uint64_t>(ToRead), std::memory_order_release);
    TotalGeneratedSamples_.fetch_add((uint64_t)NumSamples, std::memory_order_relaxed);
    return NumSamples;
}

void UR3000AudioComponent::SetMuted(bool bMute)
{
    bMuted_ = bMute;
}
