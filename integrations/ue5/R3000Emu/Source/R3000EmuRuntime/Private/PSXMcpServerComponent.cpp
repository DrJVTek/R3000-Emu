#include "PSXMcpServerComponent.h"

#include "PSXEmulatorComponent.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Logging/LogMacros.h"
#include "TimerManager.h"

#include <cstring>

#include "emu/core.h"
#include "emu/core_mcp_backend.h"
#include "emu/mcp_server.h"
#include "log/emu_log.h"

// UE5's PCH pulls in <Windows.h> (min/max macros) — the MCP sources rely on
// std::min/std::max and will fail to compile here otherwise.
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

DEFINE_LOG_CATEGORY_STATIC(LogPSXMcp, Log, All);

namespace
{
class FPsxMcpCallGuard final : public emu::IMcpCallGuard
{
public:
    explicit FPsxMcpCallGuard(UPSXEmulatorComponent* InEmu) : Emu_(InEmu) {}
    ~FPsxMcpCallGuard() override
    {
        ReleasePadOwnership();
    }

    bool before_tool_call(const char* ToolName, std::string& err) override
    {
        if (!Emu_)
        {
            err = "UE emulator component missing";
            return false;
        }
        BeginPadOwnershipForTool(ToolName ? ToolName : "");
        if (!Emu_->PauseWorkerForMcpAccess(bWasPaused_, 1.0))
        {
            EndPadOwnershipForTool(ToolName ? ToolName : "", false);
            Emu_->RestoreWorkerAfterMcpAccess(bWasPaused_);
            err = "timed out waiting for UE emulator worker to pause";
            return false;
        }
        bArmed_ = true;
        return true;
    }

    void after_tool_call(const char* ToolName, bool bSucceeded) override
    {
        EndPadOwnershipForTool(ToolName ? ToolName : "", bSucceeded);
        if (bArmed_ && Emu_)
            Emu_->RestoreWorkerAfterMcpAccess(bWasPaused_);
        bArmed_ = false;
    }

private:
    static bool IsTemporaryPadTool(const char* ToolName)
    {
        return std::strcmp(ToolName, "emu.tap_pad_buttons") == 0 ||
               std::strcmp(ToolName, "emu.tap_pad_named_buttons") == 0 ||
               std::strcmp(ToolName, "emu.step_with_pad_observation") == 0;
    }

    static bool IsPersistentPadAcquireTool(const char* ToolName)
    {
        return std::strcmp(ToolName, "emu.hold_pad_buttons") == 0 ||
               std::strcmp(ToolName, "emu.hold_pad_named_buttons") == 0 ||
               std::strcmp(ToolName, "emu.set_pad_state") == 0;
    }

    static bool IsPadReleaseTool(const char* ToolName)
    {
        return std::strcmp(ToolName, "emu.release_pad") == 0 ||
               std::strcmp(ToolName, "emu.release_pad_buttons") == 0 ||
               std::strcmp(ToolName, "emu.release_pad_named_buttons") == 0;
    }

    void AcquirePadOwnership()
    {
        if (!Emu_ || bPadOwnedByMcp_)
            return;
        Emu_->SetMcpPadInputOwned(true);
        bPadOwnedByMcp_ = true;
    }

    void ReleasePadOwnership()
    {
        if (!Emu_ || !bPadOwnedByMcp_)
            return;
        Emu_->SetMcpPadInputOwned(false);
        bPadOwnedByMcp_ = false;
    }

    void BeginPadOwnershipForTool(const char* ToolName)
    {
        (void)ToolName;
        bTemporaryPadOwnership_ = false;
        bPersistentAcquireThisCall_ = IsPersistentPadAcquireTool(ToolName);
        bReleaseThisCall_ = IsPadReleaseTool(ToolName);
        // Default mode is a hardware-style mixer, not exclusive ownership:
        // UE local input writes the local source mask, MCP writes the MCP source
        // mask, and Core exposes local&mcp to SIO0. Forced ownership can be
        // reintroduced explicitly later, but must not be the default path.
    }

    void EndPadOwnershipForTool(const char* ToolName, bool bSucceeded)
    {
        (void)ToolName;
        (void)bSucceeded;
        bTemporaryPadOwnership_ = false;
        bPersistentAcquireThisCall_ = false;
        bReleaseThisCall_ = false;
    }

