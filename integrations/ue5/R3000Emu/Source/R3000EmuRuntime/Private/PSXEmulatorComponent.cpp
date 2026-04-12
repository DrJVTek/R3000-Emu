#include "PSXEmulatorComponent.h"
#include "PSXAudioComponent.h"
#include "PSX2DRenderComponent.h"
#include "PSX3DRenderComponent.h"
#include "PSXVramViewerComponent.h"
#include "PSXVideoSurfaceComponent.h"
#include "PSXImageSurfaceComponent.h"
#include "PSXSurfaceComponent.h"

#include "Logging/LogMacros.h"
#include "Containers/StringConv.h"
#include "EnhancedInputSubsystems.h"
#include "GameFramework/PlayerController.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "InputAction.h"
#include "InputMappingContext.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstring>

#include "emu/core.h"
#include "emu/hooks.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"
#include "audio/spu.h"
#include "loader/loader.h"
#include "log/async_log.h"
#include "log/filelog.h"
#include "util/file_util.h"

using util::fopen_utf8;

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPSXEmu, Log, All);

static constexpr double kPS1CpuClock = 33868800.0;

static uint32 EffectiveBusTickBatch(bool bThreadedMode, int32 RequestedBusTickBatch)
{
    return static_cast<uint32>(FMath::Clamp(RequestedBusTickBatch, 1, 128));
}

static int EffectiveCdTimingMode(ECDTimingMode Mode)
{
    return (Mode == ECDTimingMode::Realistic) ? 0 : 1;
}

static const TCHAR* LexToString(ECDTimingMode Mode)
{
    switch (Mode)
    {
    case ECDTimingMode::Realistic: return TEXT("realistic");
    case ECDTimingMode::CompatibilityFast: return TEXT("compatibility-fast");
    default: return TEXT("compatibility-fast");
    }
}

static flog::Level ToFileLogLevel(emu::LogLevel Level)
{
    switch (Level)
    {
    case emu::LogLevel::error: return flog::Level::error;
    case emu::LogLevel::warn: return flog::Level::warn;
    case emu::LogLevel::info: return flog::Level::info;
    case emu::LogLevel::debug: return flog::Level::debug;
    case emu::LogLevel::trace: return flog::Level::trace;
    default: return flog::Level::info;
    }
}

class FPSXEmuWorker : public FRunnable
{
public:
    FPSXEmuWorker(UPSXEmulatorComponent* InOwner)
        : Owner(InOwner)
#if PLATFORM_WINDOWS
        , WaitableTimer(nullptr)
#endif
    {
#if PLATFORM_WINDOWS
        WaitableTimer = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!WaitableTimer)
        {
            WaitableTimer = CreateWaitableTimerW(nullptr, false, nullptr);
        }
        if (WaitableTimer)
        {
            emu::logf(emu::LogLevel::info, "CORE", "Worker thread created with Windows waitable timer");
        }
        else
        {
            emu::logf(emu::LogLevel::info, "CORE", "Worker thread: failed to create waitable timer, using Sleep fallback");
        }
#endif
    }

    virtual ~FPSXEmuWorker()
    {
#if PLATFORM_WINDOWS
        if (WaitableTimer)
        {
            CloseHandle(WaitableTimer);
            WaitableTimer = nullptr;
        }
#endif
    }

    virtual bool Init() override
    {
        return true;
    }

    virtual uint32 Run() override
    {
        emu::logf(emu::LogLevel::debug, "CORE", "Worker Run() entered");

        uint64 LocalTotalCycles = 0;
        uint64 LocalSteps = 0;
        uint64 NextPcSampleAt = (Owner->GetPcSampleIntervalSteps() > 0)
            ? static_cast<uint64>(Owner->GetPcSampleIntervalSteps()) : 0;

        double CycleDebt = 0.0;
        double LastTime = FPlatformTime::Seconds();
        double LastVblTime = LastTime;

        constexpr double kVblPeriod = 1.0 / 50.0; // PAL

        double StatsTime = LastTime;
        uint64 StatsCycles = 0;
        uint64 StatsSteps = 0;
        uint32 StatsVBlanks = 0;

        {
            r3000::Bus* Bus = Owner->GetCore() ? Owner->GetCore()->bus() : nullptr;
            if (Bus)
                Bus->set_external_vblank(true);
        }

        while (!Owner->bWorkerShouldStop_.Load())
        {
            if (!Owner->IsRunning() || Owner->bWorkerPaused_.Load())
            {
                FPlatformProcess::Sleep(0.001f);
                LastTime = FPlatformTime::Seconds();
                LastVblTime = LastTime;
                CycleDebt = 0.0;
                continue;
            }

            emu::Core* Core = Owner->GetCore();
            if (!Core)
            {
                FPlatformProcess::Sleep(0.001f);
                LastTime = FPlatformTime::Seconds();
                LastVblTime = LastTime;
                CycleDebt = 0.0;
                continue;
            }

            const double Now = FPlatformTime::Seconds();
            double DeltaTime = Now - LastTime;
            LastTime = Now;

            if (DeltaTime > 0.05)
                DeltaTime = 0.05;

            CycleDebt += DeltaTime * kPS1CpuClock;

            {
                r3000::Bus* Bus = Core->bus();
                uint32 PeriphAccum = 0;
                constexpr uint32 kPeriphBatch = 1024; // match CLI for timing parity

                while (CycleDebt > 0.0 && !Owner->bWorkerShouldStop_.Load())
                {
                    const auto Res = Core->step();
                    if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
                    {
                        emu::logf(emu::LogLevel::warn, "CORE",
                            "Worker: emu stopped kind=%d pc=0x%08X",
                            (int)Res.kind, Res.pc);
                        Owner->bWorkerPaused_.Store(true);
                        break;
                    }

                    const uint32 Cycles = Core->last_cycles();
                    CycleDebt -= static_cast<double>(Cycles);
                    LocalTotalCycles += Cycles;
                    ++LocalSteps;
                    PeriphAccum += Cycles;

                    if (PeriphAccum >= kPeriphBatch)
                    {
                        if (Bus)
                            Bus->tick_peripherals(PeriphAccum);
                        PeriphAccum = 0;
                    }
                }

                if (Bus && PeriphAccum > 0)
                    Bus->tick_peripherals(PeriphAccum);
            }

            Owner->UpdateStepsExecuted(LocalSteps, LocalTotalCycles);

            {
                const double StatsNow = FPlatformTime::Seconds();
                const double StatsElapsed = StatsNow - StatsTime;
                if (StatsElapsed >= 2.0)
                {
                    const uint64 DeltaCycles = LocalTotalCycles - StatsCycles;
                    const uint64 DeltaSteps = LocalSteps - StatsSteps;
                    const double CyclesPerSec = DeltaCycles / StatsElapsed;
                    const double CPI = (DeltaSteps > 0) ? (double)DeltaCycles / (double)DeltaSteps : 0.0;
                    const double SpeedPct = (CyclesPerSec / kPS1CpuClock) * 100.0;

                    // Count VBlanks from bus
                    r3000::Bus* Bus = Core->bus();
                    const uint32 CurVBlanks = Bus ? Bus->vblank_count() : 0;
                    const uint32 DeltaVBlanks = CurVBlanks - StatsVBlanks;
                    const double VBlanksPerSec = DeltaVBlanks / StatsElapsed;

                    emu::logf(emu::LogLevel::warn, "CORE",
                        "TIMING: %.1f%% speed | %.2fM cyc/s (target %.2fM) | CPI=%.2f | %.1f VBl/s (target 60) | debt=%.0f cyc | steps=%llu",
                        SpeedPct,
                        CyclesPerSec / 1e6, kPS1CpuClock / 1e6,
                        CPI,
                        VBlanksPerSec,
                        CycleDebt,
                        (unsigned long long)LocalSteps);

                    StatsTime = StatsNow;
                    StatsCycles = LocalTotalCycles;
                    StatsSteps = LocalSteps;
                    StatsVBlanks = CurVBlanks;
                }
            }

            // PC sample logging
            if (NextPcSampleAt != 0 && LocalSteps >= NextPcSampleAt)
            {
                r3000::Cpu* Cpu = Core->cpu();
                r3000::Bus* Bus = Core->bus();
                const uint32 IStat = Bus ? Bus->irq_stat_raw() : 0u;
                const uint32 IMask = Bus ? Bus->irq_mask_raw() : 0u;

                emu::logf(emu::LogLevel::info, "CORE",
                    "Worker PC sample steps=%llu pc=0x%08X total_cycles=%llu i_stat=0x%08X i_mask=0x%08X",
                    (unsigned long long)LocalSteps,
                    (unsigned)Core->pc(),
                    (unsigned long long)LocalTotalCycles,
                    (unsigned)IStat,
                    (unsigned)IMask);

                const uint64 StepInterval = static_cast<uint64>(FMath::Max(Owner->GetPcSampleIntervalSteps(), 1));
                while (NextPcSampleAt != 0 && NextPcSampleAt <= LocalSteps)
                    NextPcSampleAt += StepInterval;
            }

            // Flush SPU samples
            r3000::Bus* Bus = Core->bus();
            audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
            if (Spu)
                Spu->flush_audio();

            // If ahead of real time (negative debt), sleep to yield CPU
            if (CycleDebt < 0.0)
            {
                const double AheadSeconds = -CycleDebt / kPS1CpuClock;
                if (AheadSeconds > 0.0001) // > 100us ahead
                    WaitPrecise(FMath::Min(AheadSeconds, 0.002));
            }
        }

        emu::logf(emu::LogLevel::debug, "CORE", "Emulation worker thread exiting");
        return 0;
    }

    virtual void Stop() override
    {
        // Called from another thread to signal stop
    }

