#include "debug_server.h"
#include "../emu/core.h"
#include "../r3000/bus.h"
#include "../r3000/cpu.h"
#include "../cdrom/cdrom.h"
#include "../gpu/gpu.h"
#include "../log/emu_log.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using socket_t = SOCKET;
#define CLOSE_SOCKET closesocket
#define INVALID_SOCK INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <arpa/inet.h>
using socket_t = int;
#define CLOSE_SOCKET close
#define INVALID_SOCK (-1)
#endif

#include <cstdio>
#include <cstring>
#include <sstream>

namespace debug
{

DebugServer::DebugServer()
{
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

DebugServer::~DebugServer()
{
    stop();
#ifdef _WIN32
    WSACleanup();
#endif
}

bool DebugServer::start(uint16_t port)
{
    if (running_.load()) return true;
    should_stop_.store(false);
    server_thread_ = std::thread(&DebugServer::server_thread_func, this, port);
    return true;
}

void DebugServer::stop()
{
    should_stop_.store(true, std::memory_order_release);
    if (listen_fd_ != -1)
    {
        CLOSE_SOCKET((socket_t)listen_fd_);
        listen_fd_ = -1;
    }
    if (server_thread_.joinable())
        server_thread_.join();
    running_.store(false);
}

void DebugServer::server_thread_func(uint16_t port)
{
    socket_t fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == INVALID_SOCK)
    {
        emu::logf(emu::LogLevel::error, "DEBUG", "Failed to create socket");
        return;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // localhost only
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
    {
        emu::logf(emu::LogLevel::error, "DEBUG", "Failed to bind port %u", port);
        CLOSE_SOCKET(fd);
        return;
    }

    listen(fd, 2);
    listen_fd_ = (int)fd;
    running_.store(true, std::memory_order_release);
    emu::logf(emu::LogLevel::warn, "DEBUG", "Debug server listening on port %u", port);

    while (!should_stop_.load(std::memory_order_acquire))
    {
        struct sockaddr_in client_addr{};
        int client_len = sizeof(client_addr);
        socket_t client = accept(fd, (struct sockaddr*)&client_addr, (socklen_t*)&client_len);
        if (client == INVALID_SOCK) break;

        emu::logf(emu::LogLevel::warn, "DEBUG", "Client connected");
        handle_client((int)client);
        CLOSE_SOCKET(client);
        emu::logf(emu::LogLevel::warn, "DEBUG", "Client disconnected");
    }

    CLOSE_SOCKET(fd);
    listen_fd_ = -1;
    running_.store(false);
}

void DebugServer::handle_client(int client_fd)
{
    char buf[4096];
    while (!should_stop_.load(std::memory_order_acquire))
    {
        int n = recv((socket_t)client_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) break;
        buf[n] = '\0';

        // Process line by line
        std::istringstream ss(buf);
        std::string line;
        while (std::getline(ss, line))
        {
            if (line.empty() || line[0] == '\n') continue;
            // Trim \r
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            std::string response = process_command(line);
            response += "\n";
            send((socket_t)client_fd, response.c_str(), (int)response.size(), 0);
        }
    }
}

// Simple JSON helpers (no dependency)
static std::string json_str(const char* key, const char* val)
{
    std::string s = "\""; s += key; s += "\":\""; s += val; s += "\"";
    return s;
}
static std::string json_num(const char* key, uint64_t val)
{
    char tmp[64]; std::snprintf(tmp, sizeof(tmp), "\"%s\":%llu", key, (unsigned long long)val);
    return tmp;
}
static std::string json_hex(const char* key, uint32_t val)
{
    char tmp[64]; std::snprintf(tmp, sizeof(tmp), "\"%s\":\"0x%08X\"", key, val);
    return tmp;
}

std::string DebugServer::process_command(const std::string& line)
{
    // Simple command parsing: {"cmd":"read_cpu"} or {"cmd":"read_memory","addr":123,"size":16}
    // Minimal JSON parsing — just find key values

    auto find_str = [&](const char* key) -> std::string {
        std::string k = "\""; k += key; k += "\"";
        auto pos = line.find(k);
        if (pos == std::string::npos) return "";
        pos = line.find(':', pos + k.size());
        if (pos == std::string::npos) return "";
        pos = line.find('"', pos + 1);
        if (pos == std::string::npos) return "";
        auto end = line.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return line.substr(pos + 1, end - pos - 1);
    };

    auto find_num = [&](const char* key) -> uint32_t {
        std::string k = "\""; k += key; k += "\"";
        auto pos = line.find(k);
        if (pos == std::string::npos) return 0;
        pos = line.find(':', pos + k.size());
        if (pos == std::string::npos) return 0;
        return (uint32_t)strtoul(line.c_str() + pos + 1, nullptr, 0);
    };

    std::string cmd = find_str("cmd");

    if (cmd == "read_cpu")        return cmd_read_cpu();
    if (cmd == "read_memory")     return cmd_read_memory(find_num("addr"), find_num("size"));
    if (cmd == "read_cdrom")      return cmd_read_cdrom();
    if (cmd == "read_timers")     return cmd_read_timers();
    if (cmd == "read_gpu")        return cmd_read_gpu();
    if (cmd == "read_istat")      return cmd_read_istat();
    if (cmd == "read_game_state") return cmd_read_game_state();
    if (cmd == "read_dma")        return cmd_read_dma();
    if (cmd == "ping")            return "{\"ok\":true,\"msg\":\"pong\"}";

    return "{\"error\":\"unknown command\"}";
}

std::string DebugServer::cmd_read_cpu()
{
    if (!core_ || !core_->cpu()) return "{\"error\":\"no cpu\"}";
    const auto* cpu = core_->cpu();
    char buf[2048];
    std::snprintf(buf, sizeof(buf),
        "{\"pc\":\"0x%08X\",\"gpr\":[", cpu->pc());

    std::string s = buf;
    for (int i = 0; i < 32; ++i)
    {
        char tmp[16]; std::snprintf(tmp, sizeof(tmp), "\"0x%08X\"", cpu->gpr(i));
        if (i > 0) s += ",";
        s += tmp;
    }
    s += "],";
    char cop0buf[256];
    std::snprintf(cop0buf, sizeof(cop0buf),
        "\"cop0_status\":\"0x%08X\",\"cop0_cause\":\"0x%08X\",\"cop0_epc\":\"0x%08X\"}",
        cpu->cop0(12), cpu->cop0(13), cpu->cop0(14));
    s += cop0buf;
    return s;
}

std::string DebugServer::cmd_read_memory(uint32_t addr, uint32_t size)
{
    if (!core_ || !core_->bus()) return "{\"error\":\"no bus\"}";
    if (size == 0) size = 64;
    if (size > 4096) size = 4096;

    const uint8_t* ram = core_->bus()->ram_ptr();
    const uint32_t ram_size = core_->bus()->ram_size();
    const uint32_t phys = addr & 0x1FFFFFu; // mask to 2MB

    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"addr\":\"0x%08X\",\"size\":%u,\"hex\":\"", addr, size);
    std::string s = buf;

    for (uint32_t i = 0; i < size && (phys + i) < ram_size; ++i)
    {
        char hx[4]; std::snprintf(hx, sizeof(hx), "%02X", ram[phys + i]);
        s += hx;
    }
    s += "\"}";
    return s;
}

std::string DebugServer::cmd_read_cdrom()
{
    if (!core_ || !core_->bus() || !core_->bus()->cdrom()) return "{\"error\":\"no cdrom\"}";
    auto* cd = core_->bus()->cdrom();
    char buf[1024];
    std::snprintf(buf, sizeof(buf),
        "{\"read_lba\":%u,\"irq_flags\":\"0x%02X\",\"reading\":%d,"
        "\"now_cycles\":%llu,"
        "\"pend_type\":%u,\"pend_due\":%llu,\"irq_ready\":%llu,"
        "\"read_due\":%llu,\"want_data\":%d,\"data_ready\":%d,"
        "\"int1_count\":%u,\"sig_fire\":%u,\"sig_consumed\":%u}",
        cd->read_lba_debug(), cd->irq_flags_debug(),
        cd->is_reading_active() ? 1 : 0,
        (unsigned long long)cd->now_cycles_debug(),
        (unsigned)cd->pending_irq_type_debug(),
        (unsigned long long)cd->pending_irq_due_debug(),
        (unsigned long long)cd->next_irq_ready_debug(),
        (unsigned long long)cd->next_read_due_debug(),
        (int)cd->want_data_debug(),
        (int)cd->data_ready_pending_debug(),
        cd->int1_deliver_count_debug(),
        cd->sector_fire_count_debug(),
        cd->sector_consumed_count_debug());
    return buf;
}

std::string DebugServer::cmd_read_timers()
{
    if (!core_ || !core_->bus()) return "{\"error\":\"no bus\"}";
    const auto* bus = core_->bus();
    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"timers\":[{\"count\":%u,\"mode\":\"0x%04X\",\"target\":%u},"
        "{\"count\":%u,\"mode\":\"0x%04X\",\"target\":%u},"
        "{\"count\":%u,\"mode\":\"0x%04X\",\"target\":%u}]}",
        bus->timer_compute_count(0), bus->timer_mode(0), bus->timer_target(0),
        bus->timer_compute_count(1), bus->timer_mode(1), bus->timer_target(1),
        bus->timer_compute_count(2), bus->timer_mode(2), bus->timer_target(2));
    return buf;
}

