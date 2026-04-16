#include <cinttypes>
#include <chrono>
#include <cstdint>
#include <cstddef>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../src/debug/debug_server.h"

#if defined(_WIN32)
#include <direct.h>
#include <codecvt>
#include <fcntl.h>
#include <io.h>
#include <locale>
#include <map>
#include <string>
#endif

#include "emu/core.h"
#include "emu/hooks.h"
#include "emu/mcp_server.h"
#include "gpu/gpu.h"
#include "gpu/gpu_3d.h"
#include "gte/gte_backend_kind.h"
#include "loader/loader.h"
#include "log/async_log.h"
#include "log/emu_log.h"
#include "log/filelog.h"
#include "log/logger.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"

static const char* arg_value(int argc, char** argv, const char* key_prefix)
{
    const size_t n = std::strlen(key_prefix);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key_prefix, n) == 0)
            return argv[i] + n;
    }
    return nullptr;
}

static int has_flag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], flag) == 0)
            return 1;
    }
    return 0;
}

namespace
{

enum class CliDevkitMode
{
    none,
    devkit,
    devkit_hle,
};

static CliDevkitMode parse_devkit_mode(int argc, char** argv)
{
    const int devkit = has_flag(argc, argv, "--devkit");
    const int devkit_hle = has_flag(argc, argv, "--devkit-hle");
    if (devkit_hle)
        return CliDevkitMode::devkit_hle;
    if (devkit)
        return CliDevkitMode::devkit;
    return CliDevkitMode::none;
}

static const char* cli_devkit_mode_name(CliDevkitMode mode)
{
    switch (mode)
    {
    case CliDevkitMode::none: return "none";
    case CliDevkitMode::devkit: return "devkit";
    case CliDevkitMode::devkit_hle: return "devkit-hle";
    }
    return "none";
}

} // namespace

