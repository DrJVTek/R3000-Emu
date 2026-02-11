#pragma once

#include "Components/ActorComponent.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Templates/Atomic.h"

// R3000-Emu loggers (repo code).
#include "log/logger.h"
#include "log/emu_log.h"

#include "R3000EmuComponent.generated.h"

class UR3000AudioComponent;
class UR3000GpuComponent;

namespace emu
{
class Core;
}

// Forward declare the worker thread class.
class FR3000EmuWorker;

// Delegate fired when the BIOS prints a complete line via putchar (B(3Dh)).
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBiosPrint, const FString&, Line);

UCLASS(ClassGroup = (R3000Emu), meta = (BlueprintSpawnableComponent))
class UR3000EmuComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    UR3000EmuComponent();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    // Call this from BP BeginPlay AFTER setting BiosPath/DiscPath/OutputDir/etc.
    UFUNCTION(BlueprintCallable, Category = "R3000Emu")
    void InitEmulator();

    UFUNCTION(BlueprintCallable, Category = "R3000Emu")
    bool ResetBiosBoot();

    UFUNCTION(BlueprintCallable, Category = "R3000Emu")
    int32 StepInstructions(int32 Steps);

    UFUNCTION(BlueprintCallable, Category = "R3000Emu")
    int32 GetProgramCounter() const;

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu")
    int64 GetStepsExecuted() const { return static_cast<int64>(StepsExecuted_.Load()); }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu")
    int32 GetCyclesLastFrame() const { return CyclesLastFrame_.Load(); }

    // Optional: BIOS path to boot on BeginPlay.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    FString BiosPath;

    // Optional: CD image path to insert after init.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    FString DiscPath;

    // Optional: run N steps on BeginPlay (0 = don't run).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    int32 StepsToRunOnBeginPlay{0};

    // Start/stop emulation each frame.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    bool bRunning{false};

    // Speed multiplier (1.0 = real-time PS1 speed).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "0.1", ClampMax = "4.0"))
    float EmulationSpeed{1.0f};

    // Max milliseconds to spend in TickComponent.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "1", ClampMax = "50"))
    float BudgetMs{12.0f};

    // Optional: directory for file logs (CD/GPU/SYS/IO/SPU + core logger).
    // If empty, only UE Output Log will be used.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    FString OutputDir;

    // Core logger level (rlog): error|warn|info|debug|trace
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    FString CoreLogLevel{TEXT("info")};

    // Core logger categories CSV (rlog): fetch,decode,exec,mem,exc,all
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    FString CoreLogCats{TEXT("all")};

    // emu::logf level (error|warn|info|debug|trace) - component logging (GPU, CD, SPU, etc.)
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    FString EmuLogLevel{TEXT("debug")};

    // Enable ASM disassembly trace (very verbose, needs OutputDir).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    bool bTraceASM{false};

    // Enable MMIO I/O trace.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    bool bTraceIO{false};

    // Enable CPU loop detectors (one-shot debug dumps when known loops are hit).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Logs")
    bool bLoopDetectors{false};

    // Fast boot: skip BIOS, load game EXE directly from CD (requires DiscPath).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    bool bFastBoot{false};

    // HLE vectors: intercept BIOS exception handler (0x80000080) and syscalls (A0/B0/C0).
    // ON = HLE handles IRQs, VSync events, CDROM callbacks (simpler but less accurate)
    // OFF = Real BIOS exception handler runs (more accurate but needs precise HW emulation)
    // Try OFF first - if boot hangs, switch to ON.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    bool bHleVectors{false};

    // Bus tick batching: tick hardware every N CPU steps instead of every step.
    // 1 = cycle-accurate (recommended with threaded mode), 32 = fast, 64 = faster but less accurate.
    // With threaded mode enabled, BusTickBatch=1 is recommended for accurate timing.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "1", ClampMax = "128"))
    int32 BusTickBatch{1};

    // Cycle multiplier: cycles counted per CPU instruction.
    // 1 = simplified (original), 2 = approximate real R3000, higher = slower game.
    // If audio is too short compared to real hardware, increase this value.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "1", ClampMax = "10"))
    int32 CycleMultiplier{1};

    // THREADED MODE (Recommended): Run emulation on a dedicated worker thread.
    // This uses Windows waitable timers for precise PS1 timing (33.8688 MHz).
    // Main UE5 thread only reads GPU state for rendering.
    // This allows BusTickBatch=1 (cycle-accurate) without frame drops.
    // When OFF, emulation runs in TickComponent (legacy mode).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    bool bThreadedMode{true};

    // Audio-driven timing: pace emulation to audio sample consumption.
    // WARNING: Only works if the emulator runs faster than real-time!
    // If the emulator is slower than real-time, use wall-clock timing instead.
    // Recommended: OFF (use wall-clock timing via waitable timer).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu")
    bool bAudioDrivenTiming{false};

    // Target audio buffer size in milliseconds for audio-driven mode.
    // Higher = more latency but smoother; Lower = less latency but may stutter.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "10", ClampMax = "200"))
    float AudioBufferTargetMs{50.0f};

    // Periodically log PC progress to system.log (helps diagnose "stuck boot").
    // 0 disables. Value is in executed steps (instructions).
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Debug", meta = (ClampMin = "0", ClampMax = "50000000"))
    int32 PcSampleIntervalSteps{5000000};

    // Periodically log UE audio ring-buffer stats to system.log (helps diagnose "no sound").
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Debug")
    bool bLogAudioStats{true};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu|Debug", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float AudioStatsIntervalSec{1.0f};

    // Event fired when the BIOS/game prints a complete line via putchar.
    UPROPERTY(BlueprintAssignable, Category = "R3000Emu")
    FOnBiosPrint OnBiosPrint;

    // Routing struct for emu::logf callback (tag → file).
    struct EmuLogFiles
    {
        std::FILE* spu{nullptr};
        std::FILE* sys{nullptr};
    };

    // Thread-safe accessors for worker thread
    emu::Core* GetCore() const { return Core_; }
    UR3000AudioComponent* GetAudioComp() const { return AudioComp_; }
    bool IsRunning() const { return bRunning; }
    bool IsAudioDrivenTiming() const { return bAudioDrivenTiming; }
    float GetAudioBufferTargetMs() const { return AudioBufferTargetMs; }
    int32 GetCycleMultiplier() const { return CycleMultiplier; }
    int32 GetPcSampleIntervalSteps() const { return PcSampleIntervalSteps; }

    /** Get the GPU component (may be null before core init or if not present on actor). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu")
    UR3000GpuComponent* GetGpuComponent() const { return GpuComp_; }

    /** Check if the GPU component is ready (bound and valid). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu")
    bool IsGpuReady() const { return GpuComp_ != nullptr; }

    // Thread-safe stats update (called from worker thread)
    void UpdateStepsExecuted(uint64 Steps, uint64 Cycles);

  private:
    friend class FR3000EmuWorker;

    bool BootBiosInternal();
    void StopWorkerThread();

    emu::Core* Core_{nullptr};
    TAtomic<uint64> StepsExecuted_{0};
    UR3000AudioComponent* AudioComp_{nullptr};
    UR3000GpuComponent* GpuComp_{nullptr};
    TAtomic<int32> CyclesLastFrame_{0};
    TArray<uint8> BiosBytes_{};

    // Worker thread for threaded emulation mode
    FR3000EmuWorker* EmuWorker_{nullptr};
    FRunnableThread* EmuThread_{nullptr};

    // Critical section for GPU state synchronization
    // (GPU reads from UE5 main thread, writes from emu worker thread)
    mutable FCriticalSection GpuStateLock_;

    // File sinks (optional).
    std::FILE* CoreLogFile_{nullptr};
    std::FILE* CdLogFile_{nullptr};
    std::FILE* GpuLogFile_{nullptr};
    std::FILE* SysLogFile_{nullptr};
    std::FILE* IoLogFile_{nullptr};
    std::FILE* SpuLogFile_{nullptr};
    std::FILE* TextLogFile_{nullptr};

    EmuLogFiles EmuLogFiles_{};

    rlog::Logger CoreLogger_{};
    emu::Log EmuLog_{};

    // BIOS putchar line buffer → fires OnBiosPrint on newline.
    // PutcharCB runs on worker thread, so we queue lines and broadcast from game thread.
    FString PutcharLineBuf_{};
    TArray<FString> PutcharPendingLines_{};
    FCriticalSection PutcharLock_{};
    static void PutcharCB(char Ch, void* User);

    uint64 NextPcSampleAt_{0};
    double NextAudioStatsTime_{0.0};

    // Audio-driven timing state (accessed atomically in threaded mode)
    TAtomic<uint64> TotalCyclesExecuted_{0};      // Total CPU cycles executed since start
    TAtomic<uint64> LastAudioSamplesConsumed_{0}; // Last observed audio sample count

    // Thread control flags
    TAtomic<bool> bWorkerShouldStop_{false};
    TAtomic<bool> bWorkerPaused_{false};
};