std::string DebugServer::cmd_read_gpu()
{
    if (!core_ || !core_->gpu()) return "{\"error\":\"no gpu\"}";
    auto* gpu = core_->gpu();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"gpustat\":\"0x%08X\",\"frame_count\":%u,\"vram_frame\":%u}",
        gpu->mmio_read32(0x1F801814u),
        gpu->frame_count(),
        gpu->vram_frame_count());
    return buf;
}

std::string DebugServer::cmd_read_istat()
{
    if (!core_ || !core_->bus()) return "{\"error\":\"no bus\"}";
    const auto* bus = core_->bus();
    char buf[256];
    std::snprintf(buf, sizeof(buf),
        "{\"i_stat\":\"0x%04X\",\"i_mask\":\"0x%04X\",\"pending\":\"0x%04X\",\"vblank_count\":%u}",
        bus->irq_stat_raw(), bus->irq_mask_raw(),
        bus->irq_stat_raw() & bus->irq_mask_raw(),
        bus->vblank_count());
    return buf;
}

std::string DebugServer::cmd_read_game_state()
{
    if (!core_ || !core_->bus()) return "{\"error\":\"no bus\"}";
    const uint8_t* ram = core_->bus()->ram_ptr();
    const uint32_t rs = core_->bus()->ram_size();
    auto rd32 = [&](uint32_t vaddr) -> uint32_t {
        uint32_t p = vaddr & (rs - 1);
        return *(uint32_t*)(ram + p);
    };

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"state\":%u,\"vidx\":%u,"
        "\"dd9c0\":%u,\"dd9ac\":%u,"
        "\"cb6e4\":\"0x%08X\","
        "\"cd2e0\":\"0x%08X\",\"cd2e4\":\"0x%08X\","
        "\"sw_mask\":\"0x%04X\",\"cd_handler\":\"0x%08X\","
        "\"sync_flag\":%u,\"ready_flag\":%u,"
        "\"d1540\":%u,\"d1528\":%u}",
        rd32(0x800d19ac), rd32(0x800d19b4),
        rd32(0x800dd9c0), rd32(0x800dd9ac),
        rd32(0x800cb6e4),
        rd32(0x800cd2e0), rd32(0x800cd2e4),
        rd32(0x800cb7b0), rd32(0x800cb78c),
        rd32(0x800cd5bc) & 0xFF, rd32(0x800cd5bd) & 0xFF,
        rd32(0x800d1540), rd32(0x800d1528));
    return buf;
}

std::string DebugServer::cmd_read_dma()
{
    if (!core_ || !core_->bus()) return "{\"error\":\"no bus\"}";
    // Read DMA channel registers via bus MMIO
    char buf[1024];
    std::string s = "{\"channels\":[";
    for (int ch = 0; ch < 7; ++ch)
    {
        uint32_t base = 0x1F801080 + ch * 0x10;
        uint32_t madr = 0, bcr = 0, chcr = 0;
        r3000::Bus::MemFault f;
        core_->bus()->read_u32(base + 0, madr, f);
        core_->bus()->read_u32(base + 4, bcr, f);
        core_->bus()->read_u32(base + 8, chcr, f);
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp),
            "%s{\"ch\":%d,\"madr\":\"0x%08X\",\"bcr\":\"0x%08X\",\"chcr\":\"0x%08X\"}",
            ch > 0 ? "," : "", ch, madr, bcr, chcr);
        s += tmp;
    }
    s += "]}";
    return s;
}

} // namespace debug