namespace
{

struct McpLogEntry
{
    uint64_t seq{0};
    uint64_t ts_ns{0};
    emu::LogLevel level{emu::LogLevel::info};
    char tag[32]{};
    char msg[256]{};
};

struct McpLogSink
{
    static constexpr size_t kCap = 512;
    std::mutex mtx{};
    std::array<McpLogEntry, kCap> ring{};
    uint32_t head{0};
    uint32_t count{0};
    uint64_t next_seq{1};
};

static McpLogSink g_mcp_log_sink{};

static const char* log_level_name(emu::LogLevel lvl)
{
    switch (lvl)
    {
    case emu::LogLevel::error: return "error";
    case emu::LogLevel::warn: return "warn";
    case emu::LogLevel::info: return "info";
    case emu::LogLevel::debug: return "debug";
    case emu::LogLevel::trace: return "trace";
    }
    return "unknown";
}

static bool str_contains_ci(const char* haystack, const char* needle)
{
    if (!needle || !*needle)
        return true;
    if (!haystack)
        return false;
    const size_t nlen = std::strlen(needle);
    if (nlen == 0)
        return true;
    for (const char* h = haystack; *h; ++h)
    {
        size_t i = 0;
        while (i < nlen)
        {
            const unsigned char hc = (unsigned char)h[i];
            const unsigned char nc = (unsigned char)needle[i];
            if (!hc)
                return false;
            if (std::tolower(hc) != std::tolower(nc))
                break;
            ++i;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

static void mcp_async_log_consumer(uint64_t ts_ns, emu::LogLevel level, const char* tag, const char* msg, void* /*user*/)
{
    static const char* level_str[] = {"ERROR","WARN ","INFO ","DEBUG","TRACE"};
    const uint8_t lvl = ((uint8_t)level < 5u) ? (uint8_t)level : 4u;
    std::fprintf(stderr, "[%s] [%s] %s\n", level_str[lvl], tag ? tag : "", msg ? msg : "");

    McpLogSink& sink = g_mcp_log_sink;
    std::lock_guard<std::mutex> lock(sink.mtx);
    McpLogEntry entry{};
    entry.seq = sink.next_seq++;
    entry.ts_ns = ts_ns;
    entry.level = level;
    std::snprintf(entry.tag, sizeof(entry.tag), "%s", tag ? tag : "");
    std::snprintf(entry.msg, sizeof(entry.msg), "%s", msg ? msg : "");
    sink.ring[sink.head] = entry;
    sink.head = (sink.head + 1u) % (uint32_t)sink.kCap;
    if (sink.count < sink.kCap)
        ++sink.count;
}

} // namespace

class CliMcpBackend final : public emu::IMcpBackend
{
public:
    explicit CliMcpBackend(emu::Core& core) : core_(core)
    {
        core_.hooks().add_step(&CliMcpBackend::step_hook_trampoline, this);
        core_.hooks().add_write(&CliMcpBackend::write_hook_trampoline, this);
    }

    emu::McpFrontendKind frontend_kind() const override
    {
        return emu::McpFrontendKind::cli;
    }

    bool get_status(emu::McpStatus& out) const override
    {
        out = {};
        out.has_core = true;
        out.has_cpu = (core_.cpu() != nullptr);
        out.analysis_enabled = core_.psx3d_analysis_enabled();
        out.analysis_mode = core_.psx3d_mode() == emu::Psx3dRunMode::analysis;
        out.profile_override = core_.psx3d_profile_override();
        out.profile_path = core_.psx3d_profile_path();
        out.profile_game_id = core_.psx3d_profile_game_id();
        if (core_.gpu_3d())
        {
            out.frame_count = core_.gpu_3d()->frame_count();
            out.tri_3d = core_.gpu_3d()->dbg_last_3d_;
            out.tri_2d = core_.gpu_3d()->dbg_last_2d_;
        }
        return true;
    }

    bool get_cpu_state(emu::McpCpuState& out) const override
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
            return false;
        out = {};
        out.pc = cpu->pc();
        out.hi = cpu->hi();
        out.lo = cpu->lo();
        for (uint32_t i = 0; i < 32; ++i)
            out.gpr[i] = cpu->gpr(i);
        return true;
    }

    bool step(uint32_t count, std::string& err) override
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "step stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                return false;
            }
        }
        return true;
    }

    bool read_ram_u32(uint32_t phys_addr, uint32_t& out, std::string& err) const override
    {
        const uint8_t* ram = core_.ram();
        if (!ram)
        {
            err = "ram not allocated";
            return false;
        }
        if ((uint64_t)phys_addr + 4u > core_.ram_size())
        {
            err = "phys_addr out of range";
            return false;
        }
        out = (uint32_t)ram[phys_addr]
            | ((uint32_t)ram[phys_addr + 1] << 8)
            | ((uint32_t)ram[phys_addr + 2] << 16)
            | ((uint32_t)ram[phys_addr + 3] << 24);
        return true;
    }

    bool read_cop0(uint32_t reg, uint32_t& out, std::string& err) const override
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }
        if (reg >= 32u)
        {
            err = "cop0 reg out of range";
            return false;
        }
        out = cpu->cop0(reg);
        return true;
    }

    bool write_cop0(uint32_t reg, uint32_t value, std::string& err) override
    {
        r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }
        if (reg >= 32u)
        {
            err = "cop0 reg out of range";
            return false;
        }
        cpu->set_cop0(reg, value);
        return true;
    }

    bool add_step_hook_write_cop0(uint32_t pc, uint32_t reg, uint32_t value, bool once, uint32_t& hook_id, std::string& err)
    {
        if (reg >= 32u)
        {
            err = "cop0 reg out of range";
            return false;
        }
        StepHookRule rule{};
        rule.id = next_step_hook_id_++;
        rule.pc = pc;
        rule.kind = StepHookKind::write_cop0;
        rule.arg0 = reg;
        rule.arg1 = value;
        rule.once = once;
        rule.enabled = true;
        step_hooks_.push_back(rule);
        hook_id = rule.id;
        return true;
    }

    bool add_step_hook_write_ram_u32(uint32_t pc, uint32_t phys_addr, uint32_t value, bool once, uint32_t& hook_id, std::string& err)
    {
        uint8_t* ram = core_.ram();
        if (!ram)
        {
            err = "ram not allocated";
            return false;
        }
        if ((uint64_t)phys_addr + 4u > core_.ram_size())
        {
            err = "phys_addr out of range";
            return false;
        }
        StepHookRule rule{};
        rule.id = next_step_hook_id_++;
        rule.pc = pc;
        rule.kind = StepHookKind::write_ram_u32;
        rule.arg0 = phys_addr;
        rule.arg1 = value;
        rule.once = once;
        rule.enabled = true;
        step_hooks_.push_back(rule);
        hook_id = rule.id;
        return true;
    }

    bool list_step_hooks(std::string& out_json, std::string& err) const
    {
        (void)err;
        out_json = "{\"step_hooks\":[";
        for (size_t i = 0; i < step_hooks_.size(); ++i)
        {
            const auto& h = step_hooks_[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"id\":") + std::to_string(h.id) +
                ",\"pc\":" + std::to_string(h.pc) +
                ",\"kind\":\"" + std::string(h.kind == StepHookKind::write_cop0 ? "write_cop0" : "write_ram_u32") + "\"" +
                ",\"arg0\":" + std::to_string(h.arg0) +
                ",\"arg1\":" + std::to_string(h.arg1) +
                ",\"once\":" + (h.once ? "true" : "false") +
                ",\"enabled\":" + (h.enabled ? "true" : "false") +
                ",\"hit_count\":" + std::to_string(h.hit_count) + "}";
        }
        out_json += "]}";
        return true;
    }

    bool clear_step_hook(uint32_t hook_id, bool& removed, std::string& err)
    {
        (void)err;
        removed = false;
        for (size_t i = 0; i < step_hooks_.size(); ++i)
        {
            if (step_hooks_[i].id == hook_id)
            {
                step_hooks_.erase(step_hooks_.begin() + (ptrdiff_t)i);
                removed = true;
                return true;
            }
        }
        return true;
    }

    bool clear_all_step_hooks(uint32_t& removed_count, std::string& err)
    {
        (void)err;
        removed_count = (uint32_t)step_hooks_.size();
        step_hooks_.clear();
        return true;
    }

    bool add_mem_watch_write(uint32_t phys_addr_start, uint32_t phys_addr_end, bool has_pc, uint32_t pc,
        bool has_value, uint32_t value, bool once, uint32_t& watch_id, std::string& err) override
    {
        if (phys_addr_end < phys_addr_start)
        {
            err = "phys_addr_end before phys_addr_start";
            return false;
        }
        if ((uint64_t)phys_addr_end >= core_.ram_size())
        {
            err = "phys_addr_end out of range";
            return false;
        }
        MemWatchRule rule{};
        rule.id = next_mem_watch_id_++;
        rule.phys_addr_start = phys_addr_start;
        rule.phys_addr_end = phys_addr_end;
        rule.has_pc = has_pc;
        rule.pc = pc;
        rule.has_value = has_value;
        rule.value = value;
        rule.once = once;
        rule.enabled = true;
        mem_watches_.push_back(rule);
        watch_id = rule.id;
        return true;
    }

    bool list_mem_watches(std::string& out_json, std::string& err) const override
    {
        (void)err;
        out_json = "{\"mem_watches\":[";
        for (size_t i = 0; i < mem_watches_.size(); ++i)
        {
            const auto& w = mem_watches_[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"id\":") + std::to_string(w.id) +
                ",\"phys_addr_start\":" + std::to_string(w.phys_addr_start) +
                ",\"phys_addr_end\":" + std::to_string(w.phys_addr_end) +
                ",\"has_pc\":" + (w.has_pc ? "true" : "false") +
                ",\"pc\":" + std::to_string(w.pc) +
                ",\"has_value\":" + (w.has_value ? "true" : "false") +
                ",\"value\":" + std::to_string(w.value) +
                ",\"once\":" + (w.once ? "true" : "false") +
                ",\"enabled\":" + (w.enabled ? "true" : "false") +
                ",\"hit_count\":" + std::to_string(w.hit_count) + "}";
        }
        out_json += "]}";
        return true;
    }

    bool clear_mem_watch(uint32_t watch_id, bool& removed, std::string& err) override
    {
        (void)err;
        removed = false;
        for (size_t i = 0; i < mem_watches_.size(); ++i)
        {
            if (mem_watches_[i].id == watch_id)
            {
                mem_watches_.erase(mem_watches_.begin() + (ptrdiff_t)i);
                removed = true;
                return true;
            }
        }
        return true;
    }

    bool clear_all_mem_watches(uint32_t& removed_count, std::string& err) override
    {
        (void)err;
        removed_count = (uint32_t)mem_watches_.size();
        mem_watches_.clear();
        return true;
    }

    bool list_mem_watch_events(std::string& out_json, std::string& err) const override
    {
        (void)err;
        out_json = "{\"events\":[";
        const uint32_t count = mem_watch_event_count_;
        const uint32_t start = (mem_watch_event_head_ + kMaxMemWatchEvents - count) % kMaxMemWatchEvents;
        for (uint32_t i = 0; i < count; ++i)
        {
            const auto& e = mem_watch_events_[(start + i) % kMaxMemWatchEvents];
            if (i)
                out_json += ",";
            out_json += std::string("{\"seq\":") + std::to_string(e.seq) +
                ",\"watch_id\":" + std::to_string(e.watch_id) +
                ",\"pc\":" + std::to_string(e.pc) +
                ",\"phys_addr\":" + std::to_string(e.phys_addr) +
                ",\"value\":" + std::to_string(e.value) +
                ",\"size\":" + std::to_string(e.size) + "}";
        }
        out_json += "]}";
        return true;
    }

    bool clear_mem_watch_events(uint32_t& cleared_count, std::string& err) override
    {
        (void)err;
        cleared_count = mem_watch_event_count_;
        mem_watch_event_count_ = 0;
        mem_watch_event_head_ = 0;
        return true;
    }

    bool list_logs(uint64_t since_seq, bool has_min_level, uint32_t min_level,
        const char* tag, const char* contains, uint32_t max_entries, std::string& out_json, std::string& err) const override
    {
        (void)err;
        if (has_min_level && min_level > 4u)
        {
            err = "min_level out of range";
            return false;
        }

        std::lock_guard<std::mutex> lock(g_mcp_log_sink.mtx);
        const uint32_t count = g_mcp_log_sink.count;
        const uint32_t start = (g_mcp_log_sink.head + (uint32_t)McpLogSink::kCap - count) % (uint32_t)McpLogSink::kCap;
        out_json = "{\"logs\":[";
        uint32_t added = 0;
        uint64_t newest_seq = 0;
        for (uint32_t i = 0; i < count; ++i)
        {
            const McpLogEntry& e = g_mcp_log_sink.ring[(start + i) % (uint32_t)McpLogSink::kCap];
            newest_seq = e.seq;
            if (e.seq <= since_seq)
                continue;
            if (has_min_level && (uint32_t)e.level > min_level)
                continue;
            if (tag && *tag && std::strcmp(e.tag, tag) != 0)
                continue;
            if (contains && *contains && !str_contains_ci(e.msg, contains) && !str_contains_ci(e.tag, contains))
                continue;
            if (added >= max_entries)
                break;
            if (added)
                out_json += ",";
            out_json += std::string("{\"seq\":") + std::to_string(e.seq) +
                ",\"ts_ns\":" + std::to_string(e.ts_ns) +
                ",\"level\":\"" + emu::McpServer::json_escape(log_level_name(e.level)) + "\"" +
                ",\"tag\":\"" + emu::McpServer::json_escape(e.tag) + "\"" +
                ",\"msg\":\"" + emu::McpServer::json_escape(e.msg) + "\"}";
            ++added;
        }
        out_json += "],\"returned\":" + std::to_string(added) +
            ",\"buffered\":" + std::to_string(count) +
            ",\"newest_seq\":" + std::to_string(newest_seq) + "}";
        return true;
    }

    bool clear_logs(uint32_t& cleared_count, std::string& err) override
    {
        (void)err;
        std::lock_guard<std::mutex> lock(g_mcp_log_sink.mtx);
        cleared_count = g_mcp_log_sink.count;
        g_mcp_log_sink.head = 0;
        g_mcp_log_sink.count = 0;
        return true;
    }

    bool run_until_mem_watch(uint32_t max_steps, uint64_t& event_seq, uint32_t& watch_id,
        uint32_t& hit_pc, uint32_t& hit_phys_addr, uint32_t& hit_value, uint32_t& hit_size,
        uint32_t& steps_done, bool& hit, std::string& err) override
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }
        if (mem_watches_.empty())
        {
            err = "no mem watches set";
            return false;
        }
        event_seq = 0;
        watch_id = 0;
        hit_pc = 0;
        hit_phys_addr = 0;
        hit_value = 0;
        hit_size = 0;
        steps_done = 0;
        hit = false;
        const uint64_t start_seq = next_mem_watch_event_seq_;
        for (uint32_t i = 0; i < max_steps; ++i)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "step stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                steps_done = i;
                return false;
            }
            steps_done = i + 1;
            if (last_mem_watch_event_seq_ >= start_seq)
            {
                const MemWatchEvent& e = last_mem_watch_event_;
                event_seq = e.seq;
                watch_id = e.watch_id;
                hit_pc = e.pc;
                hit_phys_addr = e.phys_addr;
                hit_value = e.value;
                hit_size = e.size;
                hit = true;
                return true;
            }
        }
        return true;
    }

    bool set_psx3d_mode(const char* mode, std::string& err) override
    {
        if (!mode || !*mode)
        {
            err = "missing mode";
            return false;
        }
        if (std::strcmp(mode, "analysis") == 0)
        {
            core_.set_psx3d_mode(emu::Psx3dRunMode::analysis);
            return true;
        }
        if (std::strcmp(mode, "game") == 0)
        {
            core_.set_psx3d_mode(emu::Psx3dRunMode::game);
            return true;
        }
        err = "unknown mode";
        return false;
    }

    bool request_psx3d_refresh(const char* reason, const char* scope, uint32_t& id, std::string& err) override
    {
        if (!reason || !*reason)
        {
            err = "missing reason";
            return false;
        }
        id = core_.request_psx3d_analysis_refresh(reason, (scope && *scope) ? scope : "global");
        return true;
    }

    bool set_gte_trace_window(uint32_t pc_start, uint32_t pc_end, uint32_t start_frame, uint32_t end_frame, bool enabled, std::string& err) override
    {
        auto* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }
        cpu->set_gte_trace(pc_start, pc_end);
        cpu->set_gte_trace_frames(start_frame, end_frame);
        cpu->set_gte_trace_enabled(enabled ? 1 : 0);
        return true;
    }

    bool list_breakpoints(std::string& out_json, std::string& err) const override
    {
        (void)err;
        out_json = "{\"breakpoints\":[";
        for (size_t i = 0; i < breakpoints_.size(); ++i)
        {
            const auto& bp = breakpoints_[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"pc\":") + std::to_string(bp.pc) +
                ",\"enabled\":" + (bp.enabled ? "true" : "false") +
                ",\"hit_count\":" + std::to_string(bp.hit_count) + "}";
        }
        out_json += "]}";
        return true;
    }

    bool set_breakpoint_pc(uint32_t pc, std::string& err) override
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }
        for (auto& bp : breakpoints_)
        {
            if (bp.pc == pc)
            {
                bp.enabled = true;
                return true;
            }
        }
        emu::McpBreakpoint bp{};
        bp.pc = pc;
        bp.enabled = true;
        breakpoints_.push_back(bp);
        return true;
    }

    bool clear_breakpoint_pc(uint32_t pc, bool& removed, std::string& err) override
    {
        (void)err;
        removed = false;
        for (size_t i = 0; i < breakpoints_.size(); ++i)
        {
            if (breakpoints_[i].pc == pc)
            {
                breakpoints_.erase(breakpoints_.begin() + (ptrdiff_t)i);
                removed = true;
                return true;
            }
        }
        return true;
    }

    bool clear_all_breakpoints(uint32_t& removed_count, std::string& err) override
    {
        (void)err;
        removed_count = (uint32_t)breakpoints_.size();
        breakpoints_.clear();
        return true;
    }

    bool run_until_breakpoint(uint32_t max_steps, uint32_t& hit_pc, uint32_t& steps_done, bool& hit, std::string& err) override
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }
        hit_pc = 0;
        steps_done = 0;
        hit = false;
        if (breakpoints_.empty())
        {
            err = "no breakpoints set";
            return false;
        }

        auto* cpu = core_.cpu();
        for (uint32_t i = 0; i < max_steps; ++i)
        {
            const uint32_t pc_before = cpu->pc();
            for (auto& bp : breakpoints_)
            {
                if (bp.enabled && bp.pc == pc_before)
                {
                    bp.hit_count++;
                    hit_pc = pc_before;
                    steps_done = i;
                    hit = true;
                    return true;
                }
            }

            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "step stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                steps_done = i;
                return false;
            }
            steps_done = i + 1;
        }

        const uint32_t pc_after = cpu->pc();
        for (auto& bp : breakpoints_)
        {
            if (bp.enabled && bp.pc == pc_after)
            {
                bp.hit_count++;
                hit_pc = pc_after;
                hit = true;
                return true;
            }
        }

        return true;
    }

