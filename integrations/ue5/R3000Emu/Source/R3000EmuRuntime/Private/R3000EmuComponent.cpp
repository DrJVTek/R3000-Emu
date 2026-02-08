#include "R3000EmuComponent.h"
#include "R3000AudioComponent.h"
#include "R3000GpuComponent.h"

#include "Logging/LogMacros.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstring>
#include <filesystem>

#include "emu/core.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"
#include "audio/spu.h"
#include "log/filelog.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

DEFINE_LOG_CATEGORY_STATIC(LogR3000Emu, Log, All);

// PS1 CPU clock: 33.8688 MHz
static constexpr double kPS1CpuClock = 33868800.0;

// PS1 audio: 44100 Hz, 768 CPU cycles per audio sample
static constexpr uint32 kCyclesPerSample = 768;
static constexpr uint32 kSampleRate = 44100;

//=============================================================================
// FR3000EmuWorker: Worker thread for emulation with precise timing
//=============================================================================
class FR3000EmuWorker : public FRunnable
{
public:
    FR3000EmuWorker(UR3000EmuComponent* InOwner)
        : Owner(InOwner)
#if PLATFORM_WINDOWS
        , WaitableTimer(nullptr)
#endif
    {
#if PLATFORM_WINDOWS
        // Create a high-resolution waitable timer for precise timing.
        // CREATE_WAITABLE_TIMER_HIGH_RESOLUTION requires Windows 10 1803+
        WaitableTimer = CreateWaitableTimerExW(
            nullptr, nullptr,
            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
            TIMER_ALL_ACCESS);
        if (!WaitableTimer)
        {
            // Fallback to regular timer on older Windows
            WaitableTimer = CreateWaitableTimerW(nullptr, false, nullptr);
        }
        if (WaitableTimer)
        {
            emu::logf(emu::LogLevel::info, "CORE", "Worker thread created with Windows waitable timer");
        }
        else
        {
            emu::logf(emu::LogLevel::warn, "CORE", "Worker thread: failed to create waitable timer, using Sleep fallback");
        }
#endif
    }

    virtual ~FR3000EmuWorker()
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
        emu::logf(emu::LogLevel::info, "CORE", "Emulation worker thread started");

        // Track timing for precise pacing
        uint64 LocalTotalCycles = 0;
        uint64 LocalSteps = 0;
        uint64 NextPcSampleAt = (Owner->GetPcSampleIntervalSteps() > 0)
            ? static_cast<uint64>(Owner->GetPcSampleIntervalSteps()) : 0;

        // Time tracking for waitable timer mode
        const double StartTime = FPlatformTime::Seconds();
        double LastLogTime = StartTime;

