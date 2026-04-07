#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

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

    emu::Core* core_{nullptr};
    std::thread server_thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> should_stop_{false};
    int listen_fd_{-1};
};

} // namespace debug