private:
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

    static void step_hook_trampoline(uint32_t pc, void* user)
    {
        static_cast<CliMcpBackend*>(user)->on_step_hook(pc);
    }

    static void write_hook_trampoline(uint32_t phys_addr, uint32_t value, uint32_t size, void* user)
    {
        static_cast<CliMcpBackend*>(user)->on_write_hook(phys_addr, value, size);
    }

    void on_step_hook(uint32_t pc)
    {
        r3000::Cpu* cpu = core_.cpu();
        uint8_t* ram = core_.ram();
        if (!cpu)
            return;
        for (auto& h : step_hooks_)
        {
            if (!h.enabled || h.pc != pc)
                continue;
            ++h.hit_count;
            if (h.kind == StepHookKind::write_cop0)
            {
                cpu->set_cop0(h.arg0, h.arg1);
            }
            else if (h.kind == StepHookKind::write_ram_u32 && ram && ((uint64_t)h.arg0 + 4u <= core_.ram_size()))
            {
                const uint32_t addr = h.arg0;
                const uint32_t v = h.arg1;
                ram[addr] = (uint8_t)(v & 0xFFu);
                ram[addr + 1] = (uint8_t)((v >> 8) & 0xFFu);
                ram[addr + 2] = (uint8_t)((v >> 16) & 0xFFu);
                ram[addr + 3] = (uint8_t)((v >> 24) & 0xFFu);
            }
            if (h.once)
                h.enabled = false;
        }
    }

    void on_write_hook(uint32_t phys_addr, uint32_t value, uint32_t size)
    {
        if (mem_watches_.empty())
            return;
        const r3000::Cpu* cpu = core_.cpu();
        const uint32_t pc = cpu ? cpu->pc() : 0;
        const uint64_t write_start = phys_addr;
        const uint64_t write_end = write_start + (size ? (uint64_t)size - 1u : 0u);
        for (auto& w : mem_watches_)
        {
            if (!w.enabled)
                continue;
            if (write_end < w.phys_addr_start || write_start > w.phys_addr_end)
                continue;
            if (w.has_pc && w.pc != pc)
                continue;
            if (w.has_value && w.value != value)
                continue;

            ++w.hit_count;
            MemWatchEvent ev{};
            ev.seq = next_mem_watch_event_seq_++;
            ev.watch_id = w.id;
            ev.pc = pc;
            ev.phys_addr = phys_addr;
            ev.value = value;
            ev.size = size;
            mem_watch_events_[mem_watch_event_head_] = ev;
            last_mem_watch_event_ = ev;
            last_mem_watch_event_seq_ = ev.seq;
            mem_watch_event_head_ = (mem_watch_event_head_ + 1u) % kMaxMemWatchEvents;
            if (mem_watch_event_count_ < kMaxMemWatchEvents)
                ++mem_watch_event_count_;
            if (w.once)
                w.enabled = false;
        }
    }

    emu::Core& core_;
    std::vector<emu::McpBreakpoint> breakpoints_{};
    std::vector<StepHookRule> step_hooks_{};
    struct MemWatchRule
    {
        uint32_t id{0};
        uint32_t phys_addr_start{0};
        uint32_t phys_addr_end{0};
        bool has_pc{false};
        uint32_t pc{0};
        bool has_value{false};
        uint32_t value{0};
        bool once{false};
        bool enabled{true};
        uint64_t hit_count{0};
    };
    struct MemWatchEvent
    {
        uint64_t seq{0};
        uint32_t watch_id{0};
        uint32_t pc{0};
        uint32_t phys_addr{0};
        uint32_t value{0};
        uint32_t size{0};
    };
    static constexpr uint32_t kMaxMemWatchEvents = 256;
    std::vector<MemWatchRule> mem_watches_{};
    std::array<MemWatchEvent, kMaxMemWatchEvents> mem_watch_events_{};
    uint32_t mem_watch_event_head_{0};
    uint32_t mem_watch_event_count_{0};
    uint32_t next_mem_watch_id_{1};
    uint64_t next_mem_watch_event_seq_{1};
    MemWatchEvent last_mem_watch_event_{};
    uint64_t last_mem_watch_event_seq_{0};
    uint32_t next_step_hook_id_{1};
};

// --- Hook: RAM address watch (logs value each VBlank when it changes) ---
struct AddrWatchCtx
{
    r3000::Bus* bus;
    uint32_t    phys_addr;
    uint32_t    last_value;
    std::FILE*  log_file;
};

struct RangeWatchCtx
{
    uint32_t   phys_start;
    uint32_t   phys_end;
    std::FILE* log_file;
};

static void addr_watch_on_vblank(uint32_t vblank_count, void* user)
{
    auto* ctx = static_cast<AddrWatchCtx*>(user);
    const uint8_t* ram = ctx->bus->ram_ptr();
    const uint32_t a = ctx->phys_addr;

    // Read u32 little-endian
    const uint32_t val = (uint32_t)ram[a]
                       | ((uint32_t)ram[a + 1] << 8)
                       | ((uint32_t)ram[a + 2] << 16)
                       | ((uint32_t)ram[a + 3] << 24);

    if (val != ctx->last_value)
    {
        std::fprintf(ctx->log_file, "VBlank #%u: [0x%08X] = 0x%08X (%d)\n",
            vblank_count, ctx->phys_addr, val, (int32_t)val);
        std::fflush(ctx->log_file);
        ctx->last_value = val;
    }
}

static void addr_watch_on_write(uint32_t phys_addr, uint32_t value, uint32_t size, void* user)
{
    auto* ctx = static_cast<AddrWatchCtx*>(user);
    std::fprintf(ctx->log_file, "WRITE [0x%08X] = 0x%08X (size=%u)\n",
        phys_addr, value, size);
    std::fflush(ctx->log_file);
}

static void range_watch_on_write(uint32_t phys_addr, uint32_t value, uint32_t size, void* user)
{
    auto* ctx = static_cast<RangeWatchCtx*>(user);
    const uint32_t last = phys_addr + (size ? (size - 1u) : 0u);
    if (last < ctx->phys_start || phys_addr > ctx->phys_end)
        return;
    std::fprintf(ctx->log_file, "WRITE [0x%08X..0x%08X] = 0x%08X (size=%u)\n",
        phys_addr, last, value, size);
    std::fflush(ctx->log_file);
}

// --- Hook: 3D diagnostic (counts origin_3d vs 2D each VBlank) ---
struct Diag3DCtx
{
    gpu::Gpu3D* gpu3d;
    gpu::Gpu*   gpu_primary; // primary GPU for comparison logging
    gte::Gte3D* gte3d;
    gte::Gte*   gte_primary; // primary GTE for RTPT vertex pattern logging
    std::FILE*  log_file;
    uint32_t    total_3d;
    uint32_t    total_2d;
    uint32_t    total_frames;
    uint32_t    fail_frames; // frames with 0 3D tris
    uint32_t    detail_logged; // number of frames with full detail logged
    uint32_t    last_n3d;     // previous frame's 3D tri count (detect scene changes)
};