        while (!Owner->bWorkerShouldStop_.Load())
        {
            // Check if emulation is paused
            if (!Owner->IsRunning() || Owner->bWorkerPaused_.Load())
            {
                FPlatformProcess::Sleep(0.001f); // 1ms sleep when paused
                continue;
            }

            emu::Core* Core = Owner->GetCore();
            if (!Core)
            {
                FPlatformProcess::Sleep(0.001f);
                continue;
            }

            // Calculate how many cycles to run this iteration
            uint64 TargetCycles = 0;

            // FORCE wall-clock timing - audio-driven mode is broken when emu is slower than real-time
            if (false /* Owner->IsAudioDrivenTiming() && Owner->GetAudioComp() */)
            {
                // AUDIO-DRIVEN MODE: pace to audio consumption (DISABLED)
                UR3000AudioComponent* AudioComp = Owner->GetAudioComp();
                const uint64 AudioSamplesConsumed = AudioComp->GetTotalGeneratedSamples() / 2;
                const uint64 AudioDrivenCycles = AudioSamplesConsumed * kCyclesPerSample;

                // Target buffer: keep ahead by AudioBufferTargetMs worth of samples
                const uint32 BufferSamples = static_cast<uint32>(
                    (Owner->GetAudioBufferTargetMs() / 1000.0f) * kSampleRate);
                const uint64 BufferCycles = static_cast<uint64>(BufferSamples) * kCyclesPerSample;
                const uint64 RequiredCycles = AudioDrivenCycles + BufferCycles;

                if (LocalTotalCycles < RequiredCycles)
                {
                    TargetCycles = RequiredCycles - LocalTotalCycles;
                    // Cap catchup to 100ms worth
                    const uint64 MaxCatchup = static_cast<uint64>(0.1 * kPS1CpuClock);
                    if (TargetCycles > MaxCatchup)
                    {
                        emu::logf(emu::LogLevel::warn, "CORE",
                            "Worker: audio catchup clamped from %llu to %llu cycles",
                            (unsigned long long)TargetCycles, (unsigned long long)MaxCatchup);
                        TargetCycles = MaxCatchup;
                    }
                }
                else
                {
                    // Ahead of audio - but still tick bus to process CDROM/timer async events.
                    // Without this, CDROM INT2 responses never arrive because pending_irq_delay_
                    // only counts down in cdrom->tick() which requires bus.tick() to be called.
                    r3000::Bus* Bus = Core ? Core->bus() : nullptr;
                    if (Bus)
                    {
                        // Tick with ~1ms worth of cycles to keep hardware state progressing
                        constexpr uint32 kIdleTickCycles = 33869; // ~1ms at 33.8688 MHz
                        Bus->tick(kIdleTickCycles);
                        LocalTotalCycles += kIdleTickCycles;
                    }
                    WaitPrecise(0.001); // 1ms
                    continue;
                }
            }
            else
            {
                // WAITABLE TIMER MODE: run at exact PS1 speed using OS timer
                const double Now = FPlatformTime::Seconds();
                const double Elapsed = Now - StartTime;
                const uint64 TargetTotalCycles = static_cast<uint64>(Elapsed * kPS1CpuClock);

                if (LocalTotalCycles < TargetTotalCycles)
                {
                    TargetCycles = TargetTotalCycles - LocalTotalCycles;
                    // Cap to 50ms worth per iteration
                    const uint64 MaxPerIter = static_cast<uint64>(0.05 * kPS1CpuClock);
                    if (TargetCycles > MaxPerIter)
                        TargetCycles = MaxPerIter;
                }
                else
                {
                    // Ahead of real time - but still tick bus for CDROM/timer async events.
                    r3000::Bus* Bus = Core ? Core->bus() : nullptr;
                    if (Bus)
                    {
                        constexpr uint32 kIdleTickCycles = 33869; // ~1ms at 33.8688 MHz
                        Bus->tick(kIdleTickCycles);
                        LocalTotalCycles += kIdleTickCycles;
                    }
                    const double AheadBy = (LocalTotalCycles - TargetTotalCycles) / kPS1CpuClock;
                    if (AheadBy > 0.0001) // > 100us ahead
                    {
                        WaitPrecise(FMath::Min(AheadBy, 0.001)); // Max 1ms wait
                    }
                    continue;
                }
            }

            // Execute cycles in batches
            constexpr uint32 kBatchSize = 1024;
            uint64 CyclesRan = 0;
            const int32 CycleMult = FMath::Max(Owner->GetCycleMultiplier(), 1);

            while (CyclesRan < TargetCycles && !Owner->bWorkerShouldStop_.Load())
            {
                const uint32 Batch = FMath::Min(kBatchSize, static_cast<uint32>(TargetCycles - CyclesRan));

                for (uint32 i = 0; i < Batch; ++i)
                {
                    const auto Res = Core->step();
                    if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
                    {
                        emu::logf(emu::LogLevel::warn, "CORE",
                            "Worker: emu stopped kind=%d pc=0x%08X",
                            (int)Res.kind, Res.pc);
                        // Signal stop
                        Owner->bWorkerPaused_.Store(true);
                        break;
                    }
                }

                const uint64 BatchCycles = static_cast<uint64>(Batch) * CycleMult;
                CyclesRan += BatchCycles;
                LocalTotalCycles += BatchCycles;
                LocalSteps += Batch;
            }

            // Update owner stats (atomic)
            Owner->UpdateStepsExecuted(LocalSteps, LocalTotalCycles);

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
        }

        emu::logf(emu::LogLevel::info, "CORE", "Emulation worker thread exiting");
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

    UR3000EmuComponent* Owner;
#if PLATFORM_WINDOWS
    HANDLE WaitableTimer;
#endif
};