    UPSXEmulatorComponent* Emu_{nullptr};
    bool bWasPaused_{false};
    bool bArmed_{false};
    bool bPadOwnedByMcp_{false};
    bool bTemporaryPadOwnership_{false};
    bool bPersistentAcquireThisCall_{false};
    bool bReleaseThisCall_{false};
};
} // namespace

UPSXMcpServerComponent::UPSXMcpServerComponent()
{
    PrimaryComponentTick.bCanEverTick = false;
}

// Out-of-line destructor so unique_ptr<emu::McpServer / CoreMcpBackend> can
// see the full types and instantiate its own destructor correctly. Without
// this the UHT-generated .gen.cpp fails with "can't delete an incomplete type".
UPSXMcpServerComponent::~UPSXMcpServerComponent() = default;

void UPSXMcpServerComponent::BeginPlay()
{
    Super::BeginPlay();
    if (bAutoStart)
        StartServer();
}

void UPSXMcpServerComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* World = GetWorld())
        World->GetTimerManager().ClearTimer(RetryTimer_);
    StopServer();
    Super::EndPlay(EndPlayReason);
}

void UPSXMcpServerComponent::StartServer()
{
    if (bRunning_ || Worker_.joinable())
        return;

    AActor* Owner = GetOwner();
    UPSXEmulatorComponent* Emu = Owner ? Owner->FindComponentByClass<UPSXEmulatorComponent>() : nullptr;
    emu::Core* Core = Emu ? Emu->GetCore() : nullptr;
    if (!Emu || !Core)
    {
        // Emulator init is async relative to our BeginPlay. Instead of ticking
        // every frame (requires bCanEverTick=true in ctor, which Live Coding
        // cannot patch into existing instances), poll via FTimerManager.
        UE_LOG(LogPSXMcp, Log, TEXT("StartServer: waiting for emulator core (emu=%s core=%s) — retry in 0.5s"),
            Emu ? TEXT("ok") : TEXT("null"), Core ? TEXT("ok") : TEXT("null"));
        if (UWorld* World = GetWorld())
        {
            World->GetTimerManager().SetTimer(
                RetryTimer_,
                FTimerDelegate::CreateUObject(this, &UPSXMcpServerComponent::StartServer),
                0.5f, /*bLoop=*/false);
        }
        return;
    }

    Backend_ = std::make_unique<emu::CoreMcpBackend>(*Core, emu::McpFrontendKind::ue5);
    CallGuard_ = std::make_unique<FPsxMcpCallGuard>(Emu);
    Server_  = std::make_unique<emu::McpServer>(*Backend_, CallGuard_.get());
    Stop_.store(false, std::memory_order_release);
    StartupResult_.store(-1, std::memory_order_release);

    const uint16_t P = static_cast<uint16_t>(FMath::Clamp(Port, 1, 65535));
    UE_LOG(LogPSXMcp, Log, TEXT("Starting MCP TCP server on port %u"), (unsigned)P);

    Worker_ = std::thread([this, P]()
    {
        // run_tcp blocks until Stop_ is signalled.
        Server_->run_tcp(P, Stop_, &StartupResult_);
    });

    const double Deadline = FPlatformTime::Seconds() + 1.0;
    while (StartupResult_.load(std::memory_order_acquire) < 0 && FPlatformTime::Seconds() < Deadline)
        FPlatformProcess::Sleep(0.001f);

    if (StartupResult_.load(std::memory_order_acquire) != 0)
    {
        UE_LOG(LogPSXMcp, Error, TEXT("MCP TCP server failed to bind/listen on port %u"), (unsigned)P);
        Stop_.store(true, std::memory_order_release);
        if (Worker_.joinable())
            Worker_.join();
        Server_.reset();
        CallGuard_.reset();
        Backend_.reset();
        bRunning_ = false;
        return;
    }

    bRunning_ = true;
}

void UPSXMcpServerComponent::StopServer()
{
    if (!bRunning_ && !Worker_.joinable())
        return;

    Stop_.store(true, std::memory_order_release);
    if (Worker_.joinable())
        Worker_.join();

    Server_.reset();
    CallGuard_.reset();
    Backend_.reset();
    bRunning_ = false;

    UE_LOG(LogPSXMcp, Log, TEXT("MCP TCP server stopped"));
}
