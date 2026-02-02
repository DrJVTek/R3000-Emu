#include "R3000EmuComponent.h"
#include "R3000AudioComponent.h"

#include "Logging/LogMacros.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include <cstring>
#include <filesystem>

#include "emu/core.h"
#include "r3000/bus.h"
#include "audio/spu.h"
#include "log/filelog.h"

DEFINE_LOG_CATEGORY_STATIC(LogR3000Emu, Log, All);

// PS1 CPU clock: 33.8688 MHz
static constexpr double kPS1CpuClock = 33868800.0;

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
    if (Files->sys && (std::strcmp(Tag, "CPU") == 0 || std::strcmp(Tag, "BUS") == 0))
        std::fprintf(Files->sys, "[%hs] %hs\n", Tag, Msg);
}

UR3000EmuComponent::UR3000EmuComponent()
{
    PrimaryComponentTick.bCanEverTick = true;
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
        return false;

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
    Opt.hle_vectors = 1;
    Opt.loop_detectors = bLoopDetectors ? 1 : 0;
    Opt.bus_tick_batch = static_cast<uint32>(FMath::Clamp(BusTickBatch, 1, 128));

    if (!Core_->init_from_image(Img, Opt, err, sizeof(err)))
    {
        UE_LOG(LogR3000Emu, Error, TEXT("Core init (BIOS) failed: %hs"), err[0] ? err : "unknown error");
        return false;
    }

    UE_LOG(LogR3000Emu, Log, TEXT("BIOS boot initialized. PC=0x%08X"), Core_->pc());
    StepsExecuted_ = 0;
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

    // Optional BIOS boot.
    if (!BiosPath.IsEmpty())
    {
        BiosBytes_.Reset();
        if (!FFileHelper::LoadFileToArray(BiosBytes_, *BiosPath))
        {
            UE_LOG(LogR3000Emu, Error, TEXT("Failed to load BIOS: %s"), *BiosPath);
            return;
        }
        BootBiosInternal();
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

    // Fast boot: skip BIOS, load game EXE directly from CD.
    if (bFastBoot && Core_)
    {
        char fberr[256];
        fberr[0] = '\0';
        if (Core_->fast_boot_from_cd(fberr, sizeof(fberr)))
        {
            UE_LOG(LogR3000Emu, Log, TEXT("Fast boot OK. PC=0x%08X"), Core_->pc());
        }
        else
        {
            UE_LOG(LogR3000Emu, Error, TEXT("Fast boot failed: %hs"), fberr[0] ? fberr : "unknown");
        }
    }

    // Optional run N steps immediately.
    StepsExecuted_ = 0;
    const int32 N = StepsToRunOnBeginPlay;
    for (int32 i = 0; i < N; ++i)
    {
        const auto Res = Core_->step();
        if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
        {
            UE_LOG(LogR3000Emu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++StepsExecuted_;
    }

    // Find audio component on same actor and connect SPU callback.
    AActor* Owner = GetOwner();
    AudioComp_ = Owner ? Owner->FindComponentByClass<UR3000AudioComponent>() : nullptr;
    if (AudioComp_ && Core_)
    {
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
        }
        else
        {
            UE_LOG(LogR3000Emu, Warning, TEXT("SPU not available — audio callback not connected."));
        }
    }

    UE_LOG(LogR3000Emu, Log, TEXT("InitEmulator done. PC=0x%08X steps=%llu"), Core_->pc(), StepsExecuted_);
}

void UR3000EmuComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
    Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

    if (!bRunning || !Core_)
    {
        CyclesLastFrame_ = 0;
        return;
    }

    // Target cycles for this frame based on real PS1 clock.
    const uint32 TargetCycles = static_cast<uint32>(
        FMath::Clamp(DeltaTime, 0.0f, 0.05f) * kPS1CpuClock * EmulationSpeed);

    const double BudgetSeconds = FMath::Max(BudgetMs, 1.0f) * 0.001;
    const double StartTime = FPlatformTime::Seconds();

    // Run in batches, checking time budget periodically.
    constexpr uint32 kBatchSize = 4096;
    uint32 CyclesRan = 0;

    while (CyclesRan < TargetCycles)
    {
        const uint32 Batch = FMath::Min(kBatchSize, TargetCycles - CyclesRan);

        for (uint32 i = 0; i < Batch; ++i)
        {
            const auto Res = Core_->step();
            if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                UE_LOG(LogR3000Emu, Warning, TEXT("Emu stopped: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
                bRunning = false;
                CyclesLastFrame_ = static_cast<int32>(CyclesRan);
                return;
            }
        }

        CyclesRan += Batch;
        StepsExecuted_ += Batch;

        // Check wall-clock budget every batch.
        if (FPlatformTime::Seconds() - StartTime >= BudgetSeconds)
            break;
    }

    CyclesLastFrame_ = static_cast<int32>(CyclesRan);

    // Flush any remaining SPU samples to the audio ring buffer.
    // At high framerates (e.g. 1000fps), the SPU internal buffer may not fill
    // to its flush threshold within a single frame, causing audio dropouts.
    r3000::Bus* Bus = Core_ ? Core_->bus() : nullptr;
    audio::Spu* Spu = Bus ? Bus->spu() : nullptr;
    if (Spu)
        Spu->flush_audio();
}

bool UR3000EmuComponent::ResetBiosBoot()
{
    return BootBiosInternal();
}

int32 UR3000EmuComponent::StepInstructions(int32 Steps)
{
    if (!Core_ || Steps <= 0)
        return 0;

    int32 Executed = 0;
    for (int32 i = 0; i < Steps; ++i)
    {
        const auto Res = Core_->step();
        if (Res.kind != r3000::Cpu::StepResult::Kind::ok)
        {
            UE_LOG(LogR3000Emu, Warning, TEXT("Stop stepping: kind=%d PC=0x%08X"), (int32)Res.kind, Res.pc);
            break;
        }
        ++Executed;
        ++StepsExecuted_;
    }
    return Executed;
}

int32 UR3000EmuComponent::GetProgramCounter() const
{
    return Core_ ? (int32)Core_->pc() : 0;
}

void UR3000EmuComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
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
    AudioComp_ = nullptr;
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

