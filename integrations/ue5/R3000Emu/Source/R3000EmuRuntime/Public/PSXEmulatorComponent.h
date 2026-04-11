#pragma once

#include "Components/ActorComponent.h"
#include "HAL/CriticalSection.h"
#include "HAL/Runnable.h"
#include "HAL/RunnableThread.h"
#include "Templates/Atomic.h"

// Core logging.
#include "log/logger.h"
#include "log/emu_log.h"

#include "PSXEmulatorComponent.generated.h"

class UInputAction;
class UInputMappingContext;
class UPSXAudioComponent;
class UPSX2DRenderComponent;
class UPSX3DRenderComponent;
class UPSXVideoSurfaceComponent;
class UPSXImageSurfaceComponent;
class UPSXSurfaceComponent;

namespace emu
{
class Core;
}

class FPSXEmuWorker;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnBiosPrint, const FString&, Line);

UENUM(BlueprintType)
enum class EPsx3dRefreshReason : uint8
{
    Manual UMETA(DisplayName = "Manual"),
    SceneChange UMETA(DisplayName = "Scene Change"),
    HighFallback2D UMETA(DisplayName = "High Fallback 2D"),
    NewGteCode UMETA(DisplayName = "New GTE Code")
};

UENUM(BlueprintType)
enum class EPsx3dRefreshScope : uint8
{
    Global UMETA(DisplayName = "Global"),
    CurrentPcRegion UMETA(DisplayName = "Current PC Region"),
    CurrentOtWindow UMETA(DisplayName = "Current OT Window")
};

UENUM(BlueprintType)
enum class ECDTimingMode : uint8
{
    Realistic UMETA(DisplayName = "Realistic"),
    CompatibilityFast UMETA(DisplayName = "Compatibility Fast")
};

UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXEmulatorComponent : public UActorComponent
{
    GENERATED_BODY()

  public:
    UPSXEmulatorComponent();
    virtual void BeginPlay() override;
    virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    UFUNCTION(BlueprintCallable, Category = "PSXEmu")
    void InitEmulator();

    UFUNCTION(BlueprintCallable, Category = "PSXEmu")
    bool ResetBiosBoot();

    UFUNCTION(BlueprintCallable, Category = "PSXEmu")
    int32 StepInstructions(int32 Steps);

    UFUNCTION(BlueprintCallable, Category = "PSXEmu")
    int32 GetProgramCounter() const;

    UFUNCTION(BlueprintCallable, Category = "PSXEmu")
    FString GetProgramCounterString() const;


    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    int64 GetStepsExecuted() const { return static_cast<int64>(StepsExecuted_.Load()); }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    int32 GetCyclesLastFrame() const { return CyclesLastFrame_.Load(); }

    /** Boots directly from ExePath instead of BIOS/CD. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bDevKitMode{false};

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    bool IsDevKitMode() const { return bDevKitMode; }

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    FString BiosPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    FString DiscPath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    FString ExePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D")
    FString Psx3dProfilePath;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D")
    bool bPsx3dAnalysisMode{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D")
    bool bPsx3dAnalysisEnabled{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D")
    bool bPsx3dRefreshOnInit{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D", meta = (EditCondition = "bPsx3dRefreshOnInit"))
    EPsx3dRefreshReason Psx3dRefreshReason{EPsx3dRefreshReason::Manual};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|PSX3D", meta = (EditCondition = "bPsx3dRefreshOnInit"))
    EPsx3dRefreshScope Psx3dRefreshScope{EPsx3dRefreshScope::Global};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    int32 StepsToRunOnBeginPlay{0};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bRunning{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu", meta = (ClampMin = "0.1", ClampMax = "4.0"))
    float EmulationSpeed{1.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu", meta = (ClampMin = "1", ClampMax = "50"))
    float BudgetMs{12.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    FString OutputDir;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    FString CoreLogLevel{TEXT("info")};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    FString CoreLogCats{TEXT("all")};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    FString EmuLogLevel{TEXT("info")};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    bool bTraceASM{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    bool bTraceIO{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Logs")
    bool bLoopDetectors{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bFastBoot{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bHleVectors{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu", meta = (ClampMin = "1", ClampMax = "128"))
    int32 BusTickBatch{1};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|CD")
    ECDTimingMode CDTimingMode{ECDTimingMode::CompatibilityFast};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu", meta = (ClampMin = "1", ClampMax = "10"))
    int32 CycleMultiplier{2};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bThreadedMode{true};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu")
    bool bAudioDrivenTiming{false};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu", meta = (ClampMin = "10", ClampMax = "200"))
    float AudioBufferTargetMs{50.0f};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Debug", meta = (ClampMin = "0", ClampMax = "50000000"))
    int32 PcSampleIntervalSteps{5000000};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Debug")
    bool bLogAudioStats{true};

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Debug", meta = (ClampMin = "0.1", ClampMax = "10.0"))
    float AudioStatsIntervalSec{1.0f};

    UPROPERTY(BlueprintAssignable, Category = "PSXEmu")
    FOnBiosPrint OnBiosPrint;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputMappingContext> PadMappingContext;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadCross;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadCircle;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadTriangle;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadSquare;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadUp;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadDown;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadLeft;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadRight;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadStart;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadSelect;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadL1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadR1;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadL2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadR2;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadL3;

    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|Input")
    TObjectPtr<UInputAction> IA_PadR3;

    // Routing struct for emu::logf callback (tag → file).
    struct EmuLogFiles
    {
        std::FILE* spu{nullptr};
        std::FILE* sys{nullptr};
    };

    // Thread-safe accessors for worker thread
    emu::Core* GetCore() const { return Core_; }
    UPSXAudioComponent* GetAudioComp() const { return AudioComp_; }
    bool IsRunning() const { return bRunning; }
    bool IsAudioDrivenTiming() const { return bAudioDrivenTiming; }
    float GetAudioBufferTargetMs() const { return AudioBufferTargetMs; }
    int32 GetCycleMultiplier() const { return CycleMultiplier; }
    int32 GetPcSampleIntervalSteps() const { return PcSampleIntervalSteps; }

    /** Get the 2D GPU component (may be null before core init or if not present on actor). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    UPSX2DRenderComponent* GetGpuComponent() const { return GpuComp_; }

    /** Get the 3D GPU component (may be null if not present on actor). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    UPSX3DRenderComponent* GetGpu3DComponent() const { return Gpu3DComp_; }

    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    UPSXSurfaceComponent* GetSurfaceComponent() const { return SurfaceComp_; }

    /** Check if the GPU component is ready (bound and valid). */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu")
    bool IsGpuReady() const { return GpuComp_ != nullptr; }

    // Thread-safe stats update (called from worker thread)
    void UpdateStepsExecuted(uint64 Steps, uint64 Cycles);

  private:
    friend class FPSXEmuWorker;

    bool BootBiosInternal();
    void StopWorkerThread();
    void SetupPadInput();
    void PollPadInput();
    bool bPadMappingAdded_{false};
    bool bPawnInputDisabled_{false};

    emu::Core* Core_{nullptr};
    TAtomic<uint64> StepsExecuted_{0};
    UPSXAudioComponent* AudioComp_{nullptr};
    UPSX2DRenderComponent* GpuComp_{nullptr};
    UPSX3DRenderComponent* Gpu3DComp_{nullptr};
    UPSXVideoSurfaceComponent* VideoComp_{nullptr};
    UPSXImageSurfaceComponent* ImageComp_{nullptr};
    UPSXSurfaceComponent* SurfaceComp_{nullptr};
    TAtomic<int32> CyclesLastFrame_{0};
    TArray<uint8> BiosBytes_{};

    // Worker thread for threaded emulation mode
    FPSXEmuWorker* EmuWorker_{nullptr};
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
