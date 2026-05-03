#pragma once

#include "Components/ActorComponent.h"
#include <atomic>
#include <memory>
#include <thread>

// Full definitions (not forward decls) because UHT's auto-generated .gen.cpp
// instantiates `std::unique_ptr<T>::~unique_ptr()` inside the vtable-helper
// constructor, which requires T to be complete. These headers are small and
// Windows.h-free — no transitive include-bloat concern.
#include "emu/core_mcp_backend.h"
#include "emu/mcp_server.h"

#include "PSXMcpServerComponent.generated.h"

namespace emu { class Core; }

/**
 * Exposes the emulator's MCP tools over TCP (LSP framing) so Claude Code
 * can attach while a game runs inside UE5.
 *
 * Place on the same Actor as UPSXEmulatorComponent. On BeginPlay this
 * component finds the emulator, wraps its Core in a CoreMcpBackend, and
 * runs McpServer::run_tcp on a background std::thread. EndPlay signals
 * the thread to stop and joins it.
 *
 * Single-client by design: one Claude Code connection at a time.
 */
UCLASS(ClassGroup = (PSXEmu), meta = (BlueprintSpawnableComponent))
class UPSXMcpServerComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UPSXMcpServerComponent();
    // Destructor declared out-of-line: unique_ptr<emu::McpServer/CoreMcpBackend>
    // needs the full type to destroy, which isn't visible in this header.
    // Defined as `= default` in the .cpp after the full includes.
    virtual ~UPSXMcpServerComponent();

    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

    /** TCP port to listen on (loopback only). */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|MCP",
              meta = (ClampMin = "1", ClampMax = "65535"))
    int32 Port{9743};

    /** If false, the component is inert — useful for shipping builds. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|MCP")
    bool bAutoStart{true};

    /** Deprecated compatibility switch: MCP no longer owns the pad at server start.
     *
     * Pad ownership is now dynamic: tap/observation tools take it temporarily,
     * hold/set tools keep it until a release tool. This keeps local UE input
     * alive whenever MCP is merely connected or observing.
     */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "PSXEmu|MCP")
    bool bTakePadOwnership{false};

    /** True once the TCP listener is live. False during shutdown or if startup failed. */
    UFUNCTION(BlueprintCallable, BlueprintPure, Category = "PSXEmu|MCP")
    bool IsRunning() const { return bRunning_; }

    /** Manually (re)start the server. No-op if already running. */
    UFUNCTION(BlueprintCallable, Category = "PSXEmu|MCP")
    void StartServer();

    /** Manually stop the server. Blocks briefly while the worker thread joins. */
    UFUNCTION(BlueprintCallable, Category = "PSXEmu|MCP")
    void StopServer();

private:
    // unique_ptr so the forward-declared types above stay header-light.
    std::unique_ptr<emu::CoreMcpBackend> Backend_;
    std::unique_ptr<emu::IMcpCallGuard>  CallGuard_;
    std::unique_ptr<emu::McpServer>      Server_;
    std::thread                          Worker_;
    std::atomic<bool>                    Stop_{false};
    std::atomic<int>                     StartupResult_{-1};
    bool                                 bRunning_{false};

    // Retry handle for deferred StartServer attempts (emulator core may not be
    // ready when our BeginPlay fires — component init order is undefined).
    FTimerHandle RetryTimer_;
};