// Simulate UE5 R3000Gpu3DComponent::RebuildMesh3D — same face lookup,
// same RT*v+TR transform, same BBox computation.  Outputs to log file.
static void diag_3d_on_vblank(uint32_t vblank_count, void* user)
{
    auto* ctx = static_cast<Diag3DCtx*>(user);
    gpu::FrameDrawList dl;
    ctx->gpu3d->copy_ready_draw_list(dl);

    uint32_t n3d = 0, n2d = 0;
    const size_t count = std::min(dl.cmds.size(), dl.cmds_3d.size());
    for (size_t i = 0; i < count; ++i)
    {
        if (dl.cmds_3d[i].origin == gpu::PrimOrigin::origin_3d)
            ++n3d;
        else
            ++n2d;
    }

    ctx->total_3d += n3d;
    ctx->total_2d += n2d;
    ++ctx->total_frames;
    if (n3d == 0 && count > 0)
        ++ctx->fail_frames;

    // Always log summary for frames with 3D content
    if (n3d > 0)
    {
        std::fprintf(ctx->log_file, "VBlank #%u: %u origin_3d, %u origin_2d (%zu total cmds)\n",
            vblank_count, n3d, n2d, count);
    }

    // Full UE5-equivalent detail: log first 3 frames with 3D content,
    // PLUS first frame after a scene change AND 5 frames later (steady-state).
    const uint32_t prev_n3d = ctx->last_n3d;
    ctx->last_n3d = n3d;
    const bool scene_change = (n3d > 0 && n3d != prev_n3d);
    // Also capture detail 5 frames after scene change (steady-state with populated caches)
    static uint32_t scene_change_vblank = 0;
    if (scene_change) scene_change_vblank = vblank_count;
    const bool steady_state = (scene_change_vblank > 0 && vblank_count == scene_change_vblank + 5);
    // Log primary GTE RTPT patterns every 300 VBlanks (even without 3D hits)
    if (ctx->gte_primary && (vblank_count % 300 == 0) && count > 0)
    {
        const auto& d = ctx->gte_primary->rtpt_diag();
        if (d.rtpt_count > 0)
        {
            const float pct_dup = 100.0f * (d.v1_eq_v2 + d.v0_eq_v2 + d.all_same) / d.rtpt_count;
            std::fprintf(ctx->log_file, "VBlank #%u: PRIMARY GTE RTPT: %u calls  unique=%u  V1==V2=%u  V0==V2=%u  all_same=%u  dup_rate=%.1f%%  cmds=%zu\n",
                vblank_count, d.rtpt_count, d.all_unique, d.v1_eq_v2, d.v0_eq_v2, d.all_same, pct_dup, count);
        }
        ctx->gte_primary->rtpt_diag_reset();
    }

    const bool do_detail = (n3d > 0 && (ctx->detail_logged < 3 || scene_change || steady_state));
    if (!do_detail) { if (n3d > 0) std::fflush(ctx->log_file); return; }
    ++ctx->detail_logged;

    const float WorldScale = 0.1f;
    const float OriginX = 0.5f * static_cast<float>(dl.display.width());
    const float OriginY = 0.5f * static_cast<float>(dl.display.height());

    // BBox accumulators for 3D camera-space verts
    float bbox_min[3] = { 1e9f, 1e9f, 1e9f };
    float bbox_max[3] = { -1e9f, -1e9f, -1e9f };
    int bbox_3d_verts = 0;

    // Degenerate triangle counter
    int degenerate_count = 0;
    int tri3d_idx = 0;

    // Per-face_idx histogram (first 20 unique values)
    uint32_t face_idx_hist[20] = {};
    int face_idx_hist_count = 0;

    // Quad cache hit/miss tracking
    int quad_half0_count = 0, quad_half0_degen = 0;
    int quad_half1_count = 0, quad_half1_degen = 0;
    int quad_cache_hit = 0, quad_cache_miss = 0;

    std::fprintf(ctx->log_file, "=== DETAIL FRAME %u (VBlank #%u) ===\n", dl.frame_id, vblank_count);
    std::fprintf(ctx->log_file, "  Display: %ux%u  DrawEnv: offset=(%d,%d)\n",
        dl.display.width(), dl.display.height(),
        dl.draw_env.offset_x, dl.draw_env.offset_y);

    for (size_t i = 0; i < count; ++i)
    {
        const gpu::DrawCmd& cmd = dl.cmds[i];
        const gpu::DrawCmd3D& cmd3d = dl.cmds_3d[i];
        const bool is_3d = (cmd3d.origin == gpu::PrimOrigin::origin_3d);

        if (!is_3d) continue;

        // Replicate UE5 transform: RT * vertex + TR
        float ue_pos[3][3]; // [vert][xyz]
        bool all_same = true;
        for (int j = 0; j < 3; ++j)
        {
            const auto& v3 = cmd3d.verts_3d[j];
            const auto& T = cmd3d.transform;
            const int64_t vx = v3.vx, vy = v3.vy, vz = v3.vz;
            const float cx = static_cast<float>(((T.rt[0]*vx + T.rt[1]*vy + T.rt[2]*vz) >> 12) + T.tr[0]);
            const float cy = static_cast<float>(((T.rt[3]*vx + T.rt[4]*vy + T.rt[5]*vz) >> 12) + T.tr[1]);
            const float cz = static_cast<float>(((T.rt[6]*vx + T.rt[7]*vy + T.rt[8]*vz) >> 12) + T.tr[2]);

            // GTE → UE5 remap: UE_X=cz*scale, UE_Y=cx*scale, UE_Z=-cy*scale
            ue_pos[j][0] = cz * WorldScale;
            ue_pos[j][1] = cx * WorldScale;
            ue_pos[j][2] = -cy * WorldScale;

            for (int k = 0; k < 3; ++k)
            {
                if (ue_pos[j][k] < bbox_min[k]) bbox_min[k] = ue_pos[j][k];
                if (ue_pos[j][k] > bbox_max[k]) bbox_max[k] = ue_pos[j][k];
            }
            ++bbox_3d_verts;

            if (j > 0 && (v3.vx != cmd3d.verts_3d[0].vx ||
                          v3.vy != cmd3d.verts_3d[0].vy ||
                          v3.vz != cmd3d.verts_3d[0].vz))
                all_same = false;
        }

        // Check degenerate: all 3 vertices at same position
        if (all_same) ++degenerate_count;

        // Quad half tracking
        if (cmd3d.is_quad)
        {
            if (cmd3d.quad_half == 0) { ++quad_half0_count; if (all_same) ++quad_half0_degen; }
            else                      { ++quad_half1_count; if (all_same) ++quad_half1_degen;
                // Check if quad cache has this face_idx
                if (ctx->gte3d && cmd3d.face_idx != 0xFFFFFFFFu)
                {
                    const auto* qc = ctx->gte3d->quad_by_index(cmd3d.face_idx);
                    if (qc) ++quad_cache_hit; else ++quad_cache_miss;
                }
            }
        }

        // Log first 20 3D tris in detail
        if (tri3d_idx < 20)
        {
            std::fprintf(ctx->log_file, "  3D TRI[%d] face_idx=%u quad=%d half=%d",
                tri3d_idx, cmd3d.face_idx, cmd3d.is_quad ? 1 : 0, cmd3d.quad_half);
            if (all_same) std::fprintf(ctx->log_file, " *** DEGENERATE ***");
            std::fprintf(ctx->log_file, "\n");

            for (int j = 0; j < 3; ++j)
            {
                const auto& v3 = cmd3d.verts_3d[j];
                std::fprintf(ctx->log_file,
                    "    v[%d] model=(%d,%d,%d) -> UE(%.1f, %.1f, %.1f)"
                    " screen=(%d,%d)\n",
                    j, v3.vx, v3.vy, v3.vz,
                    ue_pos[j][0], ue_pos[j][1], ue_pos[j][2],
                    cmd.v[j].x, cmd.v[j].y);
            }
            const auto& T = cmd3d.transform;
            std::fprintf(ctx->log_file,
                "    RT=[%d,%d,%d / %d,%d,%d / %d,%d,%d] TR=(%d,%d,%d)\n",
                T.rt[0], T.rt[1], T.rt[2],
                T.rt[3], T.rt[4], T.rt[5],
                T.rt[6], T.rt[7], T.rt[8],
                T.tr[0], T.tr[1], T.tr[2]);
        }

        // Track face_idx histogram
        if (face_idx_hist_count < 20)
        {
            bool found = false;
            for (int f = 0; f < face_idx_hist_count; ++f)
                if (face_idx_hist[f] == cmd3d.face_idx) { found = true; break; }
            if (!found)
                face_idx_hist[face_idx_hist_count++] = cmd3d.face_idx;
        }

        ++tri3d_idx;
    }

    // Summary
    std::fprintf(ctx->log_file, "  --- SUMMARY ---\n");
    std::fprintf(ctx->log_file, "  3D tris: %d  degenerate: %d (%.1f%%)\n",
        tri3d_idx, degenerate_count,
        tri3d_idx > 0 ? 100.0f * degenerate_count / tri3d_idx : 0.0f);
    std::fprintf(ctx->log_file, "  BBox: X=[%.1f..%.1f] Y=[%.1f..%.1f] Z=[%.1f..%.1f] (%d verts)\n",
        bbox_min[0], bbox_max[0], bbox_min[1], bbox_max[1], bbox_min[2], bbox_max[2], bbox_3d_verts);

    // BBox size
    const float bx = bbox_max[0] - bbox_min[0];
    const float by = bbox_max[1] - bbox_min[1];
    const float bz = bbox_max[2] - bbox_min[2];
    std::fprintf(ctx->log_file, "  BBox size: %.1f x %.1f x %.1f",  bx, by, bz);
    if (bx < 0.01f && by < 0.01f && bz < 0.01f)
        std::fprintf(ctx->log_file, " *** ALL VERTICES AT SAME POINT ***");
    else if (bx < 1.0f || by < 1.0f || bz < 1.0f)
        std::fprintf(ctx->log_file, " *** VERY FLAT ***");
    std::fprintf(ctx->log_file, "\n");

    // OT Z distribution (from DMA2 linked-list tracking)
    {
        uint32_t ot_z_min = UINT32_MAX, ot_z_max = 0;
        uint32_t ot_z_unique = 0;
        std::map<uint32_t, uint32_t> ot_z_hist;
        for (size_t i = 0; i < count; ++i)
        {
            const uint32_t z = dl.cmds_3d[i].ot_z;
            ot_z_hist[z]++;
            if (z < ot_z_min) ot_z_min = z;
            if (z > ot_z_max) ot_z_max = z;
        }
        ot_z_unique = static_cast<uint32_t>(ot_z_hist.size());
        std::fprintf(ctx->log_file, "  OT Z: range=[%u..%u] unique=%u levels (total %zu cmds)\n",
            ot_z_min, ot_z_max, ot_z_unique, count);
        // Show first 10 OT Z levels with counts
        int shown = 0;
        std::fprintf(ctx->log_file, "  OT Z levels: ");
        for (auto& [z, cnt] : ot_z_hist)
        {
            if (shown >= 10) { std::fprintf(ctx->log_file, "..."); break; }
            std::fprintf(ctx->log_file, "%sz=%u(%u)", shown > 0 ? " " : "", z, cnt);
            ++shown;
        }
        std::fprintf(ctx->log_file, "\n");
    }

    // Quad cache stats
    std::fprintf(ctx->log_file, "  Quad stats: half0=%d (degen=%d) half1=%d (degen=%d)\n",
        quad_half0_count, quad_half0_degen, quad_half1_count, quad_half1_degen);
    std::fprintf(ctx->log_file, "  Quad cache: hit=%d miss=%d\n",
        quad_cache_hit, quad_cache_miss);

    // Face idx diversity
    std::fprintf(ctx->log_file, "  Unique face_idx (first 20): [");
    for (int f = 0; f < face_idx_hist_count; ++f)
        std::fprintf(ctx->log_file, "%s%u", f > 0 ? ", " : "", face_idx_hist[f]);
    std::fprintf(ctx->log_file, "] (%d unique)\n", face_idx_hist_count);

    // Probe face_cache vs quad_cache for first 5 face_idx values
    if (ctx->gte3d)
    {
        // Copy caches thread-safely for analysis
        std::vector<gte::GteCacheFace> face_snap;
        std::vector<gte::GteCacheQuad> quad_snap;
        ctx->gte3d->copy_ready_face_cache(face_snap);
        ctx->gte3d->copy_ready_quad_cache(quad_snap);

        std::fprintf(ctx->log_file, "  GTE3D face_cache_sz=%zu quad_cache_sz=%zu\n",
            face_snap.size(), quad_snap.size());

        for (int f = 0; f < std::min(face_idx_hist_count, 5); ++f)
        {
            const uint32_t fi = face_idx_hist[f];
            const bool has_face = (fi < face_snap.size());
            const bool has_quad = (fi < quad_snap.size());
            std::fprintf(ctx->log_file, "    face_idx=%u: face=%s quad=%s",
                fi, has_face ? "YES" : "no", has_quad ? "inrange" : "outofrange");
            if (has_face)
                std::fprintf(ctx->log_file, " v0=(%d,%d,%d)", face_snap[fi].vx[0], face_snap[fi].vy[0], face_snap[fi].vz[0]);
            if (has_quad)
            {
                const auto& q = quad_snap[fi];
                // Check if quad is zeroed (never written)
                const bool is_zero = (q.vx[0] == 0 && q.vy[0] == 0 && q.vz[0] == 0 &&
                                      q.vx[3] == 0 && q.vy[3] == 0 && q.vz[3] == 0);
                std::fprintf(ctx->log_file, " qv0=(%d,%d,%d) qv3=(%d,%d,%d)%s",
                    q.vx[0], q.vy[0], q.vz[0], q.vx[3], q.vy[3], q.vz[3],
                    is_zero ? " ZEROED" : " POPULATED");
            }
            std::fprintf(ctx->log_file, "\n");
        }

        // Find first 5 non-zero quad cache entries
        std::fprintf(ctx->log_file, "  First 5 populated quad entries: ");
        int qfound = 0;
        for (size_t qi = 0; qi < quad_snap.size() && qfound < 5; ++qi)
        {
            const auto& q = quad_snap[qi];
            if (q.vx[0] != 0 || q.vy[0] != 0 || q.vz[0] != 0)
            {
                std::fprintf(ctx->log_file, "[%zu]", qi);
                if (qfound < 4) std::fprintf(ctx->log_file, " ");
                ++qfound;
            }
        }
        std::fprintf(ctx->log_file, " (%d found)\n", qfound);
    }

    // ── Primary GPU comparison: log draw list from the REAL GPU ──
    if (ctx->gpu_primary)
    {
        gpu::FrameDrawList pdl;
        ctx->gpu_primary->copy_ready_draw_list(pdl);
        const size_t pcount = pdl.cmds.size();

        // Count tris that have valid 3D snapshots (vertex_count == 3, valid == 1)
        uint32_t p3d = 0;
        int p_v1_eq_v2 = 0;      // V1==V2 pattern
        int p_v0_eq_v2 = 0;      // V0==V2 pattern
        int p_all_same = 0;      // all 3 same
        int p_all_unique = 0;    // all 3 different
        for (size_t i = 0; i < std::min(pcount, pdl.cmds_3d.size()); ++i)
        {
            const auto& c3 = pdl.cmds_3d[i];
            if (c3.origin != gpu::PrimOrigin::origin_3d) continue;
            ++p3d;
            const auto& v0 = c3.verts_3d[0];
            const auto& v1 = c3.verts_3d[1];
            const auto& v2 = c3.verts_3d[2];
            const bool eq01 = (v0.vx==v1.vx && v0.vy==v1.vy && v0.vz==v1.vz);
            const bool eq12 = (v1.vx==v2.vx && v1.vy==v2.vy && v1.vz==v2.vz);
            const bool eq02 = (v0.vx==v2.vx && v0.vy==v2.vy && v0.vz==v2.vz);
            if (eq01 && eq12) ++p_all_same;
            else if (eq12) ++p_v1_eq_v2;
            else if (eq02) ++p_v0_eq_v2;
            else if (eq01) {} // V0==V1 (rare)
            else ++p_all_unique;
        }

        std::fprintf(ctx->log_file, "\n  ── PRIMARY GPU comparison ──\n");
        std::fprintf(ctx->log_file, "  Primary cmds: %zu  origin_3d: %u\n", pcount, p3d);
        std::fprintf(ctx->log_file, "  Vertex patterns: all_unique=%d  V1==V2=%d  V0==V2=%d  all_same=%d\n",
            p_all_unique, p_v1_eq_v2, p_v0_eq_v2, p_all_same);

        // Log first 10 primary 3D tris with coords for comparison
        int logged = 0;
        for (size_t i = 0; i < std::min(pcount, pdl.cmds_3d.size()) && logged < 10; ++i)
        {
            const auto& c3 = pdl.cmds_3d[i];
            if (c3.origin != gpu::PrimOrigin::origin_3d) continue;
            std::fprintf(ctx->log_file, "  P-TRI[%d] v0=(%d,%d,%d) v1=(%d,%d,%d) v2=(%d,%d,%d)"
                " screen=(%d,%d)(%d,%d)(%d,%d)\n",
                logged,
                c3.verts_3d[0].vx, c3.verts_3d[0].vy, c3.verts_3d[0].vz,
                c3.verts_3d[1].vx, c3.verts_3d[1].vy, c3.verts_3d[1].vz,
                c3.verts_3d[2].vx, c3.verts_3d[2].vy, c3.verts_3d[2].vz,
                pdl.cmds[i].v[0].x, pdl.cmds[i].v[0].y,
                pdl.cmds[i].v[1].x, pdl.cmds[i].v[1].y,
                pdl.cmds[i].v[2].x, pdl.cmds[i].v[2].y);
            ++logged;
        }
    }

    // ── Primary GTE RTPT vertex pattern diagnostic ──
    if (ctx->gte_primary)
    {
        const auto& d = ctx->gte_primary->rtpt_diag();
        std::fprintf(ctx->log_file, "\n  ── PRIMARY GTE RTPT patterns (this frame) ──\n");
        std::fprintf(ctx->log_file, "  RTPT calls: %u  all_unique=%u  V1==V2=%u  V0==V2=%u  all_same=%u\n",
            d.rtpt_count, d.all_unique, d.v1_eq_v2, d.v0_eq_v2, d.all_same);
        if (d.rtpt_count > 0)
        {
            const float pct_dup = 100.0f * (d.v1_eq_v2 + d.v0_eq_v2 + d.all_same) / d.rtpt_count;
            std::fprintf(ctx->log_file, "  Duplicate rate: %.1f%% (%u/%u have at least 2 identical verts)\n",
                pct_dup, d.v1_eq_v2 + d.v0_eq_v2 + d.all_same, d.rtpt_count);
        }
        ctx->gte_primary->rtpt_diag_reset();
    }

    std::fprintf(ctx->log_file, "=== END DETAIL ===\n\n");
    std::fflush(ctx->log_file);
}