// rlog callback → UE_LOG (for CPU ASM trace when no file output)
static void UERlogCallback(rlog::Level Level, rlog::Category /*Cat*/, const char* Msg, void* /*User*/)
{
    switch (Level)
    {
    case rlog::Level::error: UE_LOG(LogR3000Emu, Error,       TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::warn:  UE_LOG(LogR3000Emu, Warning,     TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::info:  UE_LOG(LogR3000Emu, Log,         TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::debug: UE_LOG(LogR3000Emu, Verbose,     TEXT("[CPU] %hs"), Msg); break;
    case rlog::Level::trace: UE_LOG(LogR3000Emu, VeryVerbose, TEXT("[CPU] %hs"), Msg); break;
    }
}

// emu::logf callback → UE_LOG + optional file sinks per tag.
// User pointer is EmuLogFiles*.
static void UELogCallback(emu::LogLevel Level, const char* Tag, const char* Msg, void* User)
{
    switch (Level)
    {
    case emu::LogLevel::error: UE_LOG(LogR3000Emu, Error,   TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::warn:  UE_LOG(LogR3000Emu, Warning, TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::info:  UE_LOG(LogR3000Emu, Log,     TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::debug: UE_LOG(LogR3000Emu, Verbose, TEXT("[%hs] %hs"), Tag, Msg); break;
    case emu::LogLevel::trace: UE_LOG(LogR3000Emu, VeryVerbose, TEXT("[%hs] %hs"), Tag, Msg); break;
    }

    if (!User || !Tag)
        return;
    const auto* Files = static_cast<const UR3000EmuComponent::EmuLogFiles*>(User);
    if (Files->spu && std::strcmp(Tag, "SPU") == 0)
        std::fprintf(Files->spu, "[%hs] %hs\n", Tag, Msg);
    if (Files->sys && (
            std::strcmp(Tag, "CPU") == 0 ||
            std::strcmp(Tag, "BUS") == 0 ||
            std::strcmp(Tag, "CORE") == 0 ||
            std::strcmp(Tag, "ISO") == 0 ||
            std::strcmp(Tag, "GPU") == 0))
        std::fprintf(Files->sys, "[%hs] %hs\n", Tag, Msg);
}

UR3000EmuComponent::UR3000EmuComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
}

void UR3000EmuComponent::UpdateStepsExecuted(uint64 Steps, uint64 Cycles)
{
    StepsExecuted_.Store(Steps);
    TotalCyclesExecuted_.Store(Cycles);
}

void UR3000EmuComponent::StopWorkerThread()
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

static std::FILE* fopen_utf8(const char* path, const char* mode)
{
    if (!path || !mode)
        return nullptr;
#if defined(_WIN32)
    const std::filesystem::path p = std::filesystem::u8path(path);
    wchar_t wmode[8] = {};
    size_t i = 0;
    for (; mode[i] && i + 1 < (sizeof(wmode) / sizeof(wmode[0])); ++i)
        wmode[i] = (wchar_t)mode[i];
    wmode[i] = 0;
    return _wfopen(p.c_str(), wmode);
#else
    return std::fopen(path, mode);
#endif
}

bool UR3000EmuComponent::BootBiosInternal()
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
        UE_LOG(LogR3000Emu, Error, TEXT("BIOS setup failed: %hs"), err[0] ? err : "unknown error");
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
    // BIOS boot requires HLE vectors - our hardware emulation isn't accurate enough
    // for the real BIOS exception handler to work correctly without HLE interception.
    // The BIOS exception handler loops infinitely waiting for conditions that our
    // current IRQ/timer emulation doesn't satisfy precisely.
    Opt.hle_vectors = 1;
    Opt.loop_detectors = bLoopDetectors ? 1 : 0;
    Opt.bus_tick_batch = static_cast<uint32>(FMath::Clamp(BusTickBatch, 1, 128));

    if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
    {
        UE_LOG(LogR3000Emu, Error, TEXT("Core init (BIOS) failed: %hs"), err[0] ? err : "unknown error");
        emu::logf(emu::LogLevel::error, "CORE", "UE BIOS init failed: %s", err[0] ? err : "unknown error");
        return false;
    }

    // Apply cycle multiplier for timing accuracy
    Core_->set_cycle_multiplier(static_cast<uint32>(FMath::Clamp(CycleMultiplier, 1, 10)));

    UE_LOG(LogR3000Emu, Log, TEXT("BIOS boot initialized. PC=0x%08X CycleMult=%d Timing=WallClock"), Core_->pc(), CycleMultiplier);
    emu::logf(emu::LogLevel::info, "CORE", "UE BIOS init OK pc=0x%08X hle_vectors=%d bus_tick_batch=%u cycle_mult=%u timing=wallclock",
        (unsigned)Core_->pc(), Opt.hle_vectors, (unsigned)Opt.bus_tick_batch, (unsigned)CycleMultiplier);
    StepsExecuted_.Store(0);
    TotalCyclesExecuted_.Store(0);
    LastAudioSamplesConsumed_.Store(0);
    return true;
}

void UR3000EmuComponent::BeginPlay()
{
    Super::BeginPlay();
}

void UR3000EmuComponent::InitEmulator()
{
    if (Core_)
    {
        UE_LOG(LogR3000Emu, Warning, TEXT("InitEmulator called but emulator already initialized."));
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

    // Init emu::logf → UE_LOG callback.
    EmuLog_.cb = UELogCallback;
    EmuLogFiles_.spu = SpuLogFile_;
    EmuLogFiles_.sys = SysLogFile_;
    EmuLog_.user = &EmuLogFiles_;
    {
        const FTCHARToUTF8 EmuLvlUtf8(*EmuLogLevel);
        EmuLog_.max_level = emu::log_parse_level(EmuLvlUtf8.Get());
    }
    emu::log_init(&EmuLog_);

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
    char err[256];
    err[0] = '\0';
    if (!Core_->alloc_ram(2u * 1024u * 1024u, err, sizeof(err)))
    {
        UE_LOG(LogR3000Emu, Error, TEXT("R3000 core RAM alloc failed: %hs"), err[0] ? err : "unknown error");
        return;
    }

    UE_LOG(LogR3000Emu, Log, TEXT("R3000 core created (RAM allocated)."));

    // Hook HW/system log sinks to files (optional).
    if (CdLogFile_ || GpuLogFile_ || SysLogFile_ || IoLogFile_)
    {
        const flog::Clock Clock = flog::clock_start();
        const flog::Sink CdSink{CdLogFile_, flog::Level::info};
        const flog::Sink GpuSink{GpuLogFile_, flog::Level::info};
        const flog::Sink SysSink{SysLogFile_, flog::Level::info};
        const flog::Sink IoSink{IoLogFile_, flog::Level::info};
        Core_->set_log_sinks(CdSink, GpuSink, SysSink, IoSink, Clock);

        if (TextLogFile_)
        {
            Core_->set_text_out(TextLogFile_);
            const flog::Sink TextSink{TextLogFile_, flog::Level::info};
            Core_->set_text_io_sink(TextSink, Clock);
        }
    }

    // BIOS putchar → OnBiosPrint delegate.
    Core_->set_putchar_callback(&UR3000EmuComponent::PutcharCB, this);

    // Init core.
    // In fastboot mode we intentionally DO NOT load BIOS, to avoid any possibility of BIOS code/audio
    // influencing the run. (Fastboot uses HLE vectors + kernel data instead.)
    if (bFastBoot)
    {
        loader::LoadedImage Img{};
        Img.entry_pc = 0x80000000u;
        Img.has_gp = 0;
        Img.has_sp = 1;
        Img.sp = 0x801FFFF0u;

        emu::Core::InitOptions Opt{};
        Opt.pretty = bTraceASM ? 1 : 0;
        Opt.trace_io = bTraceIO ? 1 : 0;
        Opt.hle_vectors = 0; // fastboot will enable HLE vectors internally after loading EXE
        Opt.loop_detectors = bLoopDetectors ? 1 : 0;
        Opt.bus_tick_batch = static_cast<uint32>(FMath::Clamp(BusTickBatch, 1, 128));

        if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
        {
            UE_LOG(LogR3000Emu, Error, TEXT("Core init (fastboot) failed: %hs"), err[0] ? err : "unknown error");
            emu::logf(emu::LogLevel::error, "CORE", "UE fastboot init_from_image failed: %s", err[0] ? err : "unknown error");
            return;
        }
        Core_->set_cycle_multiplier(static_cast<uint32>(FMath::Clamp(CycleMultiplier, 1, 10)));
        emu::logf(emu::LogLevel::info, "CORE", "UE fastboot init: BIOS skipped pc=0x%08X cycle_mult=%u", (unsigned)Core_->pc(), (unsigned)CycleMultiplier);
    }
    else
    {
        // Optional BIOS boot.
        if (!BiosPath.IsEmpty())
        {
            BiosBytes_.Reset();
            if (!FFileHelper::LoadFileToArray(BiosBytes_, *BiosPath))
            {
                UE_LOG(LogR3000Emu, Error, TEXT("Failed to load BIOS: %s"), *BiosPath);
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
            emu::logf(emu::LogLevel::warn, "CORE", "UE BiosPath is empty (BIOS init will be skipped)");
        }
    }

    // Optional disc insert.
    if (!DiscPath.IsEmpty())
    {
        FTCHARToUTF8 DiscUtf8(*DiscPath);
        if (!Core_->insert_disc(DiscUtf8.Get(), err, sizeof(err)))
        {
            UE_LOG(LogR3000Emu, Error, TEXT("CD insert failed: %hs"), err[0] ? err : "unknown error");
        }
        else
        {
            UE_LOG(LogR3000Emu, Log, TEXT("CD inserted."));
        }
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CORE", "UE DiscPath is empty (no disc inserted)");
    }

    // Fast boot: skip BIOS, load game EXE directly from CD.
    emu::logf(emu::LogLevel::info, "CORE", "UE fastboot request=%d (bFastBoot) hle_vectors(bios)=%d",
        bFastBoot ? 1 : 0, bHleVectors ? 1 : 0);
    if (bFastBoot && Core_)
    {
        char fberr[256];
        fberr[0] = '\0';
        if (Core_->fast_boot_from_cd(fberr, sizeof(fberr)))
        {
            UE_LOG(LogR3000Emu, Log, TEXT("Fast boot OK. PC=0x%08X"), Core_->pc());
            emu::logf(emu::LogLevel::info, "CORE", "UE fastboot OK pc=0x%08X", (unsigned)Core_->pc());
        }
        else
        {
            UE_LOG(LogR3000Emu, Error, TEXT("Fast boot failed: %hs"), fberr[0] ? fberr : "unknown");
            emu::logf(emu::LogLevel::error, "CORE", "UE fastboot FAILED: %s", fberr[0] ? fberr : "unknown");
        }
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
            UE_LOG(LogR3000Emu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++InitSteps;
    }
    StepsExecuted_.Store(InitSteps);

    // Find audio component on same actor and connect SPU callback.
    AActor* Owner = GetOwner();
    AudioComp_ = Owner ? Owner->FindComponentByClass<UR3000AudioComponent>() : nullptr;
    if (AudioComp_ && Core_)
    {
        // Ensure no stale audio from a previous run can replay (e.g. BIOS jingle when toggling fastboot).
        AudioComp_->ResetBuffer(false);

        r3000::Bus* Bus = Core_->bus();
        audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
        if (Spu)
        {
            UR3000AudioComponent* Audio = AudioComp_;
            Spu->set_audio_callback([Audio](const int16_t* Samples, int Count) {
                if (Audio)
                    Audio->PushSamples(Samples, Count * 2); // Count = stereo frames, *2 for individual int16 (L,R)
            });
            AudioComp_->Start();
            UE_LOG(LogR3000Emu, Log, TEXT("SPU audio connected to UR3000AudioComponent."));
            emu::logf(emu::LogLevel::info, "CORE", "UE audio connected: gain=%.3f muted=%d",
                (double)AudioComp_->OutputGain, AudioComp_->IsMuted() ? 1 : 0);
        }
        else
        {
            UE_LOG(LogR3000Emu, Warning, TEXT("SPU not available — audio callback not connected."));
            emu::logf(emu::LogLevel::warn, "CORE", "UE audio NOT connected (SPU missing)");
        }
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CORE", "UE audio NOT connected (AudioComp=%d Core=%d)",
            AudioComp_ ? 1 : 0, Core_ ? 1 : 0);
    }

    // Find GPU component on same actor and connect emulated GPU.
    GpuComp_ = Owner ? Owner->FindComponentByClass<UR3000GpuComponent>() : nullptr;
    if (GpuComp_ && Core_)
    {
        r3000::Bus* Bus = Core_->bus();
        gpu::Gpu* Gpu = Bus ? Bus->gpu() : nullptr;
        if (Gpu)
        {
            GpuComp_->BindGpu(Gpu);
            UE_LOG(LogR3000Emu, Log, TEXT("GPU connected to UR3000GpuComponent."));
            emu::logf(emu::LogLevel::info, "CORE", "UE GPU connected: scale=%.2f zstep=%.4f",
                (double)GpuComp_->PixelScale, (double)GpuComp_->ZStep);
        }
        else
        {
            UE_LOG(LogR3000Emu, Warning, TEXT("GPU not available — GpuComponent not connected."));
        }
    }
    else
    {
        emu::logf(emu::LogLevel::warn, "CORE", "UE GPU NOT connected (GpuComp=%d Core=%d)",
            GpuComp_ ? 1 : 0, Core_ ? 1 : 0);
    }

    // Start worker thread if threaded mode is enabled.
    if (bThreadedMode)
    {
        bWorkerShouldStop_.Store(false);
        bWorkerPaused_.Store(false);

        EmuWorker_ = new FR3000EmuWorker(this);
        EmuThread_ = FRunnableThread::Create(
            EmuWorker_,
            TEXT("R3000EmuWorker"),
            0, // Default stack size
            TPri_AboveNormal, // Higher priority for accurate timing
            FPlatformAffinity::GetNoAffinityMask());

        if (EmuThread_)
        {
            UE_LOG(LogR3000Emu, Log, TEXT("Threaded emulation mode: worker thread started."));
            emu::logf(emu::LogLevel::info, "CORE", "UE threaded mode: worker thread started");
        }
        else
        {
            UE_LOG(LogR3000Emu, Error, TEXT("Failed to create emulation worker thread!"));
            delete EmuWorker_;
            EmuWorker_ = nullptr;
        }
    }

    UE_LOG(LogR3000Emu, Log, TEXT("InitEmulator done. PC=0x%08X steps=%llu threaded=%d"),
        Core_->pc(), StepsExecuted_.Load(), bThreadedMode ? 1 : 0);
}

void UR3000EmuComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bRunning || !Core_)
    {
        CyclesLastFrame_.Store(0);
        return;
    }

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
        return; // Worker thread handles all emulation
    }

    // LEGACY MODE: Run emulation in main thread (original behavior)
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
    const double StartTimeLegacy = FPlatformTime::Seconds();

    // Run in batches, checking time budget periodically.
    constexpr uint32 kBatchSize = 4096;
    uint64 CyclesRan = 0;
    uint64 LocalSteps = StepsExecuted_.Load();
    uint64 LocalTotalCycles = TotalCyclesExecuted_.Load();

    while (CyclesRan < TargetCycles)
    {
        const uint32 Batch = FMath::Min(kBatchSize, static_cast<uint32>(TargetCycles - CyclesRan));

        for (uint32 i = 0; i < Batch; ++i)
        {
            const auto Res = Core_->step();
            if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                UE_LOG(LogR3000Emu, Warning, TEXT("Emu stopped: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
                bRunning = false;
                CyclesLastFrame_.Store(static_cast<int32>(CyclesRan));
                return;
            }
        }

        // Count actual cycles consumed (Batch instructions * CycleMultiplier cycles each)
        const uint64 BatchCycles = static_cast<uint64>(Batch) * FMath::Max(CycleMultiplier, 1);
        CyclesRan += BatchCycles;
        LocalTotalCycles += BatchCycles;
        LocalSteps += Batch;

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

        // Check wall-clock budget every batch.
        if (FPlatformTime::Seconds() - StartTimeLegacy >= BudgetSeconds)
            break;
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

bool UR3000EmuComponent::ResetBiosBoot()
{
    return BootBiosInternal();
}

int32 UR3000EmuComponent::StepInstructions(int32 Steps)
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
            UE_LOG(LogR3000Emu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++Executed;
        ++CurrentSteps;
    }
    StepsExecuted_.Store(CurrentSteps);
    return Executed;
}

int32 UR3000EmuComponent::GetProgramCounter() const
{
    return Core_ ? (int32)Core_->pc() : 0;
}

void UR3000EmuComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
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
        UE_LOG(LogR3000Emu, Log, TEXT("R3000 core destroyed."));
    }
    if (AudioComp_)
    {
        AudioComp_->Stop();
        AudioComp_->ResetBuffer(false);
    }
    AudioComp_ = nullptr;
    GpuComp_ = nullptr;
    emu::log_init(nullptr);

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

void UR3000EmuComponent::PutcharCB(char Ch, void* User)
{
    auto* Self = static_cast<UR3000EmuComponent*>(User);
    if (!Self)
        return;

    if (Ch == '\n' || Ch == '\r')
    {
        if (!Self->PutcharLineBuf_.IsEmpty())
        {
            Self->OnBiosPrint.Broadcast(Self->PutcharLineBuf_);
            Self->PutcharLineBuf_.Reset();
        }
    }
    else
    {
        Self->PutcharLineBuf_.AppendChar(static_cast<TCHAR>(Ch));
    }
}

