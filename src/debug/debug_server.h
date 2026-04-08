#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace r3000 { class Bus; }
namespace emu { class Core; }

namespace debug
{

// TCP debug server embedded in the emulator.
// Listens on a port and accepts JSON commands for real-time inspection.
// Designed for MCP (Model Context Protocol) bridge integration.
class DebugServer
{
  public:
    static constexpr uint16_t kDefaultPort = 9742;

    DebugServer();
    ~DebugServer();

    // Set emulator references (call before start)
    void set_core(emu::Core* core) { core_ = core; }

    // Start/stop the TCP server thread
    bool start(uint16_t port = kDefaultPort);
    void stop();

    bool running() const { return running_.load(std::memory_order_relaxed); }

  private:
    void server_thread_func(uint16_t port);
    void handle_client(int client_fd);
    std::string process_command(const std::string& json_line);

    // Command handlers
    std::string cmd_read_memory(uint32_t addr, uint32_t size);
    std::string cmd_read_cpu();
    std::string cmd_read_cdrom();
    std::string cmd_read_timers();
    std::string cmd_read_gpu();
    std::string cmd_read_istat();
    std::string cmd_read_game_state(); // Soul Reaver specific variables
    std::string cmd_read_dma();
    std::string cmd_read_cop0(uint32_t reg);
    std::string cmd_write_cop0(uint32_t reg, uint32_t value);
    std::string cmd_add_step_hook_write_cop0(uint32_t pc, uint32_t reg, uint32_t value, bool once);
    std::string cmd_add_step_hook_write_ram_u32(uint32_t pc, uint32_t phys_addr, uint32_t value, bool once);
    std::string cmd_list_step_hooks();
    std::string cmd_clear_step_hook(uint32_t hook_id);
    std::string cmd_clear_all_step_hooks();

    enum class StepHookKind : uint32_t
    {
        write_cop0 = 0,
        write_ram_u32 = 1,
    };

    struct StepHookRule
    {
        uint32_t id{0};
        uint32_t pc{0};
        StepHookKind kind{StepHookKind::write_cop0};
        uint32_t arg0{0};
        uint32_t arg1{0};
        bool once{true};
        bool enabled{true};
        uint64_t hit_count{0};
    };

    static void step_hook_trampoline(uint32_t pc, void* user);
    void on_step_hook(uint32_t pc);

    emu::Core* core_{nullptr};
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    int listen_fd_{-1};
    bool step_hook_registered_{false};
    std::vector<StepHookRule> step_hooks_{};
    uint32_t next_step_hook_id_{1};
};

} // namespace debug