private:
    void WaitPrecise(double Seconds)
    {
#if PLATFORM_WINDOWS
        if (WaitableTimer && Seconds > 0.0)
        {
            // Convert to 100ns units (negative = relative time)
            LARGE_INTEGER DueTime;
            DueTime.QuadPart = -static_cast<LONGLONG>(Seconds * 10000000.0);

            if (SetWaitableTimer(WaitableTimer, &DueTime, 0, nullptr, nullptr, false))
            {
                WaitForSingleObject(WaitableTimer, INFINITE);
                return;
            }
        }
#endif
        // Fallback to Sleep
        if (Seconds > 0.0001)
            FPlatformProcess::Sleep(static_cast<float>(Seconds));
    }

    UPSXEmulatorComponent* Owner;
#if PLATFORM_WINDOWS
    HANDLE WaitableTimer;
#endif
};

// rlog callback → UE_LOG (for CPU ASM trace when no file output)
static void UERlogCallback(rlog::Level Level, rlog::Category /*Cat*/, const char* Msg, void* /*User*/)
{
    switch (Level)
    {
    case rlog::Level::error: UE_LOG(LogPSXEmu, Error,       TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::warn:  UE_LOG(LogPSXEmu, Warning,     TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::info:  UE_LOG(LogPSXEmu, Log,         TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::debug: UE_LOG(LogPSXEmu, Verbose,     TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::trace: UE_LOG(LogPSXEmu, VeryVerbose, TEXT("[CPU] %hs"), Msg); break;
    }
}

// emu::logf async output — called from the async_log consumer thread.
// UE_LOG is thread-safe.  File writes happen on the log thread, not the emu thread.
// User pointer is EmuLogFiles*.
static void UEAsyncLogOutput(uint64_t /*ts_ns*/, emu::LogLevel Level,
                              const char* Tag, const char* Msg, void* User)
{
    switch (Level)
    {
    case emu::LogLevel::error: UE_LOG(LogPSXEmu, Error,       TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::warn:  UE_LOG(LogPSXEmu, Warning,     TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::info:  UE_LOG(LogPSXEmu, Log,         TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::debug: UE_LOG(LogPSXEmu, Verbose,     TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::trace: UE_LOG(LogPSXEmu, VeryVerbose, TEXT("[%hs] %hs"), Tag, Msg); break;
    }

    if (!User || !Tag)
        return;
    const auto* Files = static_cast<const UPSXEmulatorComponent::EmuLogFiles*>(User);
    if (Files->spu && std::strcmp(Tag, "SPU") == 0)
    {
        std::fprintf(Files->spu, "[%hs] %hs\n", Tag, Msg);
        std::fflush(Files->spu);
    }
    if (Files->sys && (
            std::strcmp(Tag, "CPU") == 0 ||
            std::strcmp(Tag, "BUS") == 0 ||
            std::strcmp(Tag, "CORE") == 0 ||
            std::strcmp(Tag, "HOOK") == 0 ||
            std::strcmp(Tag, "ISO") == 0 ||
            std::strcmp(Tag, "GPU") == 0 ||
            std::strcmp(Tag, "GPU3D") == 0 ||
            std::strcmp(Tag, "GTE") == 0))
    {
        std::fprintf(Files->sys, "[%hs] %hs\n", Tag, Msg);
        std::fflush(Files->sys);
    }
}

static const TCHAR* kPSXInputDir = TEXT("/Game/PSX/Input/");

static const char* ToPsx3dRefreshReason(EPsx3dRefreshReason V)
{
    switch (V)
    {
    case EPsx3dRefreshReason::Manual: return "manual";
    case EPsx3dRefreshReason::SceneChange: return "scene_change";
    case EPsx3dRefreshReason::HighFallback2D: return "high_fallback_2d";
    case EPsx3dRefreshReason::NewGteCode: return "new_gte_code";
    default: return "manual";
    }
}

static const char* ToPsx3dRefreshScope(EPsx3dRefreshScope V)
{
    switch (V)
    {
    case EPsx3dRefreshScope::Global: return "global";
    case EPsx3dRefreshScope::CurrentPcRegion: return "current_pc_region";
    case EPsx3dRefreshScope::CurrentOtWindow: return "current_ot_window";
    default: return "global";
    }
}

UPSXEmulatorComponent::UPSXEmulatorComponent()
{
    PrimaryComponentTick.bCanEverTick = true;

    // Load default PSX input assets created by scripts/ue5_create_psx_inputs.py
    struct FPadDefault { TObjectPtr<UInputAction>& Slot; const TCHAR* Name; };
    FPadDefault Defaults[] = {
        { IA_PadSelect,   TEXT("IA_PadSelect")   },
        { IA_PadL3,       TEXT("IA_PadL3")       },
        { IA_PadR3,       TEXT("IA_PadR3")       },
        { IA_PadStart,    TEXT("IA_PadStart")    },
        { IA_PadUp,       TEXT("IA_PadUp")       },
        { IA_PadRight,    TEXT("IA_PadRight")    },
        { IA_PadDown,     TEXT("IA_PadDown")     },
        { IA_PadLeft,     TEXT("IA_PadLeft")     },
        { IA_PadL2,       TEXT("IA_PadL2")       },
        { IA_PadR2,       TEXT("IA_PadR2")       },
        { IA_PadL1,       TEXT("IA_PadL1")       },
        { IA_PadR1,       TEXT("IA_PadR1")       },
        { IA_PadTriangle, TEXT("IA_PadTriangle") },
        { IA_PadCircle,   TEXT("IA_PadCircle")   },
        { IA_PadCross,    TEXT("IA_PadCross")    },
        { IA_PadSquare,   TEXT("IA_PadSquare")   },
    };

    for (auto& D : Defaults)
    {
        FString Path = FString(kPSXInputDir) + D.Name + TEXT(".") + D.Name;
        D.Slot = Cast<UInputAction>(StaticLoadObject(UInputAction::StaticClass(), nullptr, *Path));
    }

    // Load default InputMappingContext
    static const FString IMCPath = FString(kPSXInputDir) + TEXT("IMC_PSXPad.IMC_PSXPad");
    PadMappingContext = Cast<UInputMappingContext>(StaticLoadObject(UInputMappingContext::StaticClass(), nullptr, *IMCPath));
}

void UPSXEmulatorComponent::UpdateStepsExecuted(uint64 Steps, uint64 Cycles)
{
    StepsExecuted_.Store(Steps);
    TotalCyclesExecuted_.Store(Cycles);
}

void UPSXEmulatorComponent::StopWorkerThread()
{
    if (EmuWorker_)
    {
        bWorkerShouldStop_.Store(true);

        if (EmuThread_)
        {
            EmuThread_->WaitForCompletion();
            delete EmuThread_;
            EmuThread_ = nullptr;
        }

        delete EmuWorker_;
        EmuWorker_ = nullptr;

        emu::logf(emu::LogLevel::info, "CORE", "Worker thread stopped");
    }
}

// fopen_utf8 now provided by util/file_util.h

void UPSXEmulatorComponent::SetForceGteGeomOffsetZero(bool bEnabled)
{
    bForceGteGeomOffsetZero = bEnabled;
    if (Core_)
    {
        if (auto* Gte = Core_->gte())
            Gte->set_force_geom_offset_zero(bEnabled);
        if (auto* Gte3D = Core_->gte_3d())
            Gte3D->set_force_geom_offset_zero(bEnabled);
        emu::logf(emu::LogLevel::warn, "CORE",
            "Quirk live toggle: force_gte_geom_offset_zero = %d", bEnabled ? 1 : 0);
    }
}

bool UPSXEmulatorComponent::BootBiosInternal()
{
    if (!Core_)
        return false;

    if (BiosBytes_.Num() == 0)
    {
        emu::logf(emu::LogLevel::error, "CORE", "BootBiosInternal: BiosBytes empty!");
        return false;
    }

    emu::logf(emu::LogLevel::info, "CORE", "BootBiosInternal: BIOS size=%d bytes", BiosBytes_.Num());

    char err[256];
    err[0] = '\0';
    if (!Core_->set_bios_copy(BiosBytes_.GetData(), (uint32)BiosBytes_.Num(), err, sizeof(err)))
    {
        UE_LOG(LogPSXEmu, Error, TEXT("BIOS setup failed: %hs"), err[0] ? err : "unknown error");
        return false;
    }

    loader::LoadedImage Img{};
    Img.entry_pc = 0xBFC00000u;
    Img.has_gp = 0;
    Img.has_sp = 1;
    Img.sp = 0x801FFFF0u;

    emu::Core::InitOptions Opt{};
    Opt.pretty = bTraceASM ? 1 : 0;
    Opt.trace_io = bTraceIO ? 1 : 0;
    // HLE vectors: intercept BIOS exception handler and syscalls.
    // When OFF, the real BIOS exception handler runs (requires accurate HW emulation).
    // User can toggle via bHleVectors property in Blueprint.
    Opt.hle_vectors = bHleVectors ? 1 : 0;
    Opt.text_hle = bTextHle ? 1 : 0;
    Opt.loop_detectors = bLoopDetectors ? 1 : 0;
    Opt.bus_tick_batch = EffectiveBusTickBatch(bThreadedMode, BusTickBatch);
    Opt.cd_timing_mode = EffectiveCdTimingMode(CDTimingMode);
    if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
    {
        UE_LOG(LogPSXEmu, Error, TEXT("Core init (BIOS) failed: %hs"), err[0] ? err : "unknown error");
        emu::logf(emu::LogLevel::error, "CORE", "UE BIOS init failed: %s", err[0] ? err : "unknown error");
        return false;
    }

    // Apply cycle multiplier for timing accuracy
    Core_->set_cycle_multiplier(static_cast<uint32>(FMath::Clamp(CycleMultiplier, 1, 10)));

    UE_LOG(LogPSXEmu, Log, TEXT("BIOS boot initialized. PC=0x%08X CycleMult=%d Timing=WallClock CDTiming=%s"),
        Core_->pc(), CycleMultiplier, LexToString(CDTimingMode));
    emu::logf(emu::LogLevel::info, "CORE",
        "UE BIOS init OK pc=0x%08X hle_vectors=%d text_hle=%d bus_tick_batch=%u cycle_mult=%u threaded=%d timing=wallclock cd_timing=%s",
        (unsigned)Core_->pc(), Opt.hle_vectors, Opt.text_hle, (unsigned)Opt.bus_tick_batch, (unsigned)CycleMultiplier, bThreadedMode ? 1 : 0,
        (CDTimingMode == ECDTimingMode::Realistic) ? "realistic" : "compatibility-fast");
    StepsExecuted_.Store(0);
    TotalCyclesExecuted_.Store(0);
    LastAudioSamplesConsumed_.Store(0);
    return true;
}

void UPSXEmulatorComponent::BeginPlay()
{
    Super::BeginPlay();
}

void UPSXEmulatorComponent::InitEmulator()
{
    if (Core_)
    {
        UE_LOG(LogPSXEmu, Warning, TEXT("InitEmulator called but emulator already initialized."));
        return;
    }

    // Optional file logs.
    if (!OutputDir.IsEmpty())
    {
        const FString AbsOut = FPaths::ConvertRelativePathToFull(OutputDir);
        IFileManager::Get().MakeDirectory(*AbsOut, true);

        const FString CoreLogPath = FPaths::Combine(AbsOut, TEXT("r3000_core.log"));
        const FString CdLogPath = FPaths::Combine(AbsOut, TEXT("cdrom.log"));
        const FString GpuLogPath = FPaths::Combine(AbsOut, TEXT("gpu.log"));
        const FString SysLogPath = FPaths::Combine(AbsOut, TEXT("system.log"));
        const FString IoLogPath = FPaths::Combine(AbsOut, TEXT("io.log"));
        const FString SpuLogPath = FPaths::Combine(AbsOut, TEXT("spu.log"));

        CoreLogFile_ = fopen_utf8(FTCHARToUTF8(*CoreLogPath).Get(), "wb");
        CdLogFile_ = fopen_utf8(FTCHARToUTF8(*CdLogPath).Get(), "wb");
        GpuLogFile_ = fopen_utf8(FTCHARToUTF8(*GpuLogPath).Get(), "wb");
        SysLogFile_ = fopen_utf8(FTCHARToUTF8(*SysLogPath).Get(), "wb");
        IoLogFile_ = fopen_utf8(FTCHARToUTF8(*IoLogPath).Get(), "wb");
        SpuLogFile_ = fopen_utf8(FTCHARToUTF8(*SpuLogPath).Get(), "wb");

        const FString TextLogPath = FPaths::Combine(AbsOut, TEXT("outtext.log"));
        TextLogFile_ = fopen_utf8(FTCHARToUTF8(*TextLogPath).Get(), "wb");
    }

    // Init emu::logf → async log thread → UEAsyncLogOutput.
    // File pointers are valid until async_log_shutdown() in EndPlay.
    EmuLogFiles_.spu = SpuLogFile_;
    EmuLogFiles_.sys = SysLogFile_;
    emu::LogLevel EmuLevel = emu::LogLevel::info;
    {
        const FTCHARToUTF8 EmuLvlUtf8(*EmuLogLevel);
        EmuLevel = emu::log_parse_level(EmuLvlUtf8.Get());
        emu::async_log_init(EmuLevel, 14, UEAsyncLogOutput, &EmuLogFiles_);
    }

    if (CoreLogFile_)
        rlog::logger_init(&CoreLogger_, CoreLogFile_);
    else
        rlog::logger_init_cb(&CoreLogger_, UERlogCallback, nullptr);
    {
        const FTCHARToUTF8 LvlUtf8(*CoreLogLevel);
        rlog::Level Lvl = rlog::parse_level(LvlUtf8.Get());
        if (bTraceASM && (uint8_t)Lvl < (uint8_t)rlog::Level::trace)
            Lvl = rlog::Level::trace;
        rlog::logger_set_level(&CoreLogger_, Lvl);
    }
    {
        const FTCHARToUTF8 CatsUtf8(*CoreLogCats);
        rlog::logger_set_cats(&CoreLogger_, rlog::parse_categories_csv(CatsUtf8.Get()));
    }

    Core_ = new emu::Core(&CoreLogger_);
    if (Core_)
    {
        Core_->set_psx3d_analysis_enabled(bPsx3dAnalysisEnabled);
        Core_->set_psx3d_mode(bPsx3dAnalysisMode ? emu::Psx3dRunMode::analysis : emu::Psx3dRunMode::game);
    }
    // ─────────────────────────────────────────────────────────────────────
    // psx3dprof root directory resolution.
    //
    // Default location: <Project>/Saved/PSXProfiles/
    //   - Saved/ is the standard UE5 convention for runtime-mutable data
    //     (save games, configs, logs, profile snapshots that the user can
    //     edit live in devkit mode).
    //   - It is writable in BOTH editor (PIE) AND packaged builds, unlike
    //     Content/ which is read-only at runtime in shipped builds.
    //   - It is NOT versioned by default — that's a feature here, since
    //     profiles are per-machine devkit data, not project assets.
    //
    // Override: Psx3dProfilePath UPROPERTY. If non-empty, replaces the
    // default entirely. Used for advanced cases (shared network drive,
    // alternate test profiles). NO fallback search — the override is the
    // single source of truth, take it or leave it.
    //
    // Round-trip: load and save both go to the same root directory.
    // Live edits made via SetForceGteGeomOffsetZero() (or any future
    // devkit toggle) get persisted on Core shutdown via try_save_psx3d_profile.
    // ─────────────────────────────────────────────────────────────────────
    if (Core_)
    {
        FString ResolvedProfileRoot;
        if (!Psx3dProfilePath.IsEmpty())
        {
            ResolvedProfileRoot = Psx3dProfilePath;
        }
        else
        {
            ResolvedProfileRoot = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("PSXProfiles"));
            ResolvedProfileRoot = FPaths::ConvertRelativePathToFull(ResolvedProfileRoot);
        }
        FTCHARToUTF8 ProfileUtf8(*ResolvedProfileRoot);
        Core_->set_psx3d_profile_path_override(ProfileUtf8.Get());
        emu::logf(emu::LogLevel::warn, "CORE",
            "psx3dprof root resolved: %s (%s)",
            ProfileUtf8.Get(),
            Psx3dProfilePath.IsEmpty() ? "default Saved/PSXProfiles" : "UPROPERTY override");
    }
    if (Core_ && bPsx3dRefreshOnInit)
    {
        Core_->request_psx3d_analysis_refresh(
            ToPsx3dRefreshReason(Psx3dRefreshReason),
            ToPsx3dRefreshScope(Psx3dRefreshScope));
    }
    char err[256];
    err[0] = '\0';
    if (!Core_->alloc_ram(2u * 1024u * 1024u, err, sizeof(err)))
    {
        UE_LOG(LogPSXEmu, Error, TEXT("PSX core RAM allocation failed: %hs"), err[0] ? err : "unknown error");
        return;
    }

    UE_LOG(LogPSXEmu, Log, TEXT("PSX core created."));

    // Hook HW/system log sinks to files (optional).
    if (CdLogFile_ || GpuLogFile_ || SysLogFile_ || IoLogFile_)
    {
        const flog::Clock Clock = flog::clock_start();
        const flog::Level FileLevel = ToFileLogLevel(EmuLevel);
        const flog::Sink CdSink{CdLogFile_, FileLevel};
        const flog::Sink GpuSink{GpuLogFile_, FileLevel};
        const flog::Sink SysSink{SysLogFile_, FileLevel};
        const flog::Sink IoSink{IoLogFile_, FileLevel};
        Core_->set_log_sinks(CdSink, GpuSink, SysSink, IoSink, Clock);

        if (TextLogFile_)
        {
            Core_->set_text_out(TextLogFile_);
            const flog::Sink TextSink{TextLogFile_, FileLevel};
            Core_->set_text_io_sink(TextSink, Clock);
        }
    }

    // BIOS putchar → OnBiosPrint delegate.
    Core_->set_putchar_callback(&UPSXEmulatorComponent::PutcharCB, this);

    // Init core.
    // Priority: bDevKitMode (EXE direct boot with HLE) > bFastBoot (CD fast boot) > BIOS boot.
    if (bDevKitMode)
    {
        // DEV KIT MODE: init core (allocates RAM), then load EXE + HLE kernel.
        // Like a real DTL-H2000: BIOS kernel is initialized, then EXE is loaded on top.
        if (ExePath.IsEmpty())
        {
            UE_LOG(LogPSXEmu, Error, TEXT("bDevKitMode=true but ExePath is empty!"));
            emu::logf(emu::LogLevel::error, "CORE", "bDevKitMode=true but ExePath is empty");
            return;
        }

        // Init core with dummy entry (will be overridden by fast_boot_from_exe)
        loader::LoadedImage Img{};
        Img.entry_pc = 0x80010000u;
        Img.has_sp = 1;
        Img.sp = 0x801FFFF0u;

    emu::Core::InitOptions Opt{};
    Opt.pretty = bTraceASM ? 1 : 0;
    Opt.trace_io = bTraceIO ? 1 : 0;
    Opt.hle_vectors = 1; // Dev kit always uses HLE
    Opt.text_hle = bTextHle ? 1 : 0;
        Opt.loop_detectors = bLoopDetectors ? 1 : 0;
        Opt.bus_tick_batch = EffectiveBusTickBatch(bThreadedMode, BusTickBatch);
        Opt.cd_timing_mode = EffectiveCdTimingMode(CDTimingMode);
        if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
        {
            UE_LOG(LogPSXEmu, Error, TEXT("Core init (devkit) failed: %hs"), err[0] ? err : "unknown error");
            return;
        }

        // Load BIOS ROM data (if available) so PsyQ FntLoad can read the font.
        // We don't EXECUTE the BIOS — HLE handles syscalls — but the ROM data
        // must be mapped at 0xBFC00000 for library functions that read from it.
        if (!BiosPath.IsEmpty())
        {
            BiosBytes_.Reset();
            if (FFileHelper::LoadFileToArray(BiosBytes_, *BiosPath))
            {
                Core_->set_bios_copy(BiosBytes_.GetData(), (uint32)BiosBytes_.Num(), err, sizeof(err));
                UE_LOG(LogPSXEmu, Log, TEXT("DevKit: BIOS ROM loaded for font data (%d bytes)"), BiosBytes_.Num());
            }
        }

        // Parse and inject PSX EXE params (e.g. "1,30" for TREX attract mode)
        if (!PsxParam.IsEmpty())
        {
            TArray<FString> Tokens;
            PsxParam.ParseIntoArray(Tokens, TEXT(","), true);
            TArray<int32> Ints;
            for (const FString& T : Tokens)
            {
                Ints.Add(FCString::Atoi(*T.TrimStartAndEnd()));
            }
            if (Ints.Num() > 0)
            {
                Core_->set_psx_params(Ints.GetData(), (uint32)Ints.Num());
                emu::logf(emu::LogLevel::info, "CORE", "PsxParam parsed: %d ints from \"%s\"",
                    Ints.Num(), TCHAR_TO_UTF8(*PsxParam));
            }
        }

        // Load EXE + initialize HLE kernel (PCB/TCB, I_MASK, COP0)
        FTCHARToUTF8 ExeUtf8(*ExePath);
        if (!Core_->fast_boot_from_exe(ExeUtf8.Get(), err, sizeof(err)))
        {
            UE_LOG(LogPSXEmu, Error, TEXT("Dev kit EXE boot failed: %hs"), err[0] ? err : "unknown error");
            return;
        }

        Core_->set_cycle_multiplier(static_cast<uint32>(FMath::Clamp(CycleMultiplier, 1, 10)));
        UE_LOG(LogPSXEmu, Log, TEXT("Dev kit boot OK: %s → PC=0x%08X CDTiming=%s"), *ExePath, Core_->pc(), LexToString(CDTimingMode));
    }
    else if (bFastBoot)
    {
        // Fast boot: skip BIOS, load game EXE from CD's SYSTEM.CNF
        loader::LoadedImage Img{};
        Img.entry_pc = 0x80000000u;
        Img.has_gp = 0;
        Img.has_sp = 1;
        Img.sp = 0x801FFFF0u;

    emu::Core::InitOptions Opt{};
    Opt.pretty = bTraceASM ? 1 : 0;
    Opt.trace_io = bTraceIO ? 1 : 0;
    Opt.hle_vectors = 0; // fastboot will enable HLE vectors internally after loading EXE
    Opt.text_hle = bTextHle ? 1 : 0;
        Opt.loop_detectors = bLoopDetectors ? 1 : 0;
        Opt.bus_tick_batch = EffectiveBusTickBatch(bThreadedMode, BusTickBatch);
        Opt.cd_timing_mode = EffectiveCdTimingMode(CDTimingMode);
        if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
        {
            UE_LOG(LogPSXEmu, Error, TEXT("Core init (fastboot) failed: %hs"), err[0] ? err : "unknown error");
            emu::logf(emu::LogLevel::error, "CORE", "UE fastboot init_from_image failed: %s", err[0] ? err : "unknown error");
            return;
        }
        Core_->set_cycle_multiplier(static_cast<uint32>(FMath::Clamp(CycleMultiplier, 1, 10)));
        emu::logf(emu::LogLevel::info, "CORE", "UE fastboot init: BIOS skipped pc=0x%08X cycle_mult=%u cd_timing=%s",
            (unsigned)Core_->pc(), (unsigned)CycleMultiplier,
            (CDTimingMode == ECDTimingMode::Realistic) ? "realistic" : "compatibility-fast");
    }
    else
    {
        // Optional BIOS boot.
        if (!BiosPath.IsEmpty())
        {
            BiosBytes_.Reset();
            if (!FFileHelper::LoadFileToArray(BiosBytes_, *BiosPath))
            {
                UE_LOG(LogPSXEmu, Error, TEXT("Failed to load BIOS: %s"), *BiosPath);
                emu::logf(emu::LogLevel::error, "CORE", "UE BIOS load failed: %s", FTCHARToUTF8(*BiosPath).Get());
                return;
            }
            if (!BootBiosInternal())
            {
                // Keep going so fastboot can still report a clear error in logs.
            }
        }
        else
        {
            emu::logf(emu::LogLevel::info, "CORE", "UE BiosPath is empty (BIOS init will be skipped)");
        }
    }

    // Optional disc insert (skip in dev kit mode — no CD needed).
    if (!bDevKitMode && !DiscPath.IsEmpty())
    {
            emu::logf(
            emu::LogLevel::info,
            "CORE",
            "UE insert_disc begin path='%s' fastboot=%d hle_vectors=%d",
            FTCHARToUTF8(*DiscPath).Get(),
            bFastBoot ? 1 : 0,
            bHleVectors ? 1 : 0);
        FTCHARToUTF8 DiscUtf8(*DiscPath);
        if (!Core_->insert_disc(DiscUtf8.Get(), err, sizeof(err)))
        {
            UE_LOG(LogPSXEmu, Error, TEXT("CD insert failed: %hs"), err[0] ? err : "unknown error");
            emu::logf(
                emu::LogLevel::error,
                "CORE",
                "UE insert_disc FAILED path='%s' err='%s'",
                DiscUtf8.Get(),
                err[0] ? err : "unknown error");
        }
        else
        {
            UE_LOG(LogPSXEmu, Log, TEXT("CD inserted."));
            emu::logf(
                emu::LogLevel::info,
                "CORE",
                "UE insert_disc OK path='%s'",
                DiscUtf8.Get());
        }
    }
    else if (!bDevKitMode)
    {
        emu::logf(emu::LogLevel::info, "CORE", "UE DiscPath is empty (no disc inserted)");
    }

    // Fast boot: skip BIOS, load game EXE directly from CD (skip in dev kit mode).
    emu::logf(emu::LogLevel::info, "CORE", "UE fastboot request=%d (bFastBoot) hle_vectors(bios)=%d devkit=%d",
        bFastBoot ? 1 : 0, bHleVectors ? 1 : 0, bDevKitMode ? 1 : 0);
    if (!bDevKitMode && bFastBoot && Core_)
    {
        char fberr[256];
        fberr[0] = '\0';
        if (Core_->fast_boot_from_cd(fberr, sizeof(fberr)))
        {
            UE_LOG(LogPSXEmu, Log, TEXT("Fast boot OK. PC=0x%08X"), Core_->pc());
            emu::logf(emu::LogLevel::info, "CORE", "UE fastboot OK pc=0x%08X", (unsigned)Core_->pc());
        }
        else
        {
            UE_LOG(LogPSXEmu, Error, TEXT("Fast boot failed: %hs"), fberr[0] ? fberr : "unknown");
            emu::logf(emu::LogLevel::error, "CORE", "UE fastboot FAILED: %s", fberr[0] ? fberr : "unknown");
        }
    }

    // --- Hook: physics watch (Ridge Racer car struct at 0x80080194) ---
    // Watch all physics variables each VBlank to find the 800 mph root cause.
    // Car struct offsets: +0x82=gear, +0x84=rpm, +0xA0=speed, +0xA4=speed_delta,
    //                     +0xB0=torque, +0xB4=drive_mode
    if (Core_ && Core_->bus())
    {
        static struct {
            r3000::Bus* bus;
            uint32_t    car_base; // physical addr of car struct (0x00080194)
            int32_t     last_speed;
            int32_t     last_rpm;
            int32_t     last_torque;
            int32_t     last_delta;
        } s_phys;
        s_phys = { Core_->bus(), 0x00080194u, -1, -1, -1, -1 };

        Core_->hooks().add_vblank([](uint32_t vblank, void* user) {
            auto* ctx = decltype(&s_phys)(user);
            const uint8_t* ram = ctx->bus->ram_ptr();
            auto rd32 = [&](uint32_t off) -> int32_t {
                uint32_t a = ctx->car_base + off;
                return (int32_t)((uint32_t)ram[a]
                    | ((uint32_t)ram[a+1] << 8)
                    | ((uint32_t)ram[a+2] << 16)
                    | ((uint32_t)ram[a+3] << 24));
            };
            auto rd16 = [&](uint32_t off) -> int16_t {
                uint32_t a = ctx->car_base + off;
                return (int16_t)((uint16_t)ram[a] | ((uint16_t)ram[a+1] << 8));
            };

            int32_t speed      = rd32(0xA0);
            int32_t speed_delta= rd32(0xA4);
            int32_t rpm        = rd32(0x84);
            int32_t torque     = rd32(0xB0);
            int16_t gear       = rd16(0x82);
            int32_t drive_mode = rd32(0xB4);

            // Log when speed or key values change
            if (speed != ctx->last_speed || rpm != ctx->last_rpm
                || torque != ctx->last_torque || speed_delta != ctx->last_delta)
            {
                emu::logf(emu::LogLevel::debug, "HOOK",
                    "VB#%u spd=%d(/%d) delta=%d rpm=%d torq=%d gear=%d drv=%d",
                    vblank, speed, speed/8, speed_delta, rpm, torque, (int)gear, drive_mode);
                ctx->last_speed  = speed;
                ctx->last_rpm    = rpm;
                ctx->last_torque = torque;
                ctx->last_delta  = speed_delta;
            }
        }, &s_phys);

        emu::logf(emu::LogLevel::debug, "HOOK", "Physics watch on car 0x%08X (spd/rpm/torq/delta/gear/drv)", 0x80080194u);
    }

    // Optional run N steps immediately.
    StepsExecuted_.Store(0);
    TotalCyclesExecuted_.Store(0);
    LastAudioSamplesConsumed_.Store(0);
    NextPcSampleAt_ = (PcSampleIntervalSteps > 0) ? static_cast<uint64>(PcSampleIntervalSteps) : 0;
    NextAudioStatsTime_ = FPlatformTime::Seconds() + FMath::Max((double)AudioStatsIntervalSec, 0.1);
    const int32 N = StepsToRunOnBeginPlay;
    uint64 InitSteps = 0;
    for (int32 i = 0; i < N; ++i)
    {
        const auto Res = Core_->step();
        if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
        {
            UE_LOG(LogPSXEmu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++InitSteps;
    }
    StepsExecuted_.Store(InitSteps);

    // Find audio component on same actor and connect SPU callback.
    AActor* Owner = GetOwner();
    AudioComp_ = Owner ? Owner->FindComponentByClass<UPSXAudioComponent>() : nullptr;
    if (AudioComp_ && Core_)
    {
        // Ensure no stale audio from a previous run can replay (e.g. BIOS jingle when toggling fastboot).
        AudioComp_->ResetBuffer(false);

        r3000::Bus* Bus = Core_->bus();
        audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
        if (Spu)
        {
            UPSXAudioComponent* Audio = AudioComp_;
            Spu->set_audio_callback([Audio](const int16_t* Samples, int Count) {
                if (Audio)
                    Audio->PushSamples(Samples, Count * 2); // Count = stereo frames, *2 for individual int16 (L,R)
            });
            AudioComp_->Start();
            UE_LOG(LogPSXEmu, Log, TEXT("SPU audio connected to UPSXAudioComponent."));
            emu::logf(emu::LogLevel::info, "CORE", "UE audio connected: gain=%.3f muted=%d",
                (double)AudioComp_->OutputGain, AudioComp_->IsMuted() ? 1 : 0);
        }
        else
        {
            UE_LOG(LogPSXEmu, Warning, TEXT("SPU not available — audio callback not connected."));
            emu::logf(emu::LogLevel::info, "CORE", "UE audio not connected (SPU missing)");
        }
    }
    else
    {
        emu::logf(emu::LogLevel::info, "CORE", "UE audio not connected (AudioComp=%d Core=%d)",
            AudioComp_ ? 1 : 0, Core_ ? 1 : 0);
    }

    // Find VramViewer component — owns the VRAM texture, shared with 2D + 3D components.
    UPSXVramViewerComponent* VramComp = Owner ? Owner->FindComponentByClass<UPSXVramViewerComponent>() : nullptr;
    r3000::Bus* Bus = Core_ ? Core_->bus() : nullptr;
    gpu::Gpu* Gpu = Bus ? Bus->gpu() : nullptr;

    if (VramComp && Gpu)
    {
        VramComp->BindGpu(Gpu);
        UE_LOG(LogPSXEmu, Log, TEXT("VramViewer connected: texture=%p"), (void*)VramComp->GetVramTexture());
        emu::logf(emu::LogLevel::info, "CORE", "VramViewer connected: texture=%p", (void*)VramComp->GetVramTexture());
    }

    // Find 2D GPU component and connect.
    GpuComp_ = Owner ? Owner->FindComponentByClass<UPSX2DRenderComponent>() : nullptr;
    if (GpuComp_ && Gpu)
    {
        GpuComp_->BindGpu(Gpu);
        // Pass shared VRAM texture from VramViewer
        if (VramComp)
            GpuComp_->SetVramTexture(VramComp->GetVramTexture());
        UE_LOG(LogPSXEmu, Log, TEXT("GPU 2D connected. VramTex=%p"), (void*)GpuComp_->GetVramTexture());
        emu::logf(emu::LogLevel::info, "CORE", "UE GPU 2D connected: scale=%.2f zstep=%.4f",
            (double)GpuComp_->PixelScale, (double)GpuComp_->ZStep);
    }

    // Apply per-game render quirks via UPROPERTY override.
    // See gte::Gte::set_force_geom_offset_zero() in src/gte/gte.h for the
    // full policy comment. The psx3dprof profile is the canonical place to
    // enable this; the UPROPERTY is a manual override useful for testing.
    if (bForceGteGeomOffsetZero)
    {
        if (Core_)
        {
            if (auto* Gte = Core_->gte())
                Gte->set_force_geom_offset_zero(true);
            if (auto* Gte3D = Core_->gte_3d())
                Gte3D->set_force_geom_offset_zero(true);
        }
        emu::logf(emu::LogLevel::warn, "CORE",
            "Quirk enabled at init: force_gte_geom_offset_zero (GTE OFX/OFY → 0 in RTPS)");
    }
    else
    {
        emu::logf(emu::LogLevel::info, "CORE", "UE GPU 2D not connected (GpuComp=%d Gpu=%d)",
            GpuComp_ ? 1 : 0, Gpu ? 1 : 0);
    }

    // Find 3D GPU component and connect (shadow GPU + shared VRAM texture).
    Gpu3DComp_ = Owner ? Owner->FindComponentByClass<UPSX3DRenderComponent>() : nullptr;
    if (Gpu3DComp_ && Core_)
    {
        if (Gpu)
            Gpu3DComp_->BindGpu(Gpu);

        // Bind shadow GPU (3D tag-based reconstruction)
        gpu::Gpu3D* GpuShadow = Core_->gpu_3d();
        if (GpuShadow)
            Gpu3DComp_->BindGpu3D(GpuShadow);

        // Pass shared VRAM texture from VramViewer
        if (VramComp)
            Gpu3DComp_->SetVramTexture(VramComp->GetVramTexture());

        UE_LOG(LogPSXEmu, Log, TEXT("GPU 3D connected. Shadow=%p VramTex=%p"),
            (void*)GpuShadow, Gpu3DComp_->GetMeshComponent() ? (void*)VramComp : nullptr);
        emu::logf(emu::LogLevel::info, "CORE", "GPU3D connected (shadow=%p)", (void*)GpuShadow);
    }

    // Video component — auto-detects 24-bit (MDEC FMV) mode and shows a video plane.
    VideoComp_ = Owner ? Owner->FindComponentByClass<UPSXVideoSurfaceComponent>() : nullptr;
    if (VideoComp_ && Gpu)
    {
        emu::logf(emu::LogLevel::warn, "CORE", "Binding VideoComponent gpu=%p comp=%p", (void*)Gpu, (void*)VideoComp_);
        VideoComp_->BindGpu(Gpu);
        UE_LOG(LogPSXEmu, Log, TEXT("Video component connected"));
        emu::logf(emu::LogLevel::warn, "CORE", "VideoComponent connected");
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CORE", "VideoComponent not connected video=%d gpu=%d",
            VideoComp_ ? 1 : 0, Gpu ? 1 : 0);
    }

    // Image component — shows static CPU→VRAM images (logos, loading screens).
    ImageComp_ = Owner ? Owner->FindComponentByClass<UPSXImageSurfaceComponent>() : nullptr;
    if (ImageComp_ && Gpu)
    {
        emu::logf(emu::LogLevel::warn, "CORE", "Binding ImageComponent gpu=%p comp=%p", (void*)Gpu, (void*)ImageComp_);
        ImageComp_->BindGpu(Gpu);
        UE_LOG(LogPSXEmu, Log, TEXT("Image component connected"));
        emu::logf(emu::LogLevel::warn, "CORE", "ImageComponent connected");
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CORE", "ImageComponent not connected image=%d gpu=%d",
            ImageComp_ ? 1 : 0, Gpu ? 1 : 0);
    }

    SurfaceComp_ = Owner ? Owner->FindComponentByClass<UPSXSurfaceComponent>() : nullptr;
    if (SurfaceComp_ && Gpu)
    {
        emu::logf(emu::LogLevel::warn, "CORE", "Binding SurfaceComponent gpu=%p comp=%p", (void*)Gpu, (void*)SurfaceComp_);
        SurfaceComp_->BindGpu(Gpu);
        if (VramComp)
            SurfaceComp_->SetSurfaceTexture(VramComp->GetVramTexture());
        UE_LOG(LogPSXEmu, Log, TEXT("Surface component connected"));
        emu::logf(emu::LogLevel::warn, "CORE", "SurfaceComponent connected");
    }
    else
    {
        UE_LOG(LogPSXEmu, Log, TEXT("Surface component not connected (Surface=%d Gpu=%d)"),
            SurfaceComp_ ? 1 : 0, Gpu ? 1 : 0);
        emu::logf(emu::LogLevel::warn, "CORE", "SurfaceComponent not connected surface=%d gpu=%d",
            SurfaceComp_ ? 1 : 0, Gpu ? 1 : 0);
    }

    // Start worker thread if threaded mode is enabled.
    if (bThreadedMode)
    {
        bWorkerShouldStop_.Store(false);
        bWorkerPaused_.Store(false);

        EmuWorker_ = new FPSXEmuWorker(this);
        EmuThread_ = FRunnableThread::Create(
            EmuWorker_,
            TEXT("PSXEmuWorker"),
            0, // Default stack size
            TPri_AboveNormal, // Higher priority for accurate timing
            FPlatformAffinity::GetNoAffinityMask());

        if (EmuThread_)
        {
            UE_LOG(LogPSXEmu, Log, TEXT("Threaded emulation mode: worker thread started."));
            emu::logf(emu::LogLevel::info, "CORE", "UE threaded mode: worker thread started");
        }
        else
        {
            UE_LOG(LogPSXEmu, Error, TEXT("Failed to create emulation worker thread!"));
            delete EmuWorker_;
            EmuWorker_ = nullptr;
        }
    }

    UE_LOG(LogPSXEmu, Log, TEXT("InitEmulator done. PC=0x%08X steps=%llu threaded=%d"),
        Core_->pc(), StepsExecuted_.Load(), bThreadedMode ? 1 : 0);
}

void UPSXEmulatorComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    // Process pending putchar lines from worker thread (must broadcast on game thread)
    {
        TArray<UPSXEmulatorComponent::FPendingBiosLine> LinesToBroadcast;
        {
            FScopeLock Lock(&PutcharLock_);
            LinesToBroadcast = MoveTemp(PutcharPendingLines_);
            PutcharPendingLines_.Reset();
        }
        for (const UPSXEmulatorComponent::FPendingBiosLine& Line : LinesToBroadcast)
        {
            OnBiosPrint.Broadcast(Line.Line, Line.bFromPrintf);
        }
    }

    if (!bRunning || !Core_)
    {
        CyclesLastFrame_.Store(0);
        return;
    }

    // Poll UE5 input and forward to PS1 controller (works in both threaded and non-threaded modes)
    PollPadInput();

    // THREADED MODE: Worker thread handles emulation, we just monitor stats
    if (bThreadedMode && EmuWorker_)
    {
        // Periodic audio stats logging (in threaded mode too)
        if (bLogAudioStats && AudioComp_ && AudioStatsIntervalSec > 0.0f)
        {
            const double Now = FPlatformTime::Seconds();
            if (Now >= NextAudioStatsTime_)
            {
                const uint64 pushed = AudioComp_->GetTotalPushedSamples();
                const uint64 gen = AudioComp_->GetTotalGeneratedSamples();
                const uint64 drop = AudioComp_->GetTotalDroppedSamples();
                const uint64 sil = AudioComp_->GetTotalSilenceSamples();
                const uint32 buf = AudioComp_->GetBufferedSamples();
                const uint64 steps = StepsExecuted_.Load();
                const uint64 cycles = TotalCyclesExecuted_.Load();

                emu::logf(emu::LogLevel::info, "CORE",
                    "UE threaded stats: steps=%llu cycles=%llu pushed_i16=%llu gen_f32=%llu drop_i16=%llu silence_f32=%llu buf_i16=%u",
                    (unsigned long long)steps,
                    (unsigned long long)cycles,
                    (unsigned long long)pushed,
                    (unsigned long long)gen,
                    (unsigned long long)drop,
                    (unsigned long long)sil,
                    (unsigned)buf);

                NextAudioStatsTime_ = Now + FMath::Max((double)AudioStatsIntervalSec, 0.1);
            }
        }

        {
            static double NextSurfaceStatsTime = 0.0;
            const double Now = FPlatformTime::Seconds();
            if (Now >= NextSurfaceStatsTime)
            {
                emu::logf(
                    emu::LogLevel::warn,
                    "CORE",
                    "SurfaceState video_comp=%d video_active=%d video_tex=%p video_wh=%dx%d image_comp=%d image_active=%d image_tex=%p surface_comp=%d surface_visible=%d surface_tex=%p",
                    VideoComp_ ? 1 : 0,
                    (VideoComp_ && VideoComp_->IsVideoActive()) ? 1 : 0,
                    VideoComp_ ? (void*)VideoComp_->GetVideoTexture() : nullptr,
                    VideoComp_ ? VideoComp_->GetVideoWidth() : 0,
                    VideoComp_ ? VideoComp_->GetVideoHeight() : 0,
                    ImageComp_ ? 1 : 0,
                    (ImageComp_ && ImageComp_->IsImageActive()) ? 1 : 0,
                    ImageComp_ ? (void*)ImageComp_->GetImageTexture() : nullptr,
                    SurfaceComp_ ? 1 : 0,
                    (SurfaceComp_ && SurfaceComp_->IsSurfaceVisible()) ? 1 : 0,
                    SurfaceComp_ ? (void*)SurfaceComp_->GetSurfaceTexture() : nullptr);

                NextSurfaceStatsTime = Now + 1.0;
            }
        }
        return; // Worker thread handles all emulation
    }

    // Non-threaded fallback.
    // Constants for audio-driven timing
    // PS1 CPU: 33.8688 MHz, Audio: 44100 Hz → 768 CPU cycles per audio sample
    constexpr uint32 kCyclesPerSampleLocal = 768;
    constexpr uint32 kSampleRateLocal = 44100;

    uint64 TargetCycles = 0;

    if (bAudioDrivenTiming && AudioComp_)
    {
        // AUDIO-DRIVEN MODE: Use audio consumption as the master clock.
        // The audio thread consumes samples at exactly 44100 Hz, providing
        // a very stable timing reference.

        // Get current audio consumption (samples consumed by audio thread)
        // TotalGeneratedSamples is stereo samples (L+R pairs = 2 int16 per frame)
        const uint64 AudioSamplesConsumed = AudioComp_->GetTotalGeneratedSamples() / 2;

        // Calculate how many CPU cycles correspond to the consumed audio
        const uint64 AudioDrivenCycles = AudioSamplesConsumed * kCyclesPerSampleLocal;

        // Target buffer: keep ahead by AudioBufferTargetMs worth of samples
        // This prevents audio underruns while keeping latency reasonable
        const uint32 BufferSamples = static_cast<uint32>(
            (AudioBufferTargetMs / 1000.0f) * kSampleRateLocal);
        const uint64 BufferCycles = static_cast<uint64>(BufferSamples) * kCyclesPerSampleLocal;

        // Target = audio consumed + buffer ahead
        const uint64 RequiredCycles = AudioDrivenCycles + BufferCycles;

        // Only execute if we're behind the target
        const uint64 TotalCyclesNow = TotalCyclesExecuted_.Load();
        if (TotalCyclesNow < RequiredCycles)
        {
            TargetCycles = RequiredCycles - TotalCyclesNow;
            // Cap to prevent runaway execution if audio was paused
            const uint64 MaxCatchup = static_cast<uint64>(0.1 * kPS1CpuClock); // 100ms max
            if (TargetCycles > MaxCatchup)
            {
                emu::logf(emu::LogLevel::warn, "CORE",
                    "Audio-driven: large catchup clamped from %llu to %llu cycles",
                    (unsigned long long)TargetCycles, (unsigned long long)MaxCatchup);
                TargetCycles = MaxCatchup;
            }
        }
        else
        {
            // We're ahead of audio consumption - nothing to do this frame
            TargetCycles = 0;
        }

        LastAudioSamplesConsumed_.Store(AudioSamplesConsumed);
    }
    else
    {
        // DELTATIME MODE: Original behavior based on UE5 frame time
        TargetCycles = static_cast<uint64>(
            FMath::Clamp(DeltaTime, 0.0f, 0.05f) * kPS1CpuClock * EmulationSpeed);
    }

    const double BudgetSeconds = FMath::Max(BudgetMs, 1.0f) * 0.001;
    const double StartTimeNonThreaded = FPlatformTime::Seconds();

    // Run instructions, counting REAL cycles per instruction.
    uint64 CyclesRan = 0;
    uint64 LocalSteps = StepsExecuted_.Load();
    uint64 LocalTotalCycles = TotalCyclesExecuted_.Load();

    while (CyclesRan < TargetCycles)
    {
        const auto Res = Core_->step();
        if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
        {
            UE_LOG(LogPSXEmu, Warning, TEXT("Emu stopped: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            bRunning = false;
            CyclesLastFrame_.Store(static_cast<int32>(CyclesRan));
            return;
        }

        const uint64 InstrCycles = Core_->last_cycles();
        CyclesRan += InstrCycles;
        LocalTotalCycles += InstrCycles;
        ++LocalSteps;

        // Check wall-clock budget every 4096 instructions
        if ((LocalSteps & 0xFFF) == 0)
        {
            if (FPlatformTime::Seconds() - StartTimeNonThreaded >= BudgetSeconds)
                break;
        }

        if (NextPcSampleAt_ != 0 && LocalSteps >= NextPcSampleAt_)
        {
            // Goes to UE Output Log and system.log (tag CORE is routed to sys file).
            r3000::Cpu* Cpu = Core_ ? Core_->cpu() : nullptr;
            r3000::Bus* Bus = Core_ ? Core_->bus() : nullptr;
            const uint32 Cop0Cause = Cpu ? Cpu->cop0(13) : 0u;
            const uint32 Cop0Epc = Cpu ? Cpu->cop0(14) : 0u;
            const uint32 ExcCode = (Cop0Cause >> 2) & 0x1Fu;
            const uint32 IStat = Bus ? Bus->irq_stat_raw() : 0u;
            const uint32 IMask = Bus ? Bus->irq_mask_raw() : 0u;

            emu::logf(emu::LogLevel::info, "CORE",
                "UE PC sample steps=%llu pc=0x%08X cycles_ran=%llu target=%llu total=%llu exc=%u epc=0x%08X i_stat=0x%08X i_mask=0x%08X",
                (unsigned long long)LocalSteps,
                (unsigned)Core_->pc(),
                (unsigned long long)CyclesRan,
                (unsigned long long)TargetCycles,
                (unsigned long long)LocalTotalCycles,
                (unsigned)ExcCode,
                (unsigned)Cop0Epc,
                (unsigned)IStat,
                (unsigned)IMask);

            const uint64 StepInterval = static_cast<uint64>(FMath::Max(PcSampleIntervalSteps, 1));
            while (NextPcSampleAt_ != 0 && NextPcSampleAt_ <= LocalSteps)
                NextPcSampleAt_ += StepInterval;
        }

    }

    // Update atomic counters
    StepsExecuted_.Store(LocalSteps);
    TotalCyclesExecuted_.Store(LocalTotalCycles);
    CyclesLastFrame_.Store(static_cast<int32>(CyclesRan));

    // Flush any remaining SPU samples to the audio ring buffer.
    // At high framerates (e.g. 1000fps), the SPU internal buffer may not fill
    // to its flush threshold within a single frame, causing audio dropouts.
    r3000::Bus* Bus = Core_ ? Core_->bus() : nullptr;
    audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
    if (Spu)
        Spu->flush_audio();

    // Periodic audio stats to file log (system.log) for diagnosing silence/underruns.
    if (bLogAudioStats && AudioComp_ && AudioStatsIntervalSec > 0.0f)
    {
        const double Now = FPlatformTime::Seconds();
        if (Now >= NextAudioStatsTime_)
        {
            const uint64 pushed = AudioComp_->GetTotalPushedSamples();
            const uint64 gen = AudioComp_->GetTotalGeneratedSamples();
            const uint64 drop = AudioComp_->GetTotalDroppedSamples();
            const uint64 sil = AudioComp_->GetTotalSilenceSamples();
            const uint32 buf = AudioComp_->GetBufferedSamples();
            emu::logf(emu::LogLevel::info, "CORE",
                "UE audio stats: pushed_i16=%llu gen_f32=%llu drop_i16=%llu silence_f32=%llu buf_i16=%u gain=%.3f muted=%d",
                (unsigned long long)pushed,
                (unsigned long long)gen,
                (unsigned long long)drop,
                (unsigned long long)sil,
                (unsigned)buf,
                (double)AudioComp_->OutputGain,
                AudioComp_->IsMuted() ? 1 : 0);

            NextAudioStatsTime_ = Now + FMath::Max((double)AudioStatsIntervalSec, 0.1);
        }
    }
}

bool UPSXEmulatorComponent::ResetBiosBoot()
{
    return BootBiosInternal();
}

int32 UPSXEmulatorComponent::StepInstructions(int32 Steps)
{
    if (!Core_ || Steps <= 0)
        return 0;

    // Manual stepping should pause worker thread if running
    if (bThreadedMode && EmuWorker_)
    {
        bWorkerPaused_.Store(true);
    }

    int32 Executed = 0;
    uint64 CurrentSteps = StepsExecuted_.Load();
    for (int32 i = 0; i < Steps; ++i)
    {
        const auto Res = Core_->step();
        if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
        {
            UE_LOG(LogPSXEmu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++Executed;
        ++CurrentSteps;
    }
    StepsExecuted_.Store(CurrentSteps);
    return Executed;
}

int32 UPSXEmulatorComponent::GetProgramCounter() const
{
    return Core_ ? (int32)Core_->pc() : 0;
}

FString UPSXEmulatorComponent::GetProgramCounterString() const
{
    return Core_ ? FString::Printf(TEXT("%08x"), (int32)Core_->pc()) : TEXT("error");
}


void UPSXEmulatorComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    // Stop worker thread FIRST (before touching Core_)
    StopWorkerThread();

    if (Core_)
    {
        // Disconnect SPU callback before destroying to avoid dangling pointer.
        r3000::Bus* Bus = Core_->bus();
        audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
        if (Spu)
            Spu->set_audio_callback(nullptr);
        delete Core_;
        Core_ = nullptr;
        UE_LOG(LogPSXEmu, Log, TEXT("PSX core destroyed."));
    }
    if (AudioComp_)
    {
        AudioComp_->Stop();
        AudioComp_->ResetBuffer(false);
    }
    AudioComp_ = nullptr;
    GpuComp_ = nullptr;
    emu::async_log_shutdown();  // blocks until consumer thread drains + exits

    if (CoreLogFile_)
    {
        std::fclose(CoreLogFile_);
        CoreLogFile_ = nullptr;
    }
    if (CdLogFile_)
    {
        std::fclose(CdLogFile_);
        CdLogFile_ = nullptr;
    }
    if (GpuLogFile_)
    {
        std::fclose(GpuLogFile_);
        GpuLogFile_ = nullptr;
    }
    if (SysLogFile_)
    {
        std::fclose(SysLogFile_);
        SysLogFile_ = nullptr;
    }
    if (IoLogFile_)
    {
        std::fclose(IoLogFile_);
        IoLogFile_ = nullptr;
    }
    if (SpuLogFile_)
    {
        std::fclose(SpuLogFile_);
        SpuLogFile_ = nullptr;
    }
    if (TextLogFile_)
    {
        std::fclose(TextLogFile_);
        TextLogFile_ = nullptr;
    }

    Super::EndPlay(EndPlayReason);
}

void UPSXEmulatorComponent::PutcharCB(char Ch, bool bFromPrintf, void* User)
{
    auto* Self = static_cast<UPSXEmulatorComponent*>(User);
    if (!Self)
        return;

    // NOTE: This callback runs on the WORKER THREAD during Core::step().
    // UE5 delegates are NOT thread-safe, so we queue completed lines here
    // and broadcast them from TickComponent (game thread).
    if (!Self->PutcharLineBuf_.IsEmpty() && Self->PutcharLineFromPrintf_ != bFromPrintf)
    {
        FScopeLock Lock(&Self->PutcharLock_);
        Self->PutcharPendingLines_.Add({Self->PutcharLineBuf_, Self->PutcharLineFromPrintf_});
        Self->PutcharLineBuf_.Reset();
    }

    if (Ch == '\n' || Ch == '\r')
    {
        if (!Self->PutcharLineBuf_.IsEmpty())
        {
            FScopeLock Lock(&Self->PutcharLock_);
            Self->PutcharPendingLines_.Add({Self->PutcharLineBuf_, Self->PutcharLineFromPrintf_});
            Self->PutcharLineBuf_.Reset();
        }
    }
    else
    {
        if (Self->PutcharLineBuf_.IsEmpty())
            Self->PutcharLineFromPrintf_ = bFromPrintf;
        Self->PutcharLineBuf_.AppendChar(static_cast<TCHAR>(Ch));
    }
}

// ---------------------------------------------------------------------------
// Setup Enhanced Input mapping context for PS1 controller
// ---------------------------------------------------------------------------
void UPSXEmulatorComponent::SetupPadInput()
{
    if (bPadMappingAdded_ || !PadMappingContext) return;

    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!PC || !PC->GetLocalPlayer()) return;

    UEnhancedInputLocalPlayerSubsystem* EIS =
        ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PC->GetLocalPlayer());
    if (EIS)
    {
        EIS->AddMappingContext(PadMappingContext, 0);
        UE_LOG(LogPSXEmu, Log, TEXT("PS1 pad mapping context added via Enhanced Input"));
    }
    else
    {
        UE_LOG(LogPSXEmu, Warning, TEXT("Enhanced Input subsystem not available, using direct IsInputKeyDown"));
    }

    bPadMappingAdded_ = true;
}

// ---------------------------------------------------------------------------
// Poll gamepad input and forward to PS1 controller (SIO0)
// Requires a GameMode with a DefaultPawn so PlayerController receives input.
// Run scripts/ue5_create_psx_gamemode.py then set GameMode Override in World Settings.
// ---------------------------------------------------------------------------
void UPSXEmulatorComponent::PollPadInput()
{
    if (!Core_) { return; }

    if (!bPadMappingAdded_)
        SetupPadInput();

    APlayerController* PC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
    if (!PC)
    {
        static bool bWarnedNoPC = false;
        if (!bWarnedNoPC)
        {
            UE_LOG(LogPSXEmu, Warning, TEXT("PadInput: No PlayerController found"));
            bWarnedNoPC = true;
        }
        return;
    }

    // Disable default pawn input so gamepad buttons don't move the UE5 camera.
    // We consume all gamepad input for the PS1 emulator.
    if (!bPawnInputDisabled_)
    {
        if (APawn* Pawn = PC->GetPawn())
        {
            Pawn->DisableInput(PC);
            bPawnInputDisabled_ = true;
            UE_LOG(LogPSXEmu, Log, TEXT("PadInput: Disabled default pawn input (gamepad goes to PS1 only)"));
        }
    }

    // PS1 digital pad button bits (active-low: 0=pressed, 1=released)
    uint16_t Buttons = 0xFFFF;

    auto Check = [&](int Bit, FKey Key)
    {
        if (PC->IsInputKeyDown(Key))
            Buttons &= ~(1u << Bit);
    };

    Check(0,  EKeys::Gamepad_Special_Left);         // Select  (View)
    Check(1,  EKeys::Gamepad_LeftThumbstick);        // L3
    Check(2,  EKeys::Gamepad_RightThumbstick);       // R3
    Check(3,  EKeys::Gamepad_Special_Right);         // Start   (Menu)
    Check(4,  EKeys::Gamepad_DPad_Up);               // Up
    Check(5,  EKeys::Gamepad_DPad_Right);            // Right
    Check(6,  EKeys::Gamepad_DPad_Down);             // Down
    Check(7,  EKeys::Gamepad_DPad_Left);             // Left
    Check(8,  EKeys::Gamepad_LeftTrigger);           // L2      (LT)
    Check(9,  EKeys::Gamepad_RightTrigger);          // R2      (RT)
    Check(10, EKeys::Gamepad_LeftShoulder);          // L1      (LB)
    Check(11, EKeys::Gamepad_RightShoulder);         // R1      (RB)
    Check(12, EKeys::Gamepad_FaceButton_Top);        // Triangle (Y)
    Check(13, EKeys::Gamepad_FaceButton_Right);      // Circle   (B)
    Check(14, EKeys::Gamepad_FaceButton_Bottom);     // Cross    (A)
    Check(15, EKeys::Gamepad_FaceButton_Left);       // Square   (X)

    // Debug: log when any button is pressed (throttled)
    if (Buttons != 0xFFFF)
    {
        static double LastLogTime = 0.0;
        const double Now = FPlatformTime::Seconds();
        if (Now - LastLogTime > 0.5)
        {
            UE_LOG(LogPSXEmu, Log, TEXT("PadInput: buttons=0x%04X"), Buttons);
            LastLogTime = Now;
        }
    }

    Core_->set_pad_buttons(Buttons);
}

