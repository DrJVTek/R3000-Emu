#pragma once

#include "Components/ActorComponent.h"

// R3000-Emu loggers (repo code).
#include "log/logger.h"
#include "log/emu_log.h"

#include "R3000EmuComponent.generated.h"

class UR3000AudioComponent;

namespace emu
{
class Core;
}

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
    int64 GetStepsExecuted() const { return static_cast<int64>(StepsExecuted_); }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "R3000Emu")
    int32 GetCyclesLastFrame() const { return CyclesLastFrame_; }

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

    // Bus tick batching: tick hardware every N CPU steps instead of every step.
    // 1 = cycle-accurate (slow), 32 = fast (good default for UE5), 64 = faster but less accurate.
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (ClampMin = "1", ClampMax = "128"))
    int32 BusTickBatch{1};

    // Routing struct for emu::logf callback (tag → file).
    struct EmuLogFiles
    {
        std::FILE* spu{nullptr};
        std::FILE* sys{nullptr};
    };

  private:
    bool BootBiosInternal();

    emu::Core* Core_{nullptr};
    uint64 StepsExecuted_{0};
    UR3000AudioComponent* AudioComp_{nullptr};
    int32 CyclesLastFrame_{0};
    TArray<uint8> BiosBytes_{};

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
};