static void print_usage(void)
{
    std::fprintf(
        stderr,
        "Usage:\n"
        "  r3000_emu [--bios=<bios.bin>] [--cd=<image>] [--gpu-dump=<file>] [--wav-output=<file.wav>]\n"
        "            [--max-steps=N] [--pretty] [--log-level=..] [--log-cats=..] [--emu-log-level=..]\n"
        "            [--psx3d-mode=game|analysis] [--psx3d-analysis=0|1] [--psx3d-refresh=reason:scope]\n"
        "  r3000_emu --load=<file> [--format=auto|elf|psxexe] [--pretty] [--max-steps=N]\n"
        "\n"
        "Options:\n"
        "  --bios=<file>         Load BIOS ROM (default: bios/ps1_bios.bin)\n"
        "  --cd=<image>          Insert CD image (CUE/BIN)\n"
        "  --gpu-dump=<file>     Dump GPU commands to file\n"
        "  --wav-output=<file>   Save SPU audio to WAV file\n"
        "  --max-steps=N         Stop after N instructions\n"
        "  --psx3d-mode=...      Set PSX3D runtime mode (game|analysis)\n"
        "  --psx3d-analysis=0|1  Authorize/disallow PSX3D analysis path\n"
        "  --psx3d-refresh=R:S   Queue analysis refresh request (reason:scope)\n"
        "  --max-time=N          Stop after N seconds wall clock (default: 300)\n"
        "  --load=<file>         Load ELF or PS-X EXE directly (skips BIOS)\n"
        "  --devkit              Real devkit EXE boot (non-HLE, BIOS-backed)\n"
        "  --devkit-hle          Assisted devkit EXE boot (explicit HLE bootstrap)\n"
        "  --pretty              Pretty print instructions\n"
        "  --trace-io            Verbose MMIO logging\n"
        "  --hle                 Enable HLE vectors explicitly\n"
        "  --text-hle            Intercept BIOS/SDK printf+putchar only (keeps non-HLE execution)\n"
        "  --gte-backend=NAME    Primary GTE backend: faithful|modern\n"
        "  --cpu-crash-trace=N   Keep last N CPU states and dump them on fatal faults\n"
        "  --watch-ram-range=A:S Watch RAM writes in physical range A..A+S (hex or dec)\n"
        "  --pc-sample=N         Print PC every N steps\n"
        "  --bus-tick-batch=N    Tick HW every N CPU steps (1=accurate, 32=fast)\n"
        "  --cd-timing=MODE      CD seek/spin-up timing: realistic|compat\n"
        "  --stop-on-pc=ADDR     Stop when PC hits ADDR (hex ok)\n"
        "  --emu-log-level=LVL   Set emu log level (error|warn|info|debug|trace)\n"
        "  --watch-addr=ADDR     Watch RAM address (physical, hex ok) — log changes each VBlank\n"
        "  --watch-range=S:E     Watch RAM write range (physical, hex ok) -> logs/watch_range.log\n"
        "  --watch-writes        Also log every write to watched address (verbose!)\n"
        "  --3d-diag             Log 3D reconstruction stats each VBlank to logs/3d_diag.log\n"
        "  --force-gte-geom-offset-zero  Force GTE OFX/OFY to 0 in RTPS projection\n"
        "                        (per-game quirk, e.g. SCEE Demo One TREX double-buffer)\n"
        "  --psxparam=A,B,...    Pass int params to PSX EXE ($a1 -> scratchpad)\n"
        "                        Example: --psxparam=1,30 (TREX attract, 30s)\n"
        "  --reg-trace=START:END[:WATCH]  Trace registers in PC range, optionally watch for value\n"
        "                        Example: --reg-trace=0x8004AB00:0x8004AC00:0x35096\n"
        "  --gte-trace=START:END Log GTE commands executed in a CPU PC range\n"
        "                        Example: --gte-trace=0x80046000:0x80047000\n"
        "  --gte-trace-start-frame=N  Delay GTE trace until VBlank/frame N\n"
        "  --gte-trace-end-frame=N    Stop GTE trace after VBlank/frame N\n"
    );
}

// CLI callback for emu::Log - writes to stderr
static void cli_log_callback(emu::LogLevel level, const char* tag, const char* msg, void* /*user*/)
{
    static const char* lvl_names[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
    const int idx = (int)level;
    const char* lvl_str = (idx >= 0 && idx <= 4) ? lvl_names[idx] : "???";
    std::fprintf(stderr, "[%s] [%s] %s\n", lvl_str, tag, msg);
    std::fflush(stderr);
}

static int read_file_malloc(const char* path, uint8_t** out_buf, uint32_t* out_size, char* err, size_t err_cap)
{
    if (err && err_cap)
        err[0] = '\0';
    if (!path || !out_buf || !out_size)
        return 0;

    auto fopen_utf8 = [](const char* p, const char* mode) -> std::FILE* {
        if (!p || !mode)
            return nullptr;
#if defined(_WIN32)
        std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
        const std::wstring wpath = conv.from_bytes(p);
        const std::wstring wmode = conv.from_bytes(mode);
        return _wfopen(wpath.c_str(), wmode.c_str());
#else
        return std::fopen(p, mode);
#endif
    };

    std::FILE* f = fopen_utf8(path, "rb");
    if (!f)
    {
        if (err && err_cap)
            std::snprintf(err, err_cap, "could not open '%s'", path);
        return 0;
    }

    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0)
    {
        std::fclose(f);
        if (err && err_cap)
            std::snprintf(err, err_cap, "empty file '%s'", path);
        return 0;
    }

    uint8_t* buf = (uint8_t*)std::malloc((size_t)n);
    if (!buf)
    {
        std::fclose(f);
        if (err && err_cap)
            std::snprintf(err, err_cap, "out of memory");
        return 0;
    }

    const size_t got = std::fread(buf, 1, (size_t)n, f);
    std::fclose(f);
    if (got != (size_t)n)
    {
        std::free(buf);
        if (err && err_cap)
            std::snprintf(err, err_cap, "failed to read '%s'", path);
        return 0;
    }

    *out_buf = buf;
    *out_size = (uint32_t)n;
    return 1;
}

static uint64_t parse_u64_or_zero(const char* s)
{
    if (!s || !*s)
        return 0;
    return std::strtoull(s, nullptr, 0);
}

static void ensure_dir_logs(void)
{
#if defined(_WIN32)
    _mkdir("logs");
#endif
}

static flog::Level parse_flog_level_or(const char* s, flog::Level fallback)
{
    if (!s || !*s)
        return fallback;
    if (std::strcmp(s, "error") == 0)
        return flog::Level::error;
    if (std::strcmp(s, "warn") == 0)
        return flog::Level::warn;
    if (std::strcmp(s, "info") == 0)
        return flog::Level::info;
    if (std::strcmp(s, "debug") == 0)
        return flog::Level::debug;
    if (std::strcmp(s, "trace") == 0)
        return flog::Level::trace;
    return fallback;
}

int main(int argc, char** argv)
{
    const int mcp_stdio = has_flag(argc, argv, "--mcp-stdio");
#if defined(_WIN32)
    if (mcp_stdio)
    {
        _setmode(_fileno(stdin), _O_BINARY);
        _setmode(_fileno(stdout), _O_BINARY);
    }
#endif
    rlog::Logger logger{};
    rlog::logger_init(&logger, mcp_stdio ? stderr : stdout);

    const char* lvl = arg_value(argc, argv, "--log-level=");
    if (lvl)
    {
        rlog::logger_set_level(&logger, rlog::parse_level(lvl));
    }

    const char* cats = arg_value(argc, argv, "--log-cats=");
    if (cats)
    {
        rlog::logger_set_cats(&logger, rlog::parse_categories_csv(cats));
    }

    // Initialize emu::Log — async threaded backend for CLI.
    // UE5 uses its own log_init(). The async backend decouples stderr writes
    // from the emulation thread, eliminating fflush overhead (~10-30ns per log call).
    const char* emu_lvl = arg_value(argc, argv, "--emu-log-level=");
    const emu::LogLevel log_max_level = emu::log_parse_level(emu_lvl);
    emu::async_log_init(log_max_level, 14, &mcp_async_log_consumer, nullptr);

    const char* bios_path = arg_value(argc, argv, "--bios=");
    const char* load_path = arg_value(argc, argv, "--load=");
    const char* cd_path = arg_value(argc, argv, "--cd=");
    const CliDevkitMode devkit_mode = parse_devkit_mode(argc, argv);
    const int is_devkit_mode  = (devkit_mode != CliDevkitMode::none) ? 1 : 0;
    // --devkit      : BIOS boot + virtual disc (real BIOS, no HLE) — new mode
    // --devkit-hle  : skip BIOS, HLE kernel bootstrap — old mode
    const int is_bios_devkit  = (devkit_mode == CliDevkitMode::devkit)     ? 1 : 0;
    const int is_hle_devkit   = (devkit_mode == CliDevkitMode::devkit_hle) ? 1 : 0;
    if (has_flag(argc, argv, "--devkit") && has_flag(argc, argv, "--devkit-hle"))
    {
        emu::logf(emu::LogLevel::warn, "MAIN",
            "Both --devkit and --devkit-hle were passed; using --devkit-hle");
    }
    const char* gpu_dump = arg_value(argc, argv, "--gpu-dump=");
    const char* wav_output = arg_value(argc, argv, "--wav-output=");
    const int trace_io = has_flag(argc, argv, "--trace-io");
    const char* psx3d_mode_s = arg_value(argc, argv, "--psx3d-mode=");
    const char* psx3d_analysis_s = arg_value(argc, argv, "--psx3d-analysis=");
    const char* psx3d_refresh_s = arg_value(argc, argv, "--psx3d-refresh=");

    const char* fmt_s = arg_value(argc, argv, "--format=");
    loader::Format fmt = loader::Format::auto_detect;
    if (fmt_s)
    {
        if (std::strcmp(fmt_s, "auto") == 0)
            fmt = loader::Format::auto_detect;
        else if (std::strcmp(fmt_s, "psxexe") == 0)
            fmt = loader::Format::psxexe;
        else if (std::strcmp(fmt_s, "elf") == 0)
            fmt = loader::Format::elf;
        else
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Unknown --format=%s (use auto|psxexe|elf)", fmt_s);
            return 1;
        }
    }

    const uint64_t max_steps = parse_u64_or_zero(arg_value(argc, argv, "--max-steps="));
    const uint64_t max_time_raw = parse_u64_or_zero(arg_value(argc, argv, "--max-time="));
    const uint64_t max_time_s = (max_time_raw != 0) ? max_time_raw : 300; // default 5 min
    const uint64_t pc_sample = parse_u64_or_zero(arg_value(argc, argv, "--pc-sample="));
    const uint64_t stop_on_pc = parse_u64_or_zero(arg_value(argc, argv, "--stop-on-pc="));
    const uint64_t cpu_crash_trace_raw = parse_u64_or_zero(arg_value(argc, argv, "--cpu-crash-trace="));
    const uint64_t cpu_crash_trace = cpu_crash_trace_raw ? cpu_crash_trace_raw : 512;
    const char* watch_ram_range_s = arg_value(argc, argv, "--watch-ram-range=");
    const char* stack_watch_s = arg_value(argc, argv, "--stack-watch=");
    const char* stack_watch_target_s = arg_value(argc, argv, "--stack-watch-target=");
    const int no_stack_watch = has_flag(argc, argv, "--no-stack-watch") ? 1 : 0;
    const char* cd_timing_s = arg_value(argc, argv, "--cd-timing=");
    const char* bus_tick_batch_s = arg_value(argc, argv, "--bus-tick-batch=");
    uint32_t bus_tick_batch = 0;
    int cd_timing_mode = 1;
    if (cd_timing_s)
    {
        if (std::strcmp(cd_timing_s, "realistic") == 0 || std::strcmp(cd_timing_s, "real") == 0)
            cd_timing_mode = 0;
        else if (std::strcmp(cd_timing_s, "compat") == 0 || std::strcmp(cd_timing_s, "compatibility-fast") == 0)
            cd_timing_mode = 1;
        else
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Unknown --cd-timing=%s (use realistic|compat)", cd_timing_s);
            return 1;
        }
    }
    if (bus_tick_batch_s)
    {
        uint64_t v = parse_u64_or_zero(bus_tick_batch_s);
        if (v < 1u) v = 1u;
        if (v > 128u) v = 128u;
        bus_tick_batch = (uint32_t)v;
    }

    const flog::Level hw_lvl = parse_flog_level_or(arg_value(argc, argv, "--hw-log-level="), flog::Level::info);
    const flog::Level cd_lvl = parse_flog_level_or(arg_value(argc, argv, "--cd-log-level="), hw_lvl);
    const flog::Level gpu_lvl = parse_flog_level_or(arg_value(argc, argv, "--gpu-log-level="), hw_lvl);
    const flog::Level io_lvl = parse_flog_level_or(arg_value(argc, argv, "--io-log-level="), hw_lvl);
    const flog::Level sys_lvl = parse_flog_level_or(arg_value(argc, argv, "--system-log-level="), hw_lvl);

    // Parse --reg-trace=START:END[:WATCH]
    // Example: --reg-trace=0x8004AB00:0x8004AC00:0x35096
    uint32_t reg_trace_start = 0, reg_trace_end = 0, reg_trace_watch = 0;
    const char* reg_trace_s = arg_value(argc, argv, "--reg-trace=");
    if (reg_trace_s)
    {
        // Parse format: START:END or START:END:WATCH
        char* endp = nullptr;
        reg_trace_start = (uint32_t)std::strtoul(reg_trace_s, &endp, 0);
        if (endp && *endp == ':')
        {
            reg_trace_end = (uint32_t)std::strtoul(endp + 1, &endp, 0);
            if (endp && *endp == ':')
            {
                reg_trace_watch = (uint32_t)std::strtoul(endp + 1, &endp, 0);
            }
        }
        emu::logf(emu::LogLevel::info, "MAIN", "Register trace: PC=0x%08X-0x%08X watch=0x%08X",
            reg_trace_start, reg_trace_end, reg_trace_watch);
    }

    // Parse --gte-trace=START:END
    uint32_t gte_trace_start = 0, gte_trace_end = 0;
    uint32_t gte_trace_start_frame = 0, gte_trace_end_frame = 0;
    const char* gte_trace_s = arg_value(argc, argv, "--gte-trace=");
    if (gte_trace_s)
    {
        char* endp = nullptr;
        gte_trace_start = (uint32_t)std::strtoul(gte_trace_s, &endp, 0);
        if (endp && *endp == ':')
            gte_trace_end = (uint32_t)std::strtoul(endp + 1, &endp, 0);
        emu::logf(emu::LogLevel::info, "MAIN", "GTE trace: PC=0x%08X-0x%08X",
            gte_trace_start, gte_trace_end);
    }
    gte_trace_start_frame = (uint32_t)parse_u64_or_zero(arg_value(argc, argv, "--gte-trace-start-frame="));
    gte_trace_end_frame = (uint32_t)parse_u64_or_zero(arg_value(argc, argv, "--gte-trace-end-frame="));
    if (gte_trace_start_frame != 0 || gte_trace_end_frame != 0)
    {
        emu::logf(emu::LogLevel::info, "MAIN", "GTE trace frame gate: start=%u end=%u",
            gte_trace_start_frame, gte_trace_end_frame);
    }

    const uint32_t kRamSize = 2u * 1024u * 1024u;
    emu::Core core(&logger);
    if (psx3d_analysis_s)
    {
        const bool enabled =
            (std::strcmp(psx3d_analysis_s, "1") == 0) ||
            (std::strcmp(psx3d_analysis_s, "true") == 0) ||
            (std::strcmp(psx3d_analysis_s, "on") == 0);
        core.set_psx3d_analysis_enabled(enabled);
    }
    if (psx3d_mode_s)
    {
        if (std::strcmp(psx3d_mode_s, "analysis") == 0)
            core.set_psx3d_mode(emu::Psx3dRunMode::analysis);
        else if (std::strcmp(psx3d_mode_s, "game") == 0)
            core.set_psx3d_mode(emu::Psx3dRunMode::game);
        else
            emu::logf(emu::LogLevel::warn, "MAIN", "Unknown --psx3d-mode=%s (use game|analysis)", psx3d_mode_s);
    }
    if (psx3d_refresh_s && *psx3d_refresh_s)
    {
        const char* colon = std::strchr(psx3d_refresh_s, ':');
        if (colon)
        {
            char reason[64]{};
            char scope[64]{};
            const size_t rn = (size_t)(colon - psx3d_refresh_s);
            const size_t rsz = (rn < sizeof(reason) - 1) ? rn : (sizeof(reason) - 1);
            std::memcpy(reason, psx3d_refresh_s, rsz);
            std::strncpy(scope, colon + 1, sizeof(scope) - 1);
            core.request_psx3d_analysis_refresh(reason, scope);
        }
        else
        {
            core.request_psx3d_analysis_refresh(psx3d_refresh_s, "global");
        }
    }
    {
        char err[256];
        err[0] = '\0';
        if (!core.alloc_ram(kRamSize, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "RAM alloc failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
    }

    // Setup log files
    ensure_dir_logs();
    const flog::Clock clock = flog::clock_start();
    std::FILE* outtext = std::fopen("logs/outtext.log", "wb");
    std::FILE* cdlog_f = std::fopen("logs/cdrom.log", "wb");
    std::FILE* gpulog_f = std::fopen("logs/gpu.log", "wb");
    std::FILE* syslog_f = std::fopen("logs/system.log", "wb");
    std::FILE* iolog_f = std::fopen("logs/io.log", "wb");

    const flog::Sink cdlog{cdlog_f, cd_lvl};
    const flog::Sink gpulog{gpulog_f, gpu_lvl};
    const flog::Sink syslog{syslog_f, sys_lvl};
    const flog::Sink iolog{iolog_f, io_lvl};

    uint8_t* bios = nullptr;
    uint32_t bios_size = 0;

    loader::LoadedImage img{};
    bool boot_bios = false;

    if (!load_path || is_devkit_mode)
    {
        boot_bios = true;
        if (!bios_path)
        {
            // Default: try to find BIOS in bios/ directory
            const char* candidates[] = {
                "bios/ps1_bios.bin",
                "bios/bios.bin",
                "bios/scph1001.bin",
            };
            char err[256];
            bool ok = false;
            for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i)
            {
                bios_path = candidates[i];
                if (read_file_malloc(bios_path, &bios, &bios_size, err, sizeof(err)))
                {
                    emu::logf(emu::LogLevel::info, "MAIN", "BIOS loaded: %s (%u bytes)", bios_path, bios_size);
                    ok = true;
                    break;
                }
            }
            if (!ok)
            {
                emu::logf(emu::LogLevel::error, "MAIN", "No BIOS found. Put a BIOS in 'bios/ps1_bios.bin' or use --bios=...");
                print_usage();
                return 1;
            }
        }
        else
        {
            char err[256];
            if (!read_file_malloc(bios_path, &bios, &bios_size, err, sizeof(err)))
            {
                emu::logf(emu::LogLevel::error, "MAIN", "BIOS load failed: %s", err[0] ? err : "unknown error");
                return 1;
            }
            emu::logf(emu::LogLevel::info, "MAIN", "BIOS loaded: %s (%u bytes)", bios_path, bios_size);
        }
    }
    else
    {
        char err[256];
        err[0] = '\0';
        if (!loader::load_file_into_ram(load_path, fmt, core.ram(), core.ram_size(), &img, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Load failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
        // Devkit mode: derive psx3dprof identity from the EXE path so any
        // per-game quirks (e.g. force_gte_geom_offset_zero for TREX) are
        // auto-applied. Mirrors what Core::fast_boot_from_exe() does for the
        // UE5 path.
        core.load_psx3d_profile_for_exe(load_path);
    }

    if (boot_bios && !is_devkit_mode)
    {
        img.entry_pc = 0xBFC0'0000u;  // BIOS reset vector
        img.has_gp = 0;
        img.has_sp = 1;
        img.sp = 0x801F'FFF0u;
    }

    core.set_log_sinks(cdlog, gpulog, syslog, iolog, clock);

    if (gpu_dump)
    {
        core.set_gpu_dump_file(gpu_dump);
    }

    if (cd_path && !is_devkit_mode)
    {
        char err[256];
        err[0] = '\0';
        if (!core.insert_disc(cd_path, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "CD image load failed: %s", err[0] ? err : "unknown error");
        }
        else
        {
            emu::logf(emu::LogLevel::info, "MAIN", "CD inserted: %s", cd_path);
        }
    }

    // For BIOS devkit, set_bios_copy is called here (BIOS boots for real).
    // For HLE devkit, the BIOS is optional and loaded inside the devkit block.
    if (bios && (!is_devkit_mode || is_bios_devkit))
    {
        char err[256];
        err[0] = '\0';
        if (!core.set_bios_copy(bios, bios_size, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "BIOS setup failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
        std::free(bios);
        bios = nullptr;
    }

    core.set_text_out(outtext);
    core.set_text_io_sink(iolog, clock);

    emu::Core::InitOptions core_opt{};
    core_opt.pretty = has_flag(argc, argv, "--pretty") ? 1 : 0;
    core_opt.trace_io = trace_io ? 1 : 0;
    core_opt.hle_vectors = has_flag(argc, argv, "--hle") ? 1 : 0;
    core_opt.text_hle = has_flag(argc, argv, "--text-hle") ? 1 : 0;
    if (is_hle_devkit)
        core_opt.hle_vectors = 1;  // HLE devkit needs syscall interception
    // is_bios_devkit: real BIOS installs its own vectors — hle_vectors stays 0
    core_opt.crash_trace_steps = static_cast<uint32_t>(cpu_crash_trace);
    if (watch_ram_range_s && *watch_ram_range_s)
    {
        const char* sep = std::strchr(watch_ram_range_s, ':');
        if (sep)
        {
            std::string base_s(watch_ram_range_s, (size_t)(sep - watch_ram_range_s));
            std::string size_s(sep + 1);
            core_opt.watch_ram_range_phys = (uint32_t)parse_u64_or_zero(base_s.c_str());
            core_opt.watch_ram_range_size = (uint32_t)parse_u64_or_zero(size_s.c_str());
        }
    }
    if (no_stack_watch)
    {
        core_opt.stack_watch_enabled = 0;
        core_opt.stack_watch_size = 0;
    }
    else if (stack_watch_s && *stack_watch_s)
    {
        const char* sep = std::strchr(stack_watch_s, ':');
        if (sep)
        {
            std::string base_s(stack_watch_s, (size_t)(sep - stack_watch_s));
            std::string size_s(sep + 1);
            core_opt.stack_watch_phys = (uint32_t)parse_u64_or_zero(base_s.c_str());
            core_opt.stack_watch_size = (uint32_t)parse_u64_or_zero(size_s.c_str());
        }
        else
        {
            core_opt.stack_watch_phys = (uint32_t)parse_u64_or_zero(stack_watch_s);
        }
    }
    if (stack_watch_target_s && *stack_watch_target_s)
    {
        core_opt.stack_watch_target_enabled = 1;
        core_opt.stack_watch_target_value = (uint32_t)parse_u64_or_zero(stack_watch_target_s);
    }
    {
        const char* gte_backend = arg_value(argc, argv, "--gte-backend=");
        if (gte_backend && *gte_backend)
        {
            if (std::strcmp(gte_backend, "modern") == 0)
                core_opt.gte_backend = gte::BackendKind::modern;
            else
                core_opt.gte_backend = gte::BackendKind::faithful;
        }
    }
    core_opt.cd_timing_mode = cd_timing_mode;
    if (bus_tick_batch != 0)
        core_opt.bus_tick_batch = bus_tick_batch;
    if (stop_on_pc != 0)
    {
        core_opt.stop_on_pc_enabled = 1;
        core_opt.stop_on_pc = (uint32_t)stop_on_pc;
    }

    {
        char err[256];
        err[0] = '\0';
        if (is_bios_devkit && load_path)
        {
            // BIOS devkit: boot_bios_with_exe inserts virtual disc + calls
            // init_from_image internally starting at 0xBFC00000. The real
            // BIOS runs fully, finds the disc, and loads BOOT.EXE.
            // BIOS was already set via set_bios_copy above.
            if (!core.boot_bios_with_exe(load_path, cd_path, core_opt, err, sizeof(err)))
            {
                emu::logf(emu::LogLevel::error, "MAIN", "BIOS devkit boot failed: %s", err[0] ? err : "unknown error");
                return 1;
            }
            emu::logf(emu::LogLevel::info, "MAIN",
                "BIOS devkit boot initialised: EXE=%s cd=%s hle=%d",
                load_path, cd_path ? cd_path : "<virtual>", core_opt.hle_vectors);
        }
        else if (is_hle_devkit && load_path)
        {
            // HLE devkit: skip BIOS entirely, fake kernel bootstrap.
            loader::LoadedImage devkit_img{};
            devkit_img.entry_pc = 0x80010000u;
            devkit_img.has_sp = 1;
            devkit_img.sp = 0x801FFFF0u;

            if (!core.init_from_image(devkit_img, core_opt, err, sizeof(err)))
            {
                emu::logf(emu::LogLevel::error, "MAIN", "Core init (devkit-hle) failed: %s", err[0] ? err : "unknown error");
                return 1;
            }

            if (bios)
            {
                if (!core.set_bios_copy(bios, bios_size, err, sizeof(err)))
                {
                    emu::logf(emu::LogLevel::error, "MAIN", "BIOS setup (devkit-hle) failed: %s", err[0] ? err : "unknown error");
                    return 1;
                }
                std::free(bios);
                bios = nullptr;
                emu::logf(emu::LogLevel::info, "MAIN", "Devkit-hle BIOS ROM mapped for SDK services");
            }

            emu::logf(emu::LogLevel::info, "MAIN", "Devkit-hle boot begin: exe=%s hle=%d text_hle=%d gte=%s",
                load_path,
                core_opt.hle_vectors ? 1 : 0,
                core_opt.text_hle ? 1 : 0,
                (core_opt.gte_backend == gte::BackendKind::modern) ? "modern" : "faithful");
        }
        else
        {
            if (!core.init_from_image(img, core_opt, err, sizeof(err)))
            {
                emu::logf(emu::LogLevel::error, "MAIN", "Core init failed: %s", err[0] ? err : "unknown error");
                return 1;
            }
        }
    }

    // BIOS TTY: capture B(3Dh) putchar output from game/PSYQ runtime.
    // Lines are accumulated and flushed with [BIOS_TTY] tag on newline or buffer full.
    {
        struct TtyCtx
        {
            char buf[256];
            int len;
        };
        static TtyCtx tty{};
        tty.len = 0;
        core.set_putchar_callback(
            [](char ch, bool /*bFromPrintf*/, void* user) {
                TtyCtx* ctx = static_cast<TtyCtx*>(user);
                if (ch == '\n' || ch == '\r' || ctx->len >= (int)sizeof(ctx->buf) - 1)
                {
                    ctx->buf[ctx->len] = '\0';
                    if (ctx->len > 0)
                        emu::logf(emu::LogLevel::warn, "BIOS_TTY", "%s", ctx->buf);
                    ctx->len = 0;
                }
                else
                {
                    ctx->buf[ctx->len++] = ch;
                }
            },
            &tty);
    }

    // Enable register trace mode if requested
    if (reg_trace_start != 0 || reg_trace_end != 0)
    {
        if (core.cpu())
        {
            core.cpu()->set_reg_trace(reg_trace_start, reg_trace_end, reg_trace_watch);
            emu::logf(emu::LogLevel::info, "MAIN", "Register trace enabled");
        }
    }
    if (gte_trace_start != 0 || gte_trace_end != 0)
    {
        if (core.cpu())
        {
            core.cpu()->set_gte_trace(gte_trace_start, gte_trace_end);
            core.cpu()->set_gte_trace_frames(gte_trace_start_frame, gte_trace_end_frame);
            emu::logf(emu::LogLevel::info, "MAIN", "GTE trace enabled");
        }
    }

    // Enable WAV audio output if requested
    if (wav_output && core.bus())
    {
        core.bus()->enable_wav_output(wav_output);
        emu::logf(emu::LogLevel::info, "MAIN", "WAV output: %s", wav_output);
    }

    // Optional MDEC frame dump: --mdec-dump=N dumps first N decoded frames as PPM
    {
        const char* mdec_dump_s = arg_value(argc, argv, "--mdec-dump=");
        if (mdec_dump_s && core.bus() && core.bus()->mdec())
        {
            const int max_frames = std::atoi(mdec_dump_s);
            core.bus()->mdec()->enable_frame_dump("logs/mdec_frame", 0, 0, max_frames);
        }
    }

    // Fast boot: skip BIOS, load game EXE directly from CD
    if (has_flag(argc, argv, "--fast-boot") && cd_path)
    {
        char err[256]{};
        if (!core.fast_boot_from_cd(err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Fast boot failed: %s", err[0] ? err : "unknown");
            return 1;
        }
    }

    // --- Hook system: register watches ---
    AddrWatchCtx watch_ctx{};
    std::FILE* watch_log_f = nullptr;
    RangeWatchCtx range_watch_ctx{};
    std::FILE* range_watch_log_f = nullptr;
    const char* watch_addr_s = arg_value(argc, argv, "--watch-addr=");
    if (watch_addr_s && core.bus())
    {
        const uint32_t watch_phys = (uint32_t)std::strtoul(watch_addr_s, nullptr, 0);
        watch_log_f = std::fopen("logs/watch.log", "wb");
        if (watch_log_f)
        {
            watch_ctx.bus = core.bus();
            watch_ctx.phys_addr = watch_phys;
            watch_ctx.last_value = 0xDEADBEEFu;
            watch_ctx.log_file = watch_log_f;

            core.hooks().add_vblank(addr_watch_on_vblank, &watch_ctx);
            emu::logf(emu::LogLevel::info, "HOOK", "VBlank watch on phys 0x%08X -> logs/watch.log", watch_phys);

            if (has_flag(argc, argv, "--watch-writes"))
            {
                core.hooks().add_write(addr_watch_on_write, &watch_ctx, watch_phys);
                emu::logf(emu::LogLevel::info, "HOOK", "Write watch on phys 0x%08X", watch_phys);
            }
        }
    }
    const char* watch_range_s = arg_value(argc, argv, "--watch-range=");
    if (watch_range_s && core.bus())
    {
        char* endp = nullptr;
        const uint32_t start = (uint32_t)std::strtoul(watch_range_s, &endp, 0);
        if (endp && *endp == ':')
        {
            const uint32_t end = (uint32_t)std::strtoul(endp + 1, nullptr, 0);
            range_watch_log_f = std::fopen("logs/watch_range.log", "wb");
            if (range_watch_log_f)
            {
                range_watch_ctx.phys_start = start;
                range_watch_ctx.phys_end = end;
                range_watch_ctx.log_file = range_watch_log_f;
                core.hooks().add_write(range_watch_on_write, &range_watch_ctx, 0);
                emu::logf(emu::LogLevel::info, "HOOK",
                    "Write range watch on phys 0x%08X..0x%08X -> logs/watch_range.log",
                    start, end);
            }
        }
    }

    // --- Game-specific render quirks (manual override) ---
    // See gte::Gte::set_force_geom_offset_zero() for the policy comment.
    // The psx3dprof per-game profile is the canonical place to enable this;
    // this CLI flag is a manual override useful for testing without a profile.
    if (has_flag(argc, argv, "--force-gte-geom-offset-zero"))
    {
        if (auto* gte = core.gte())
        {
            gte->set_force_geom_offset_zero(true);
            emu::logf(emu::LogLevel::warn, "MAIN",
                "Quirk enabled: force_gte_geom_offset_zero (GTE OFX/OFY → 0 in RTPS)");
        }
        if (auto* gte3d = core.gte_3d())
            gte3d->set_force_geom_offset_zero(true);
    }

    // --- PSX EXE params (devkit mode) ---
    {
        const char* psxparam = arg_value(argc, argv, "--psxparam=");
        if (psxparam && *psxparam)
        {
            int32_t params[32];
            int count = 0;
            const char* p = psxparam;
            while (*p && count < 32)
            {
                params[count++] = (int32_t)std::strtol(p, nullptr, 0);
                while (*p && *p != ',') ++p;
                if (*p == ',') ++p;
            }
            if (count > 0)
            {
                core.set_psx_params(params, (uint32_t)count);
                emu::logf(emu::LogLevel::info, "MAIN", "PSX params: %d ints from '%s'", count, psxparam);
            }
        }
    }

    // HLE devkit: set CPU state directly from EXE (skips BIOS).
    // BIOS devkit: boot_bios_with_exe already handled everything.
    if (is_hle_devkit && load_path)
    {
        char err[256]{};
        if (!core.fast_boot_from_exe(load_path, emu::Core::ExeBootMode::devkit_hle, err, sizeof(err)))
        {
            emu::logf(emu::LogLevel::error, "MAIN", "Devkit-hle EXE boot failed: %s", err[0] ? err : "unknown error");
            return 1;
        }
        emu::logf(emu::LogLevel::info, "MAIN", "Devkit-hle boot OK: %s -> PC=0x%08X",
            load_path, core.pc());
    }

    // --- Hook: 3D diagnostic ---
    Diag3DCtx diag3d_ctx{};
    std::FILE* diag3d_log_f = nullptr;
    const int use_3d_diag = has_flag(argc, argv, "--3d-diag");
    if (use_3d_diag && core.gpu_3d())
    {
        diag3d_log_f = std::fopen("logs/3d_diag.log", "wb");
        if (diag3d_log_f)
        {
            diag3d_ctx.gpu3d = core.gpu_3d();
            diag3d_ctx.gpu_primary = core.bus() ? core.bus()->gpu() : nullptr;
            diag3d_ctx.gte3d = core.gte_3d();
            diag3d_ctx.gte_primary = core.cpu() ? &core.cpu()->gte() : nullptr;
            diag3d_ctx.log_file = diag3d_log_f;
            diag3d_ctx.total_3d = 0;
            diag3d_ctx.total_2d = 0;
            diag3d_ctx.total_frames = 0;
            diag3d_ctx.fail_frames = 0;
            diag3d_ctx.detail_logged = 0;
            diag3d_ctx.last_n3d = 0;

            core.hooks().add_vblank(diag_3d_on_vblank, &diag3d_ctx);
            emu::logf(emu::LogLevel::info, "HOOK", "3D diagnostic enabled -> logs/3d_diag.log");
        }
    }

    // --- Auto-input: timed button presses to navigate menus ---
    // PS1 pad active-low: 0xFFFF = all released, bit clear = pressed
    // Bit layout: [Select L3 R3 Start Up Right Down Left | L2 R2 L1 R1 Tri Cir X Sqr]
    static constexpr uint16_t kPadX     = (1u << 14);
    static constexpr uint16_t kPadStart = (1u << 3);
    static constexpr uint16_t kAllUp    = 0xFFFFu;

    struct AutoInput { uint32_t vblank_press; uint32_t vblank_release; uint16_t buttons; };
    // Timeline (at ~60 Hz):
    //   ~30s (1800 vb) = press X to skip intro
    //   ~32s (1920 vb) = press X again (menu confirm)
    //   ~34s (2040 vb) = press X (car select / start race)
    //   ~50s (3000 vb) = press X (accelerate — hold for a few seconds)
    AutoInput auto_inputs[] = {
        { 350,  500,  kPadX },      // Hold X right after SIO0 starts
        { 600,  700,  kPadX },      // X press again
        { 800,  900,  kPadX },      // X press
        { 1000, 1100, kPadX },      // X press
        { 1200, 1300, kPadX },      // X press
        { 1500, 1600, kPadX },      // X press
        { 1800, 1830, kPadX },      // X press
        { 3000, 6000, kPadX },      // Hold X (accelerate!)
    };
    const int auto_input_count = (int)(sizeof(auto_inputs) / sizeof(auto_inputs[0]));
    const int use_auto_input = has_flag(argc, argv, "--auto-input");
    uint32_t last_auto_vblank = 0;

    if (mcp_stdio)
    {
        emu::logf(emu::LogLevel::warn, "MCP", "Starting CLI MCP stdio server");
        CliMcpBackend backend(core);
        emu::McpServer server(backend);
        const int rc = server.run_stdio(stdin, stdout);

        if (diag3d_log_f)
            std::fclose(diag3d_log_f);
        if (watch_log_f)
            std::fclose(watch_log_f);
        if (range_watch_log_f)
            std::fclose(range_watch_log_f);
        if (outtext)
            std::fclose(outtext);
        if (cdlog_f)
            std::fclose(cdlog_f);
        if (gpulog_f)
            std::fclose(gpulog_f);
        if (syslog_f)
            std::fclose(syslog_f);
        if (iolog_f)
            std::fclose(iolog_f);
        return rc;
    }

    emu::logf(emu::LogLevel::info, "MAIN", "Run start PC=0x%08X%s", core.pc(),
        use_auto_input ? " (auto-input enabled)" : "");

    rlog::logger_logf(
        &logger, rlog::Level::info, rlog::Category::exec, "R3000 run start (PC=0x%08X)", core.pc()
    );

    // Start debug TCP server for MCP integration
    debug::DebugServer debug_server;
    debug_server.set_core(&core);
    debug_server.start();

    uint64_t steps = 0;
    const auto run_start = std::chrono::steady_clock::now();
    bool running = true;

    // Same model as UE5: external VBlank + IRQ threads + wall-clock throttle.
    if (core.bus())
        core.bus()->set_external_vblank(true);
    uint32_t periph_accum = 0;
    constexpr uint32_t kPeriphBatch = 1024;
    constexpr double kPS1CpuClock = 33868800.0;
    double cycle_debt = 0.0;
    auto last_time = std::chrono::steady_clock::now();

    while (running)
    {
        // Wall-clock throttle: accumulate cycle debt from real elapsed time
        const auto now = std::chrono::steady_clock::now();
        const double dt = std::chrono::duration<double>(now - last_time).count();
        last_time = now;
        cycle_debt += std::min(dt, 0.05) * kPS1CpuClock;

        // Execute instructions until debt is paid
        while (cycle_debt > 0.0 && running)
        {
            const auto res = core.step();
            const uint32_t cyc = core.last_cycles();
            cycle_debt -= (double)cyc;

            // Auto-input
            if (use_auto_input && core.bus())
            {
                const uint32_t vb = core.bus()->vblank_count();
                if (vb != last_auto_vblank)
                {
                    last_auto_vblank = vb;
                    uint16_t pad = kAllUp;
                    for (int i = 0; i < auto_input_count; ++i)
                    {
                        if (vb >= auto_inputs[i].vblank_press && vb < auto_inputs[i].vblank_release)
                            pad &= ~auto_inputs[i].buttons;
                    }
                    core.set_pad_buttons(pad);
                }
            }

            // Tick peripherals
            periph_accum += cyc;
            if (periph_accum >= kPeriphBatch)
            {
                if (core.bus())
                    core.bus()->tick_peripherals(periph_accum);
                periph_accum = 0;
            }

            if (res.kind == r3000::Cpu::StepResult::Kind::ok)
            {
                if (max_steps != 0 && steps >= max_steps)
                {
                    emu::logf(emu::LogLevel::info, "MAIN", "Stop: reached --max-steps=%" PRIu64, max_steps);
                    running = false;
                }
                if (max_time_s != 0 && (steps & 0xFFFF) == 0)
                {
                    const auto now2 = std::chrono::steady_clock::now();
                    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now2 - run_start).count();
                    if ((uint64_t)elapsed >= max_time_s)
                    {
                        emu::logf(emu::LogLevel::info, "MAIN", "Stop: reached --max-time=%" PRIu64 "s", max_time_s);
                        running = false;
                    }
                }
            }
            else if (res.kind == r3000::Cpu::StepResult::Kind::halted)
            {
                emu::logf(emu::LogLevel::info, "MAIN", "HALT at PC=0x%08X", res.pc);
                running = false;
            }
            else
            {
                emu::logf(emu::LogLevel::error, "MAIN", "Error at PC=0x%08X kind=%d", res.pc, (int)res.kind);
                running = false;
            }
        } // inner while (cycle_debt)

        // Flush remaining peripheral cycles
        if (core.bus() && periph_accum > 0)
        {
            core.bus()->tick_peripherals(periph_accum);
            periph_accum = 0;
        }

        // Wait for IRQ or timeout — like a real CPU in idle/halt state.
        // An IRQ thread (VBlank, timer) can wake us immediately instead
        // of waiting the full 100µs. Zero-latency IRQ delivery.
        if (running && cycle_debt <= 0.0 && core.bus())
            core.bus()->wait_for_irq_or_timeout(std::chrono::microseconds(100));
    }

    // --- 3D diagnostic summary ---
    if (diag3d_log_f)
    {
        std::fprintf(diag3d_log_f, "\n=== SUMMARY ===\n");
        std::fprintf(diag3d_log_f, "Frames: %u (fail: %u)\n", diag3d_ctx.total_frames, diag3d_ctx.fail_frames);
        std::fprintf(diag3d_log_f, "Total origin_3d: %u\n", diag3d_ctx.total_3d);
        std::fprintf(diag3d_log_f, "Total origin_2d: %u\n", diag3d_ctx.total_2d);
        std::fflush(diag3d_log_f);

        emu::logf(emu::LogLevel::warn, "3D_DIAG",
            "SUMMARY: %u frames, %u origin_3d, %u origin_2d, %u fail_frames",
            diag3d_ctx.total_frames, diag3d_ctx.total_3d, diag3d_ctx.total_2d, diag3d_ctx.fail_frames);

        std::fclose(diag3d_log_f);
    }

    if (watch_log_f)
        std::fclose(watch_log_f);
    if (range_watch_log_f)
        std::fclose(range_watch_log_f);
    if (outtext)
        std::fclose(outtext);
    if (cdlog_f)
        std::fclose(cdlog_f);
    if (gpulog_f)
        std::fclose(gpulog_f);
    if (syslog_f)
        std::fclose(syslog_f);
    if (iolog_f)
        std::fclose(iolog_f);

    // Flush and stop the async log thread — must be last so all pending log
    // entries are written before the process exits.
    emu::async_log_shutdown();

    return 0;
}
