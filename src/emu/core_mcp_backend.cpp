// AUTO-EXTRACTED from cli/main.cpp by workbench/extract_core_mcp_backend.py
// Do not edit by hand — re-run the script to regenerate.

#include "emu/core_mcp_backend.h"

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstdio>

#include "emu/core.h"
#include "emu/hooks.h"
#include "gpu/gpu.h"
#include "gpu/gpu_3d.h"
#include "log/async_log.h"
#include "log/emu_log.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"
#include "r3000/cpu_helpers.h"

// UE5's PCH pulls in <Windows.h> which defines `min` and `max` as function-like
// macros. Any later `std::min(...)` / `std::max(...)` call becomes `std::(...)`
// — "illegal token on right side of '::'". Undef AFTER every include so no
// subsequent header can re-define them. The CLI build is unaffected (never
// includes Windows.h).
#ifdef min
#  undef min
#endif
#ifdef max
#  undef max
#endif

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

struct PadBitName
{
    const char* name;
    uint16_t bit;
};

static constexpr PadBitName kPadBitNames[] = {
    {"select", 1u << 0},
    {"l3",     1u << 1},
    {"r3",     1u << 2},
    {"start",  1u << 3},
    {"up",     1u << 4},
    {"right",  1u << 5},
    {"down",   1u << 6},
    {"left",   1u << 7},
    {"l2",     1u << 8},
    {"r2",     1u << 9},
    {"l1",     1u << 10},
    {"r1",     1u << 11},
    {"triangle", 1u << 12},
    {"circle",   1u << 13},
    {"cross",    1u << 14},
    {"square",   1u << 15},
};

static std::string pad_mask_to_names(uint16_t mask)
{
    std::string out;
    bool first = true;
    for (const auto& entry : kPadBitNames)
    {
        if ((mask & entry.bit) != 0)
            continue;
        if (!first)
            out += ",";
        out += entry.name;
        first = false;
    }
    if (first)
        out = "none";
    return out;
}

static bool ci_equal_ascii(const std::string& a, const char* b)
{
    if (!b)
        return false;
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
    {
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i]))
            return false;
    }
    return i == a.size() && b[i] == '\0';
}

static bool pad_name_to_bit(const std::string& raw_name, uint16_t& out_bit)
{
    std::string name;
    name.reserve(raw_name.size());
    for (char c : raw_name)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
            continue;
        name.push_back(c);
    }
    if (name.empty())
        return false;

    for (const auto& entry : kPadBitNames)
    {
        if (ci_equal_ascii(name, entry.name))
        {
            out_bit = entry.bit;
            return true;
        }
    }

    if (ci_equal_ascii(name, "tri"))
    {
        out_bit = uint16_t(1u << 12);
        return true;
    }
    if (ci_equal_ascii(name, "circ"))
    {
        out_bit = uint16_t(1u << 13);
        return true;
    }
    if (ci_equal_ascii(name, "x"))
    {
        out_bit = uint16_t(1u << 14);
        return true;
    }
    if (ci_equal_ascii(name, "sq"))
    {
        out_bit = uint16_t(1u << 15);
        return true;
    }
    return false;
}

static bool parse_pad_names_csv(const char* names_csv, uint16_t& out_mask, std::string& err)
{
    if (!names_csv || !*names_csv)
    {
        err = "missing names";
        return false;
    }

    out_mask = 0;
    std::string unknown{};
    std::string token{};
    for (const char* p = names_csv;; ++p)
    {
        const char c = *p;
        if (c == ',' || c == ';' || c == '|' || c == '\0')
        {
            if (!token.empty())
            {
                uint16_t bit = 0;
                if (pad_name_to_bit(token, bit))
                {
                    out_mask |= bit;
                }
                else
                {
                    if (!unknown.empty())
                        unknown += ",";
                    unknown += token;
                }
                token.clear();
            }
            if (c == '\0')
                break;
        }
        else
        {
            token.push_back(c);
        }
    }

    if (!unknown.empty())
    {
        err = std::string("unknown pad names: ") + unknown;
        return false;
    }
    if (out_mask == 0)
    {
        err = "no valid pad names";
        return false;
    }
    return true;
}

} // namespace
namespace emu {

void core_mcp_log_consumer(uint64_t ts_ns, LogLevel level,
                           const char* tag, const char* msg, void* /*user*/)
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

CoreMcpBackend::CoreMcpBackend(Core& core, McpFrontendKind kind)
    : core_(core), kind_(kind)
{
    step_hook_handle_ = core_.hooks().add_step(&CoreMcpBackend::step_hook_trampoline, this);
    write_hook_handle_ = core_.hooks().add_write(&CoreMcpBackend::write_hook_trampoline, this);
}

CoreMcpBackend::~CoreMcpBackend()
{
    if (write_hook_handle_ >= 0)
        core_.hooks().remove_write(&CoreMcpBackend::write_hook_trampoline, this);
    if (step_hook_handle_ >= 0)
        core_.hooks().remove_step(&CoreMcpBackend::step_hook_trampoline, this);
}

McpFrontendKind CoreMcpBackend::frontend_kind() const
{
    return kind_;
}



    bool CoreMcpBackend::get_status(emu::McpStatus& out) const
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


    bool CoreMcpBackend::get_cpu_state(emu::McpCpuState& out) const
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


    bool CoreMcpBackend::get_run_state(emu::McpRunState& out) const
    {
        out = run_state_;
        if (const r3000::Cpu* cpu = core_.cpu())
            out.last_pc = cpu->pc();
        return true;
    }


    bool CoreMcpBackend::get_boot_exe_info(std::string& out_json, std::string& err) const
    {
        (void)err;
        emu::Core::BootExeInfo info{};
        const bool has_info = core_.get_boot_exe_info(info);
        out_json =
            std::string("{") +
            "\"valid\":" + (has_info ? std::string("true") : std::string("false")) +
            ",\"loaded_to_ram\":" + (info.loaded_to_ram ? std::string("true") : std::string("false")) +
            ",\"reached_entry_pc\":" + (info.reached_entry_pc ? std::string("true") : std::string("false")) +
            ",\"source\":\"" + emu::McpServer::json_escape(info.source) + "\"" +
            ",\"boot_path\":\"" + emu::McpServer::json_escape(info.boot_path) + "\"" +
            ",\"disc_lba\":" + std::to_string(info.disc_lba) +
            ",\"file_size\":" + std::to_string(info.file_size) +
            ",\"entry_pc\":" + std::to_string(info.entry_pc) +
            ",\"gp0\":" + std::to_string(info.gp0) +
            ",\"t_addr\":" + std::to_string(info.t_addr) +
            ",\"t_size\":" + std::to_string(info.t_size) +
            ",\"b_addr\":" + std::to_string(info.b_addr) +
            ",\"b_size\":" + std::to_string(info.b_size) +
            ",\"sp_addr\":" + std::to_string(info.sp_addr) +
            ",\"sp_size\":" + std::to_string(info.sp_size) +
            "}";
        return true;
    }


    bool CoreMcpBackend::get_boot_exe_history(std::string& out_json, std::string& err) const
    {
        (void)err;
        const auto history = core_.boot_exe_history();
        out_json = "{\"count\":" + std::to_string((uint32_t)history.size()) + ",\"events\":[";
        for (size_t i = 0; i < history.size(); ++i)
        {
            const auto& ev = history[i];
            if (i)
                out_json += ",";
            out_json +=
                std::string("{") +
                "\"step_index\":" + std::to_string(ev.step_index) +
                ",\"stage\":\"" + emu::McpServer::json_escape(ev.stage) + "\"" +
                ",\"source\":\"" + emu::McpServer::json_escape(ev.source) + "\"" +
                ",\"boot_path\":\"" + emu::McpServer::json_escape(ev.boot_path) + "\"" +
                ",\"pc\":" + std::to_string(ev.pc) +
                ",\"entry_pc\":" + std::to_string(ev.entry_pc) +
                ",\"loaded_to_ram\":" + (ev.loaded_to_ram ? std::string("true") : std::string("false")) +
                ",\"reached_entry_pc\":" + (ev.reached_entry_pc ? std::string("true") : std::string("false")) +
                "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_runtime_module_history(std::string& out_json, std::string& err) const
    {
        (void)err;
        const auto history = core_.runtime_module_history();
        out_json = "{\"count\":" + std::to_string((uint32_t)history.size()) + ",\"events\":[";
        for (size_t i = 0; i < history.size(); ++i)
        {
            const auto& ev = history[i];
            if (i)
                out_json += ",";
            out_json +=
                std::string("{") +
                "\"step_index\":" + std::to_string(ev.step_index) +
                ",\"kind\":\"" + emu::McpServer::json_escape(ev.kind) + "\"" +
                ",\"label\":\"" + emu::McpServer::json_escape(ev.label) + "\"" +
                ",\"pc\":" + std::to_string(ev.pc) +
                ",\"base\":" + std::to_string(ev.base) +
                ",\"span\":" + std::to_string(ev.span) +
                ",\"reason\":\"" + emu::McpServer::json_escape(ev.reason) + "\"" +
                "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::pause(std::string& err)
    {
        (void)err;
        run_state_.paused = true;
        if (const r3000::Cpu* cpu = core_.cpu())
            run_state_.last_pc = cpu->pc();
        return true;
    }


    bool CoreMcpBackend::resume(uint32_t max_steps, uint32_t max_frames, bool stop_on_breakpoint, std::string& out_json, std::string& err)
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }

        auto* cpu = core_.cpu();
        const auto current_frame_count = [&]() -> uint32_t
        {
            if (core_.gpu_3d())
                return core_.gpu_3d()->frame_count();
            return 0u;
        };
        const uint32_t start_frame = current_frame_count();
        uint32_t steps_done = 0;
        uint32_t frames_done = 0;
        uint32_t hit_pc = 0;
        bool hit_breakpoint = false;
        run_state_.paused = false;

        while (true)
        {
            const uint32_t pc_before = cpu->pc();
            if (stop_on_breakpoint)
            {
                for (auto& bp : breakpoints_)
                {
                    if (bp.enabled && bp.pc == pc_before)
                    {
                        bp.hit_count++;
                        hit_pc = pc_before;
                        hit_breakpoint = true;
                        goto resume_done;
                    }
                }
            }

            if (max_steps != 0 && steps_done >= max_steps)
                break;

            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "step stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                run_state_.paused = true;
                run_state_.last_pc = res.pc;
                run_state_.last_steps = steps_done;
                run_state_.last_frames = current_frame_count() - start_frame;
                run_state_.last_hit_breakpoint = false;
                run_state_.last_hit_pc = 0;
                return false;
            }

            ++steps_done;
            frames_done = current_frame_count() - start_frame;

            if (stop_on_breakpoint)
            {
                const uint32_t pc_after = cpu->pc();
                for (auto& bp : breakpoints_)
                {
                    if (bp.enabled && bp.pc == pc_after)
                    {
                        bp.hit_count++;
                        hit_pc = pc_after;
                        hit_breakpoint = true;
                        goto resume_done;
                    }
                }
            }

            if (max_frames != 0 && frames_done >= max_frames)
                break;
            if (max_steps == 0 && max_frames == 0)
                break;
        }

resume_done:
        run_state_.paused = true;
        run_state_.last_pc = cpu->pc();
        run_state_.last_steps = steps_done;
        run_state_.last_frames = frames_done;
        run_state_.last_hit_breakpoint = hit_breakpoint;
        run_state_.last_hit_pc = hit_pc;

        out_json =
            std::string("{") +
            "\"paused\":true" +
            ",\"steps_done\":" + std::to_string(steps_done) +
            ",\"frames_done\":" + std::to_string(frames_done) +
            ",\"hit_breakpoint\":" + (hit_breakpoint ? std::string("true") : std::string("false")) +
            ",\"hit_pc\":" + std::to_string(hit_pc) +
            ",\"pc\":" + std::to_string(cpu->pc()) +
            "}";
        return true;
    }


    bool CoreMcpBackend::resume_until_boot_exe(uint32_t max_steps, std::string& out_json, std::string& err)
    {
        if (!core_.cpu())
        {
            err = "cpu not initialized";
            return false;
        }

        emu::Core::BootExeInfo info{};
        if (!core_.get_boot_exe_info(info) || !info.valid)
        {
            err = "boot exe info unavailable";
            return false;
        }

        auto* cpu = core_.cpu();
        uint32_t steps_done = 0;
        bool hit = false;
        uint32_t hit_pc = 0;
        run_state_.paused = false;

        while (steps_done < max_steps || max_steps == 0)
        {
            emu::Core::BootExeInfo cur{};
            core_.get_boot_exe_info(cur);
            if ((cur.valid && cur.reached_entry_pc) || cpu->pc() == info.entry_pc)
            {
                hit = true;
                hit_pc = info.entry_pc;
                break;
            }

            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[160];
                std::snprintf(msg, sizeof(msg), "step stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                run_state_.paused = true;
                run_state_.last_pc = res.pc;
                run_state_.last_steps = steps_done;
                run_state_.last_frames = 0;
                run_state_.last_hit_breakpoint = false;
                run_state_.last_hit_pc = 0;
                return false;
            }
            ++steps_done;
        }

        run_state_.paused = true;
        run_state_.last_pc = cpu->pc();
        run_state_.last_steps = steps_done;
        run_state_.last_frames = 0;
        run_state_.last_hit_breakpoint = hit;
        run_state_.last_hit_pc = hit_pc;

        out_json =
            std::string("{") +
            "\"paused\":true" +
            ",\"hit\":" + (hit ? std::string("true") : std::string("false")) +
            ",\"steps_done\":" + std::to_string(steps_done) +
            ",\"entry_pc\":" + std::to_string(info.entry_pc) +
            ",\"pc\":" + std::to_string(cpu->pc()) +
            ",\"boot_path\":\"" + emu::McpServer::json_escape(info.boot_path) + "\"" +
            "}";
        return true;
    }


    bool CoreMcpBackend::step(uint32_t count, std::string& err)
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
        run_state_.paused = true;
        if (const r3000::Cpu* cpu = core_.cpu())
            run_state_.last_pc = cpu->pc();
        run_state_.last_steps = count;
        run_state_.last_frames = 0;
        run_state_.last_hit_breakpoint = false;
        run_state_.last_hit_pc = 0;
        return true;
    }


    bool CoreMcpBackend::read_ram_u32(uint32_t phys_addr, uint32_t& out, std::string& err) const
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


    bool CoreMcpBackend::read_cop0(uint32_t reg, uint32_t& out, std::string& err) const
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


    bool CoreMcpBackend::write_cop0(uint32_t reg, uint32_t value, std::string& err)
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


    bool CoreMcpBackend::add_step_hook_write_cop0(uint32_t pc, uint32_t reg, uint32_t value, bool once, uint32_t& hook_id, std::string& err)
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


    bool CoreMcpBackend::add_step_hook_write_ram_u32(uint32_t pc, uint32_t phys_addr, uint32_t value, bool once, uint32_t& hook_id, std::string& err)
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


    bool CoreMcpBackend::list_step_hooks(std::string& out_json, std::string& err) const
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


    bool CoreMcpBackend::clear_step_hook(uint32_t hook_id, bool& removed, std::string& err)
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


    bool CoreMcpBackend::clear_all_step_hooks(uint32_t& removed_count, std::string& err)
    {
        (void)err;
        removed_count = (uint32_t)step_hooks_.size();
        step_hooks_.clear();
        return true;
    }


    bool CoreMcpBackend::add_mem_watch_write(uint32_t phys_addr_start, uint32_t phys_addr_end, bool has_pc, uint32_t pc,
        bool has_value, uint32_t value, bool once, uint32_t& watch_id, std::string& err)
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


    bool CoreMcpBackend::list_mem_watches(std::string& out_json, std::string& err) const
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


    bool CoreMcpBackend::clear_mem_watch(uint32_t watch_id, bool& removed, std::string& err)
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


    bool CoreMcpBackend::clear_all_mem_watches(uint32_t& removed_count, std::string& err)
    {
        (void)err;
        removed_count = (uint32_t)mem_watches_.size();
        mem_watches_.clear();
        return true;
    }


    bool CoreMcpBackend::list_mem_watch_events(std::string& out_json, std::string& err) const
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


    bool CoreMcpBackend::clear_mem_watch_events(uint32_t& cleared_count, std::string& err)
    {
        (void)err;
        cleared_count = mem_watch_event_count_;
        mem_watch_event_count_ = 0;
        mem_watch_event_head_ = 0;
        return true;
    }


    bool CoreMcpBackend::list_logs(uint64_t since_seq, bool has_min_level, uint32_t min_level,
        const char* tag, const char* contains, uint32_t max_entries, std::string& out_json, std::string& err) const
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


    bool CoreMcpBackend::clear_logs(uint32_t& cleared_count, std::string& err)
    {
        (void)err;
        std::lock_guard<std::mutex> lock(g_mcp_log_sink.mtx);
        cleared_count = g_mcp_log_sink.count;
        g_mcp_log_sink.head = 0;
        g_mcp_log_sink.count = 0;
        return true;
    }


    bool CoreMcpBackend::get_gte_trace_summary(std::string& out_json, std::string& err) const
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        const auto cfg = cpu->gte_trace_config();
        auto pc_hist = cpu->gte_trace_pc_hist_snapshot();
        auto op_hist = cpu->gte_trace_op_hist_snapshot();
        auto by_count_desc = [](const auto& a, const auto& b) {
            if (a.second != b.second)
                return a.second > b.second;
            return a.first < b.first;
        };
        std::sort(pc_hist.begin(), pc_hist.end(), by_count_desc);
        std::sort(op_hist.begin(), op_hist.end(), by_count_desc);

        out_json =
            std::string("{") +
            "\"enabled\":" + (cfg.enabled ? "true" : "false") +
            ",\"pc_start\":" + std::to_string(cfg.pc_start) +
            ",\"pc_end\":" + std::to_string(cfg.pc_end) +
            ",\"start_frame\":" + std::to_string(cfg.start_frame) +
            ",\"end_frame\":" + std::to_string(cfg.end_frame) +
            ",\"summary_dumped\":" + (cfg.summary_dumped ? "true" : "false") +
            ",\"top_pcs\":[";
        const size_t max_pc = (pc_hist.size() < 8u) ? pc_hist.size() : 8u;
        for (size_t i = 0; i < max_pc; ++i)
        {
            if (i)
                out_json += ",";
            out_json += std::string("{\"pc\":") + std::to_string(pc_hist[i].first) +
                ",\"count\":" + std::to_string(pc_hist[i].second) + "}";
        }
        out_json += "],\"top_ops\":[";
        const size_t max_op = (op_hist.size() < 8u) ? op_hist.size() : 8u;
        for (size_t i = 0; i < max_op; ++i)
        {
            if (i)
                out_json += ",";
            out_json += std::string("{\"op\":") + std::to_string(op_hist[i].first) +
                ",\"name\":\"" + emu::McpServer::json_escape(r3000::gte_func_name(op_hist[i].first)) + "\"" +
                ",\"count\":" + std::to_string(op_hist[i].second) + "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_dma2_nohint_summary(std::string& out_json, std::string& err) const
    {
        const r3000::Bus* bus = core_.bus();
        if (!bus)
        {
            err = "bus not initialized";
            return false;
        }

        r3000::Bus::Dma2NoHintSummary s{};
        const bool has_last = bus->peek_dma2_nohint_summary(s);
        const auto hotspots = core_.provenance_hotspots_snapshot();
        auto hotspots_sorted = hotspots;
        std::sort(hotspots_sorted.begin(), hotspots_sorted.end(),
            [](const auto& a, const auto& b) {
                if (a.total_words != b.total_words)
                    return a.total_words > b.total_words;
                return a.pc < b.pc;
            });

        out_json =
            std::string("{") +
            "\"has_last\":" + (has_last ? "true" : "false");
        if (has_last)
        {
            out_json +=
                ",\"last\":{\"vblank\":" + std::to_string(s.vblank) +
                ",\"nohint_words\":" + std::to_string(s.nohint_words) +
                ",\"top_pcs\":[";
            for (size_t i = 0; i < s.top_pcs.size(); ++i)
            {
                if (i)
                    out_json += ",";
                out_json += std::string("{\"pc\":") + std::to_string(s.top_pcs[i].first) +
                    ",\"count\":" + std::to_string(s.top_pcs[i].second) + "}";
            }
            out_json += "]}";
        }
        out_json += ",\"hotspots\":[";
        const size_t max_hotspots = (hotspots_sorted.size() < 12u) ? hotspots_sorted.size() : 12u;
        for (size_t i = 0; i < max_hotspots; ++i)
        {
            const auto& h = hotspots_sorted[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"pc\":") + std::to_string(h.pc) +
                ",\"total_words\":" + std::to_string(h.total_words) +
                ",\"last_seen_vblank\":" + std::to_string(h.last_seen_vblank) +
                ",\"last_refresh_vblank\":" + std::to_string(h.last_refresh_vblank) + "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_draw_list_summary(std::string& out_json, std::string& err) const
    {
        gpu::Gpu3D* gpu3d = core_.gpu_3d();
        if (!gpu3d)
        {
            err = "gpu3d not initialized";
            return false;
        }

        gpu::FrameDrawList dl;
        gpu3d->copy_ready_draw_list(dl);

        uint32_t n3d = 0;
        uint32_t n2d_hud = 0;
        uint32_t n2d_rect = 0;
        uint32_t n2d_line = 0;
        uint32_t textured = 0;
        uint32_t semi = 0;
        uint32_t raw = 0;
        uint32_t quads = 0;
        uint32_t linked_faces = 0;
        uint32_t max_ot_z = 0;
        std::unordered_map<uint32_t, uint32_t> face_hist{};

        const size_t count = std::min(dl.cmds.size(), dl.cmds_3d.size());
        for (size_t i = 0; i < count; ++i)
        {
            const auto& cmd = dl.cmds[i];
            const auto& cmd3d = dl.cmds_3d[i];
            switch (cmd3d.origin)
            {
            case gpu::PrimOrigin::origin_3d: ++n3d; break;
            case gpu::PrimOrigin::origin_2d_hud: ++n2d_hud; break;
            case gpu::PrimOrigin::origin_2d_rect: ++n2d_rect; break;
            case gpu::PrimOrigin::origin_2d_line: ++n2d_line; break;
            }
            if (cmd.flags & 0x1u) ++textured;
            if (cmd.flags & 0x2u) ++semi;
            if (cmd.flags & 0x4u) ++raw;
            if (cmd3d.is_quad) ++quads;
            if (cmd3d.ot_z > max_ot_z) max_ot_z = cmd3d.ot_z;
            if (cmd3d.face_idx != 0xFFFFFFFFu)
            {
                ++linked_faces;
                ++face_hist[cmd3d.face_idx];
            }
        }

        std::vector<std::pair<uint32_t, uint32_t>> top_faces(face_hist.begin(), face_hist.end());
        std::sort(top_faces.begin(), top_faces.end(),
            [](const auto& a, const auto& b) {
                if (a.second != b.second)
                    return a.second > b.second;
                return a.first < b.first;
            });

        out_json =
            std::string("{") +
            "\"frame_id\":" + std::to_string(dl.frame_id) +
            ",\"total_cmds\":" + std::to_string((uint32_t)dl.cmds.size()) +
            ",\"total_cmds_3d\":" + std::to_string((uint32_t)dl.cmds_3d.size()) +
            ",\"origin_counts\":{\"origin_3d\":" + std::to_string(n3d) +
            ",\"origin_2d_hud\":" + std::to_string(n2d_hud) +
            ",\"origin_2d_rect\":" + std::to_string(n2d_rect) +
            ",\"origin_2d_line\":" + std::to_string(n2d_line) + "}" +
            ",\"packet_flags\":{\"textured\":" + std::to_string(textured) +
            ",\"semi_transparent\":" + std::to_string(semi) +
            ",\"raw_texture\":" + std::to_string(raw) +
            ",\"quad_halves\":" + std::to_string(quads) + "}" +
            ",\"linked_faces\":{\"linked_triangles\":" + std::to_string(linked_faces) +
            ",\"unique_face_indices\":" + std::to_string((uint32_t)face_hist.size()) + "}" +
            ",\"display\":{\"x\":" + std::to_string(dl.display.display_x) +
            ",\"y\":" + std::to_string(dl.display.display_y) +
            ",\"width\":" + std::to_string(dl.display.width()) +
            ",\"height\":" + std::to_string(dl.display.height()) +
            ",\"pal\":" + (dl.display.is_pal ? "true" : "false") + "}" +
            ",\"draw_env\":{\"offset_x\":" + std::to_string(dl.draw_env.offset_x) +
            ",\"offset_y\":" + std::to_string(dl.draw_env.offset_y) +
            ",\"clip_x1\":" + std::to_string(dl.draw_env.clip_x1) +
            ",\"clip_y1\":" + std::to_string(dl.draw_env.clip_y1) +
            ",\"clip_x2\":" + std::to_string(dl.draw_env.clip_x2) +
            ",\"clip_y2\":" + std::to_string(dl.draw_env.clip_y2) + "}" +
            ",\"max_ot_z\":" + std::to_string(max_ot_z) +
            ",\"top_face_indices\":[";
        const size_t max_top = (top_faces.size() < 8u) ? top_faces.size() : 8u;
        for (size_t i = 0; i < max_top; ++i)
        {
            if (i)
                out_json += ",";
            out_json += std::string("{\"face_idx\":") + std::to_string(top_faces[i].first) +
                ",\"count\":" + std::to_string(top_faces[i].second) + "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_camera_candidates(std::string& out_json, std::string& err) const
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        auto cams = cpu->camera_candidates_snapshot();
        const uint32_t ram_size = core_.ram_size();
        std::sort(cams.begin(), cams.end(),
            [ram_size](const auto& a, const auto& b) {
                const int a_main = (a.addr < ram_size) ? 1 : 0;
                const int b_main = (b.addr < ram_size) ? 1 : 0;
                if (a_main != b_main)
                    return a_main > b_main;
                if (a.frame_hits != b.frame_hits)
                    return a.frame_hits > b.frame_hits;
                if (a.hits != b.hits)
                    return a.hits > b.hits;
                return a.addr < b.addr;
            });

        out_json =
            std::string("{") +
            "\"serial\":" + std::to_string(cpu->camera_candidates_serial()) +
            ",\"candidates\":[";
        for (size_t i = 0; i < cams.size(); ++i)
        {
            const auto& c = cams[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"addr\":") + std::to_string(c.addr) +
                ",\"region\":\"" + ((c.addr < ram_size) ? std::string("ram") : std::string("scratch")) + "\"" +
                ",\"hits\":" + std::to_string(c.hits) +
                ",\"frame_hits\":" + std::to_string(c.frame_hits) +
                ",\"first_vblank\":" + std::to_string(c.first_vblank) +
                ",\"last_vblank\":" + std::to_string(c.last_vblank) +
                ",\"last_pc\":" + std::to_string(c.last_pc) +
                ",\"gte_reg_mask\":" + std::to_string(c.gte_reg_mask) + "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_linked_poly_groups(std::string& out_json, std::string& err) const
    {
        gpu::FrameDrawList dl;
        std::vector<GroupObs> sorted{};
        if (!build_sorted_group_obs(dl, sorted, err))
            return false;

        out_json =
            std::string("{") +
            "\"frame_id\":" + std::to_string(dl.frame_id) +
            ",\"group_count\":" + std::to_string((uint32_t)sorted.size()) +
            ",\"groups\":[";
        const size_t max_groups = (sorted.size() < 32u) ? sorted.size() : 32u;
        for (size_t i = 0; i < max_groups; ++i)
        {
            const GroupObs& g = sorted[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"source_pc\":") + std::to_string(g.source_pc) +
                ",\"face_idx\":" + std::to_string(g.face_idx) +
                ",\"tri_count\":" + std::to_string(g.tri_count) +
                ",\"quad_halves\":" + std::to_string(g.quad_halves) +
                ",\"ot_z_min\":" + std::to_string(g.min_ot_z == 0xFFFFFFFFu ? 0u : g.min_ot_z) +
                ",\"ot_z_max\":" + std::to_string(g.max_ot_z) +
                ",\"textured\":" + std::to_string(g.textured) +
                ",\"semi_transparent\":" + std::to_string(g.semi) +
                ",\"raw_texture\":" + std::to_string(g.raw) +
                ",\"seen_frames\":" + std::to_string(g.seen_frames) +
                ",\"first_frame\":" + std::to_string(g.first_frame) +
                ",\"last_frame\":" + std::to_string(g.last_frame) +
                ",\"stable_score\":" + std::to_string(g.stable_score) +
                ",\"bbox\":{\"min_x\":" + std::to_string(g.min_x) +
                ",\"min_y\":" + std::to_string(g.min_y) +
                ",\"max_x\":" + std::to_string(g.max_x) +
                ",\"max_y\":" + std::to_string(g.max_y) + "}}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_transform_roots(std::string& out_json, std::string& err) const
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        auto cams = cpu->camera_candidates_snapshot();
        const uint32_t ram_size = core_.ram_size();
        std::sort(cams.begin(), cams.end(),
            [ram_size](const auto& a, const auto& b) {
                const int a_main = (a.addr < ram_size) ? 1 : 0;
                const int b_main = (b.addr < ram_size) ? 1 : 0;
                if (a_main != b_main)
                    return a_main > b_main;
                if (a.frame_hits != b.frame_hits)
                    return a.frame_hits > b.frame_hits;
                if (a.hits != b.hits)
                    return a.hits > b.hits;
                return a.addr < b.addr;
            });

        out_json =
            std::string("{") +
            "\"serial\":" + std::to_string(cpu->camera_candidates_serial()) +
            ",\"roots\":[";
        const size_t max_roots = (cams.size() < 32u) ? cams.size() : 32u;
        for (size_t i = 0; i < max_roots; ++i)
        {
            const auto& c = cams[i];
            const bool main_ram = c.addr < ram_size;
            const uint32_t score = (main_ram ? 1000u : 0u) + c.frame_hits * 10u + c.hits;
            if (i)
                out_json += ",";
            out_json += std::string("{\"addr\":") + std::to_string(c.addr) +
                ",\"region\":\"" + (main_ram ? std::string("ram") : std::string("scratch")) + "\"" +
                ",\"score\":" + std::to_string(score) +
                ",\"hits\":" + std::to_string(c.hits) +
                ",\"frame_hits\":" + std::to_string(c.frame_hits) +
                ",\"last_pc\":" + std::to_string(c.last_pc) +
                ",\"gte_reg_mask\":" + std::to_string(c.gte_reg_mask) +
                ",\"first_vblank\":" + std::to_string(c.first_vblank) +
                ",\"last_vblank\":" + std::to_string(c.last_vblank) + "}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_group_transform_links(std::string& out_json, std::string& err) const
    {
        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        gpu::FrameDrawList dl;
        std::vector<GroupObs> sorted_groups{};
        if (!build_sorted_group_obs(dl, sorted_groups, err))
            return false;
        auto cams = cpu->camera_candidates_snapshot();
        const uint32_t ram_size = core_.ram_size();
        std::sort(cams.begin(), cams.end(),
            [ram_size](const auto& a, const auto& b) {
                const int a_main = (a.addr < ram_size) ? 1 : 0;
                const int b_main = (b.addr < ram_size) ? 1 : 0;
                if (a_main != b_main)
                    return a_main > b_main;
                if (a.frame_hits != b.frame_hits)
                    return a.frame_hits > b.frame_hits;
                if (a.hits != b.hits)
                    return a.hits > b.hits;
                return a.addr < b.addr;
            });

        out_json =
            std::string("{") +
            "\"frame_id\":" + std::to_string(dl.frame_id) +
            ",\"link_kind\":\"heuristic\"" +
            ",\"links\":[";
        const size_t max_links = std::min<size_t>(std::min<size_t>(sorted_groups.size(), 16u), cams.size());
        for (size_t i = 0; i < max_links; ++i)
        {
            const auto& g = sorted_groups[i];
            const auto& c = cams[i];
            const uint32_t root_score = ((c.addr < ram_size) ? 1000u : 0u) + c.frame_hits * 10u + c.hits;
            uint32_t confidence = std::min<uint32_t>(95u, 20u + std::min<uint32_t>(g.tri_count * 5u, 40u) + std::min<uint32_t>(root_score / 100u, 20u) + std::min<uint32_t>(g.stable_score / 4u, 15u));
            if (i)
                out_json += ",";
            out_json += std::string("{\"source_pc\":") + std::to_string(g.source_pc) +
                ",\"face_idx\":" + std::to_string(g.face_idx) +
                ",\"tri_count\":" + std::to_string(g.tri_count) +
                ",\"seen_frames\":" + std::to_string(g.seen_frames) +
                ",\"root_addr\":" + std::to_string(c.addr) +
                ",\"root_region\":\"" + ((c.addr < ram_size) ? std::string("ram") : std::string("scratch")) + "\"" +
                ",\"root_last_pc\":" + std::to_string(c.last_pc) +
                ",\"confidence\":" + std::to_string(confidence) +
                ",\"note\":\"ranked heuristic only; no direct face->matrix provenance yet\"}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_mesh_cache_candidates(std::string& out_json, std::string& err) const
    {
        gpu::FrameDrawList dl;
        std::vector<GroupObs> sorted{};
        if (!build_sorted_group_obs(dl, sorted, err))
            return false;

        std::vector<MeshCacheCandidate> candidates;
        candidates.reserve(mesh_cache_candidates_.size());
        for (const auto& kv : mesh_cache_candidates_)
            candidates.push_back(kv.second);
        std::sort(candidates.begin(), candidates.end(),
            [](const MeshCacheCandidate& a, const MeshCacheCandidate& b) {
                if (a.promotion_score != b.promotion_score)
                    return a.promotion_score > b.promotion_score;
                if (a.seen_frames != b.seen_frames)
                    return a.seen_frames > b.seen_frames;
                if (a.source_pc != b.source_pc)
                    return a.source_pc < b.source_pc;
                return a.face_idx < b.face_idx;
            });

        out_json =
            std::string("{") +
            "\"current_frame\":" + std::to_string(dl.frame_id) +
            ",\"candidate_count\":" + std::to_string((uint32_t)candidates.size()) +
            ",\"candidates\":[";
        const size_t max_candidates = std::min<size_t>(candidates.size(), 24u);
        for (size_t i = 0; i < max_candidates; ++i)
        {
            const auto& c = candidates[i];
            if (i)
                out_json += ",";
            out_json += std::string("{\"source_pc\":") + std::to_string(c.source_pc) +
                ",\"face_idx\":" + std::to_string(c.face_idx) +
                ",\"seen_frames\":" + std::to_string(c.seen_frames) +
                ",\"first_frame\":" + std::to_string(c.first_frame) +
                ",\"last_frame\":" + std::to_string(c.last_frame) +
                ",\"last_tri_count\":" + std::to_string(c.last_tri_count) +
                ",\"stable_ot_z\":" + (c.stable_ot_z ? std::string("true") : std::string("false")) +
                ",\"material_signature\":\"" + emu::McpServer::json_escape(c.material_signature) + "\"" +
                ",\"promotion_score\":" + std::to_string(c.promotion_score) +
                ",\"status\":\"" + emu::McpServer::json_escape(c.status) + "\"}";
        }
        out_json += "]}";
        return true;
    }


    bool CoreMcpBackend::get_pad_state(uint32_t slot, std::string& out_json, std::string& err) const
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }
        const uint16_t mask = core_.pad_buttons_for_slot(slot);
        const uint16_t local_mask = core_.pad_local_buttons_for_slot(slot);
        const uint16_t mcp_mask = core_.pad_mcp_buttons_for_slot(slot);
        out_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"slot_count\":" + std::to_string(r3000::Bus::kPadSlotCount) +
            ",\"buttons_mask\":" + std::to_string(mask) +
            ",\"local_mask\":" + std::to_string(local_mask) +
            ",\"mcp_mask\":" + std::to_string(mcp_mask) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(mask)) + "\"" +
            ",\"pressed\":{" +
            "\"select\":" + (((mask & (1u << 0)) == 0) ? "true" : "false") +
            ",\"l3\":" + (((mask & (1u << 1)) == 0) ? "true" : "false") +
            ",\"r3\":" + (((mask & (1u << 2)) == 0) ? "true" : "false") +
            ",\"start\":" + (((mask & (1u << 3)) == 0) ? "true" : "false") +
            ",\"up\":" + (((mask & (1u << 4)) == 0) ? "true" : "false") +
            ",\"right\":" + (((mask & (1u << 5)) == 0) ? "true" : "false") +
            ",\"down\":" + (((mask & (1u << 6)) == 0) ? "true" : "false") +
            ",\"left\":" + (((mask & (1u << 7)) == 0) ? "true" : "false") +
            ",\"l2\":" + (((mask & (1u << 8)) == 0) ? "true" : "false") +
            ",\"r2\":" + (((mask & (1u << 9)) == 0) ? "true" : "false") +
            ",\"l1\":" + (((mask & (1u << 10)) == 0) ? "true" : "false") +
            ",\"r1\":" + (((mask & (1u << 11)) == 0) ? "true" : "false") +
            ",\"triangle\":" + (((mask & (1u << 12)) == 0) ? "true" : "false") +
            ",\"circle\":" + (((mask & (1u << 13)) == 0) ? "true" : "false") +
            ",\"cross\":" + (((mask & (1u << 14)) == 0) ? "true" : "false") +
            ",\"square\":" + (((mask & (1u << 15)) == 0) ? "true" : "false") +
            "}}";
        return true;
    }


    bool CoreMcpBackend::set_pad_state(uint32_t slot, uint16_t buttons_mask, std::string& out_json, std::string& err)
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }
        core_.set_pad_mcp_buttons_for_slot(slot, buttons_mask);
        const uint16_t effective = core_.pad_buttons_for_slot(slot);
        out_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"mcp_mask\":" + std::to_string((uint32_t)buttons_mask) +
            ",\"effective_mask\":" + std::to_string((uint32_t)effective) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(effective)) + "\"" +
            "}";
        return true;
    }


    bool CoreMcpBackend::tap_pad_buttons(uint32_t slot, uint16_t press_mask, uint32_t hold_steps, uint32_t release_steps, std::string& out_json, std::string& err)
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }

        if (hold_steps == 0)
            hold_steps = 1;
        if (hold_steps > 100000000u)
            hold_steps = 100000000u;
        if (release_steps > 100000000u)
            release_steps = 100000000u;

        const uint16_t previous_mask = core_.pad_mcp_buttons_for_slot(slot);
        const uint16_t pressed_mask = uint16_t(previous_mask & ~press_mask);
        core_.set_pad_mcp_buttons_for_slot(slot, pressed_mask);

        uint32_t hold_done = 0;
        for (; hold_done < hold_steps; ++hold_done)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);
                char msg[128];
                std::snprintf(msg, sizeof(msg), "tap hold stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                return false;
            }
        }

        const uint16_t observed_effective_mask = core_.pad_buttons_for_slot(slot);
        core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);

        uint32_t release_done = 0;
        for (; release_done < release_steps; ++release_done)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                char msg[128];
                std::snprintf(msg, sizeof(msg), "tap release stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                return false;
            }
        }

        out_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"previous_mask\":" + std::to_string((uint32_t)previous_mask) +
            ",\"mcp_pressed_mask\":" + std::to_string((uint32_t)pressed_mask) +
            ",\"press_mask\":" + std::to_string((uint32_t)press_mask) +
            ",\"observed_effective_mask\":" + std::to_string((uint32_t)observed_effective_mask) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(observed_effective_mask)) + "\"" +
            ",\"hold_steps\":" + std::to_string(hold_done) +
            ",\"release_steps\":" + std::to_string(release_done) +
            "}";
        return true;
    }


    bool CoreMcpBackend::tap_pad_named_buttons(uint32_t slot, const char* names_csv, uint32_t hold_steps, uint32_t release_steps, std::string& out_json, std::string& err)
    {
        uint16_t press_mask = 0;
        if (!parse_pad_names_csv(names_csv, press_mask, err))
            return false;
        return tap_pad_buttons(slot, press_mask, hold_steps, release_steps, out_json, err);
    }


    bool CoreMcpBackend::hold_pad_buttons(uint32_t slot, uint16_t press_mask, std::string& out_json, std::string& err)
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }

        const uint16_t previous_mask = core_.pad_mcp_buttons_for_slot(slot);
        const uint16_t current_mask = uint16_t(previous_mask & ~press_mask);
        core_.set_pad_mcp_buttons_for_slot(slot, current_mask);
        const uint16_t effective = core_.pad_buttons_for_slot(slot);
        out_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"previous_mask\":" + std::to_string((uint32_t)previous_mask) +
            ",\"mcp_mask\":" + std::to_string((uint32_t)current_mask) +
            ",\"effective_mask\":" + std::to_string((uint32_t)effective) +
            ",\"press_mask\":" + std::to_string((uint32_t)press_mask) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(effective)) + "\"" +
            "}";
        return true;
    }


    bool CoreMcpBackend::hold_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err)
    {
        uint16_t press_mask = 0;
        if (!parse_pad_names_csv(names_csv, press_mask, err))
            return false;
        return hold_pad_buttons(slot, press_mask, out_json, err);
    }


    bool CoreMcpBackend::release_pad_buttons(uint32_t slot, uint16_t release_mask, std::string& out_json, std::string& err)
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }

        const uint16_t previous_mask = core_.pad_mcp_buttons_for_slot(slot);
        const uint16_t current_mask = uint16_t(previous_mask | release_mask);
        core_.set_pad_mcp_buttons_for_slot(slot, current_mask);
        const uint16_t effective = core_.pad_buttons_for_slot(slot);
        out_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"previous_mask\":" + std::to_string((uint32_t)previous_mask) +
            ",\"mcp_mask\":" + std::to_string((uint32_t)current_mask) +
            ",\"effective_mask\":" + std::to_string((uint32_t)effective) +
            ",\"release_mask\":" + std::to_string((uint32_t)release_mask) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(effective)) + "\"" +
            "}";
        return true;
    }


    bool CoreMcpBackend::release_pad_named_buttons(uint32_t slot, const char* names_csv, std::string& out_json, std::string& err)
    {
        uint16_t release_mask = 0;
        if (!parse_pad_names_csv(names_csv, release_mask, err))
            return false;
        return release_pad_buttons(slot, release_mask, out_json, err);
    }


    bool CoreMcpBackend::get_scene_vector_snapshot(uint32_t max_groups, uint32_t max_roots, bool include_hud, bool include_raw_triangles, std::string& out_json, std::string& err) const
    {
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(max_groups, max_roots, include_hud, include_raw_triangles, snapshot, err))
            return false;
        out_json = add_timing_to_json_object(scene_vector_snapshot_to_json(snapshot), make_observation_timing_json(snapshot.frame_id));
        return true;
    }


    bool CoreMcpBackend::get_scene_delta(uint32_t max_groups, std::string& out_json, std::string& err)
    {
        SceneVectorSnapshot current{};
        if (!build_scene_vector_snapshot(max_groups, 12u, false, false, current, err))
            return false;

        out_json = add_timing_to_json_object(scene_delta_to_json(last_scene_snapshot_, current, has_last_scene_snapshot_), make_observation_timing_json(current.frame_id));
        last_scene_snapshot_ = current;
        has_last_scene_snapshot_ = true;
        return true;
    }


    bool CoreMcpBackend::get_object_candidates(uint32_t max_objects, std::string& out_json, std::string& err) const
    {
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(max_objects == 0 ? 12u : max_objects, 12u, false, false, snapshot, err))
            return false;
        out_json = add_timing_to_json_object(object_candidates_to_json(snapshot, max_objects == 0 ? 12u : max_objects), make_observation_timing_json(snapshot.frame_id));
        return true;
    }


    bool CoreMcpBackend::get_scene_salience_summary(uint32_t max_targets, std::string& out_json, std::string& err) const
    {
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(24u, 12u, false, false, snapshot, err))
            return false;
        out_json = add_timing_to_json_object(scene_salience_to_json(snapshot, max_targets == 0 ? 6u : max_targets), make_observation_timing_json(snapshot.frame_id));
        return true;
    }


    bool CoreMcpBackend::get_hierarchy_candidates(uint32_t max_nodes, std::string& out_json, std::string& err) const
    {
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(max_nodes == 0 ? 12u : max_nodes, 12u, false, false, snapshot, err))
            return false;
        out_json = add_timing_to_json_object(hierarchy_candidates_to_json(snapshot, max_nodes == 0 ? 12u : max_nodes), make_observation_timing_json(snapshot.frame_id));
        return true;
    }


    bool CoreMcpBackend::get_focus_candidate(std::string& out_json, std::string& err) const
    {
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(24u, 12u, false, false, snapshot, err))
            return false;
        out_json = add_timing_to_json_object(focus_candidate_to_json(snapshot), make_observation_timing_json(snapshot.frame_id));
        return true;
    }


    bool CoreMcpBackend::step_with_pad_observation(uint32_t slot, const char* names_csv, uint32_t hold_steps, uint32_t observe_steps, uint32_t max_groups, uint32_t max_targets, std::string& out_json, std::string& err)
    {
        if (!core_.bus())
        {
            err = "bus not initialized";
            return false;
        }
        if (slot >= r3000::Bus::kPadSlotCount)
        {
            err = "pad slot out of range";
            return false;
        }

        uint16_t press_mask = 0;
        if (!parse_pad_names_csv(names_csv, press_mask, err))
            return false;

        if (hold_steps == 0)
            hold_steps = 1;
        if (hold_steps > 100000000u)
            hold_steps = 100000000u;
        if (observe_steps > 100000000u)
            observe_steps = 100000000u;

        const uint16_t previous_mask = core_.pad_mcp_buttons_for_slot(slot);
        const uint16_t pressed_mask = uint16_t(previous_mask & ~press_mask);
        core_.set_pad_mcp_buttons_for_slot(slot, pressed_mask);

        uint32_t hold_done = 0;
        for (; hold_done < hold_steps; ++hold_done)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);
                char msg[128];
                std::snprintf(msg, sizeof(msg), "observe hold stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                return false;
            }
        }

        uint32_t observe_done = 0;
        for (; observe_done < observe_steps; ++observe_done)
        {
            const auto res = core_.step();
            if (res.kind != r3000::Cpu::StepResult::Kind::ok)
            {
                core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);
                char msg[128];
                std::snprintf(msg, sizeof(msg), "observe window stopped kind=%d pc=0x%08X", (int)res.kind, res.pc);
                err = msg;
                return false;
            }
        }

        SceneVectorSnapshot current{};
        if (!build_scene_vector_snapshot(max_groups == 0 ? 24u : max_groups, 12u, false, false, current, err))
        {
            core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);
            return false;
        }
        const uint16_t observed_effective_mask = core_.pad_buttons_for_slot(slot);
        core_.set_pad_mcp_buttons_for_slot(slot, previous_mask);

        const std::string pad_json =
            std::string("{") +
            "\"slot\":" + std::to_string(slot) +
            ",\"previous_mask\":" + std::to_string((uint32_t)previous_mask) +
            ",\"mcp_pressed_mask\":" + std::to_string((uint32_t)pressed_mask) +
            ",\"press_mask\":" + std::to_string((uint32_t)press_mask) +
            ",\"observed_effective_mask\":" + std::to_string((uint32_t)observed_effective_mask) +
            ",\"pressed_names\":\"" + emu::McpServer::json_escape(pad_mask_to_names(observed_effective_mask)) + "\"" +
            ",\"hold_steps\":" + std::to_string(hold_done) +
            ",\"observe_steps_held\":" + std::to_string(observe_done) +
            "}";
        const std::string snapshot_json = scene_vector_snapshot_to_json(current);
        const std::string delta_json = scene_delta_to_json(last_scene_snapshot_, current, has_last_scene_snapshot_);
        const std::string salience_json = scene_salience_to_json(current, max_targets == 0 ? 6u : max_targets);
        const std::string focus_json = focus_candidate_to_json(current);
        last_scene_snapshot_ = current;
        has_last_scene_snapshot_ = true;

        out_json =
            std::string("{") +
            "\"pad_action\":" + pad_json +
            ",\"observe_steps\":" + std::to_string(observe_done) +
            ",\"scene_snapshot\":" + snapshot_json +
            ",\"scene_delta\":" + delta_json +
            ",\"scene_salience\":" + salience_json +
            ",\"focus_candidate\":" + focus_json +
            ",\"timing\":" + make_observation_timing_json(current.frame_id) +
            "}";
        return true;
    }


    // v1 aggregator: composes existing observations into a single fingerprint JSON.
    // Consumer: LLM applying the decision tree §4 of docs/PSX_RENDER_LOOP_PLAYBOOK.md.
    // Each sub-section is a self-contained JSON object so the LLM can also query them
    // individually via their native endpoints if it needs more resolution.
    // Server-side scoring (candidate_classifications with confidences) is left empty
    // in v1 — the LLM reads the fingerprint and classifies. See playbook §4 JSON output
    // format for what the LLM should produce.
    bool CoreMcpBackend::match_render_pattern(uint32_t max_candidates, std::string& out_json, std::string& err) const
    {
        (void)max_candidates; // reserved for v2 server-side scoring

        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        // Current PC for context ("where are we right now").
        const uint32_t pc = cpu->pc();

        // Aggregate the 5 most relevant observations.  Each of these already
        // exists as a first-class MCP endpoint; we compose them in one round-trip.
        std::string gte_trace_json;
        std::string dma2_json;
        std::string drawlist_json;
        std::string scene_snapshot_json;
        std::string focus_json;
        std::string sub_err;

        if (!get_gte_trace_summary(gte_trace_json, sub_err))
            gte_trace_json = std::string("{\"error\":\"") + emu::McpServer::json_escape(sub_err) + "\"}";
        sub_err.clear();
        if (!get_dma2_nohint_summary(dma2_json, sub_err))
            dma2_json = std::string("{\"error\":\"") + emu::McpServer::json_escape(sub_err) + "\"}";
        sub_err.clear();
        if (!get_draw_list_summary(drawlist_json, sub_err))
            drawlist_json = std::string("{\"error\":\"") + emu::McpServer::json_escape(sub_err) + "\"}";
        sub_err.clear();

        // Scene snapshot (includes dominant_kind, roots, groups — key for classification).
        SceneVectorSnapshot snapshot{};
        if (!build_scene_vector_snapshot(24u, 12u, false, false, snapshot, sub_err))
            scene_snapshot_json = std::string("{\"error\":\"") + emu::McpServer::json_escape(sub_err) + "\"}";
        else
            scene_snapshot_json = scene_vector_snapshot_to_json(snapshot);
        sub_err.clear();

        focus_json = focus_candidate_to_json(snapshot);

        // Emit the fingerprint.  Structure is stable (schema versioned) so the
        // LLM / scripts can parse it reliably across sessions.
        char pc_buf[24];
        std::snprintf(pc_buf, sizeof(pc_buf), "\"0x%08X\"", pc);

        out_json =
            std::string("{") +
            "\"schema_version\":\"match_render_pattern/v1\"" +
            ",\"playbook_ref\":\"docs/PSX_RENDER_LOOP_PLAYBOOK.md\"" +
            ",\"cpu_pc\":" + pc_buf +
            ",\"gte_trace\":" + gte_trace_json +
            ",\"dma2\":" + dma2_json +
            ",\"drawlist\":" + drawlist_json +
            ",\"scene_snapshot\":" + scene_snapshot_json +
            ",\"focus_candidate\":" + focus_json +
            ",\"candidate_classifications\":[]" + // reserved for v2 server-side scoring
            ",\"classification_hint\":\"Apply playbook decision tree §4 on the signals above. Start at Q1 (RTPT dominance), branch down to Q4. Emit the output JSON format shown in playbook §4.\"" +
            ",\"timing\":" + make_observation_timing_json(snapshot.frame_id) +
            "}";
        return true;
    }


    bool CoreMcpBackend::run_until_mem_watch(uint32_t max_steps, uint64_t& event_seq, uint32_t& watch_id,
        uint32_t& hit_pc, uint32_t& hit_phys_addr, uint32_t& hit_value, uint32_t& hit_size,
        uint32_t& steps_done, bool& hit, std::string& err)
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


    bool CoreMcpBackend::set_psx3d_mode(const char* mode, std::string& err)
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


    bool CoreMcpBackend::request_psx3d_refresh(const char* reason, const char* scope, uint32_t& id, std::string& err)
    {
        if (!reason || !*reason)
        {
            err = "missing reason";
            return false;
        }
        id = core_.request_psx3d_analysis_refresh(reason, (scope && *scope) ? scope : "global");
        return true;
    }


    bool CoreMcpBackend::set_gte_trace_window(uint32_t pc_start, uint32_t pc_end, uint32_t start_frame, uint32_t end_frame, bool enabled, std::string& err)
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


    bool CoreMcpBackend::list_breakpoints(std::string& out_json, std::string& err) const
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


    bool CoreMcpBackend::set_breakpoint_pc(uint32_t pc, std::string& err)
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


    bool CoreMcpBackend::clear_breakpoint_pc(uint32_t pc, bool& removed, std::string& err)
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


    bool CoreMcpBackend::clear_all_breakpoints(uint32_t& removed_count, std::string& err)
    {
        (void)err;
        removed_count = (uint32_t)breakpoints_.size();
        breakpoints_.clear();
        return true;
    }


    bool CoreMcpBackend::run_until_breakpoint(uint32_t max_steps, uint32_t& hit_pc, uint32_t& steps_done, bool& hit, std::string& err)
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










    std::vector<CoreMcpBackend::SceneRootSnapshot> CoreMcpBackend::build_sorted_scene_roots(const emu::Core& core, const r3000::Cpu& cpu, uint32_t max_roots)
    {
        auto cams = cpu.camera_candidates_snapshot();
        const uint32_t ram_size = core.ram_size();
        std::sort(cams.begin(), cams.end(),
            [ram_size](const auto& a, const auto& b) {
                const int a_main = (a.addr < ram_size) ? 1 : 0;
                const int b_main = (b.addr < ram_size) ? 1 : 0;
                if (a_main != b_main)
                    return a_main > b_main;
                if (a.frame_hits != b.frame_hits)
                    return a.frame_hits > b.frame_hits;
                if (a.hits != b.hits)
                    return a.hits > b.hits;
                return a.addr < b.addr;
            });

        std::vector<SceneRootSnapshot> roots{};
        const size_t limit = std::min<size_t>(cams.size(), max_roots);
        roots.reserve(limit);
        for (size_t i = 0; i < limit; ++i)
        {
            const auto& c = cams[i];
            const bool main_ram = c.addr < ram_size;
            SceneRootSnapshot r{};
            r.addr = c.addr;
            r.region = main_ram ? "ram" : "scratch";
            r.score = (main_ram ? 1000u : 0u) + c.frame_hits * 10u + c.hits;
            r.hits = c.hits;
            r.frame_hits = c.frame_hits;
            r.last_pc = c.last_pc;
            r.gte_reg_mask = c.gte_reg_mask;
            roots.push_back(r);
        }
        return roots;
    }


    std::string CoreMcpBackend::make_observation_timing_json(uint32_t frame_id) const
    {
        const auto now = std::chrono::steady_clock::now();
        uint32_t frame_delta = 0;
        uint64_t wall_ms = 0;
        const bool has_previous = observation_timing_state_.valid;
        if (has_previous)
        {
            frame_delta = (frame_id >= observation_timing_state_.last_frame_id)
                ? (frame_id - observation_timing_state_.last_frame_id)
                : 0u;
            wall_ms = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - observation_timing_state_.last_tp).count();
        }
        observation_timing_state_.valid = true;
        observation_timing_state_.last_frame_id = frame_id;
        observation_timing_state_.last_tp = now;

        return std::string("{") +
            "\"has_previous\":" + (has_previous ? "true" : "false") +
            ",\"frame_id\":" + std::to_string(frame_id) +
            ",\"frame_delta_since_last_observation\":" + std::to_string(frame_delta) +
            ",\"wall_ms_since_last_observation\":" + std::to_string(wall_ms) +
            "}";
    }


    std::string CoreMcpBackend::add_timing_to_json_object(const std::string& base_json, const std::string& timing_json)
    {
        if (base_json.empty() || base_json.back() != '}')
            return base_json;
        std::string out = base_json;
        out.pop_back();
        if (out.size() > 1u)
            out += ",";
        out += "\"timing\":" + timing_json + "}";
        return out;
    }


    bool CoreMcpBackend::build_scene_vector_snapshot(uint32_t max_groups, uint32_t max_roots, bool include_hud, bool include_raw_triangles, CoreMcpBackend::SceneVectorSnapshot& out, std::string& err) const
    {
        (void)include_raw_triangles;
        gpu::FrameDrawList dl;
        std::vector<GroupObs> sorted{};
        if (!build_sorted_group_obs(dl, sorted, err))
            return false;

        const r3000::Cpu* cpu = core_.cpu();
        if (!cpu)
        {
            err = "cpu not initialized";
            return false;
        }

        out = {};
        out.frame_id = dl.frame_id;
        out.display_width = dl.display.width();
        out.display_height = dl.display.height();
        out.pal = dl.display.is_pal;

        uint32_t hud_like = 0;
        const size_t cmd_count = std::min(dl.cmds.size(), dl.cmds_3d.size());
        for (size_t i = 0; i < cmd_count; ++i)
        {
            const auto origin = dl.cmds_3d[i].origin;
            if (origin == gpu::PrimOrigin::origin_2d_hud || origin == gpu::PrimOrigin::origin_2d_rect || origin == gpu::PrimOrigin::origin_2d_line)
                ++hud_like;
        }
        out.hud_present = hud_like != 0;

        auto roots = build_sorted_scene_roots(core_, *cpu, max_roots == 0 ? 12u : max_roots);
        out.roots = roots;

        if (max_groups == 0)
            max_groups = 24;
        const size_t limit = std::min<size_t>(sorted.size(), max_groups);
        out.groups.reserve(limit);
        for (size_t i = 0; i < limit; ++i)
        {
            const auto& g = sorted[i];
            SceneGroupSnapshot sg{};
            char id_buf[64];
            std::snprintf(id_buf, sizeof(id_buf), "pc:0x%08X_face:%u", g.source_pc, g.face_idx);
            sg.group_id = id_buf;
            sg.source_pc = g.source_pc;
            sg.face_idx = g.face_idx;
            sg.tri_count = g.tri_count;
            sg.quad_halves = g.quad_halves;
            sg.min_ot_z = (g.min_ot_z == 0xFFFFFFFFu) ? 0u : g.min_ot_z;
            sg.max_ot_z = g.max_ot_z;
            sg.textured = g.textured;
            sg.semi = g.semi;
            sg.raw = g.raw;
            sg.seen_frames = g.seen_frames;
            sg.stable_score = g.stable_score;
            sg.min_x = g.min_x;
            sg.min_y = g.min_y;
            sg.max_x = g.max_x;
            sg.max_y = g.max_y;
            sg.center_x = 0.5f * (float(g.min_x) + float(g.max_x));
            sg.center_y = 0.5f * (float(g.min_y) + float(g.max_y));
            sg.extent_w = uint32_t((g.max_x >= g.min_x) ? (g.max_x - g.min_x) : 0);
            sg.extent_h = uint32_t((g.max_y >= g.min_y) ? (g.max_y - g.min_y) : 0);
            sg.material_signature =
                std::string("t") + std::to_string(g.textured) +
                "_s" + std::to_string(g.semi) +
                "_r" + std::to_string(g.raw);

            const uint64_t key = (uint64_t(g.source_pc) << 32) | uint64_t(g.face_idx);
            auto mc_it = mesh_cache_candidates_.find(key);
            if (mc_it != mesh_cache_candidates_.end())
            {
                sg.promotion_score = mc_it->second.promotion_score;
                if (mc_it->second.promotion_score >= 80u)
                    sg.kind = "object_candidate";
                else if (mc_it->second.promotion_score >= 45u)
                    sg.kind = "observe_candidate";
                else
                    sg.kind = "early_group";
            }
            else
            {
                sg.kind = "group";
            }

            if (i < roots.size())
            {
                sg.root_addr = roots[i].addr;
                sg.root_last_pc = roots[i].last_pc;
                sg.root_score = roots[i].score;
                const float tri_conf = std::min(0.45f, float(g.tri_count) / 160.0f);
                const float root_conf = std::min(0.35f, float(roots[i].score) / 3000.0f);
                const float stable_conf = std::min(0.20f, float(g.stable_score) / 64.0f);
                sg.root_confidence = std::min(0.95f, 0.15f + tri_conf + root_conf + stable_conf);
            }

            const float cx_norm = (out.display_width != 0) ? (sg.center_x / float(out.display_width)) : 0.5f;
            if (sg.tri_count >= 64u && sg.stable_score >= 8u)
                sg.notes = "large stable 3D group";
            else if (sg.tri_count >= 24u)
                sg.notes = "mid-size 3D group";
            else
                sg.notes = "small 3D group";
            if (cx_norm > 0.35f && cx_norm < 0.65f)
                sg.notes += " near screen center";
            else if (cx_norm <= 0.35f)
                sg.notes += " on left side";
            else
                sg.notes += " on right side";

            out.groups.push_back(sg);
        }

        if (!include_hud)
        {
            // HUD is still reflected in scene_summary; this flag only avoids trying
            // to synthesize explicit HUD objects before we have a better 2D grouping model.
        }

        if (!out.groups.empty())
        {
            const auto& g0 = out.groups.front();
            out.dominant_kind = "active_3d";
            out.dominant_source_pc = g0.source_pc;
            out.dominant_root_addr = g0.root_addr;
            out.dominant_group_count = uint32_t(out.groups.size());
            out.confidence = std::min(0.95f, 0.25f + std::min(0.35f, float(g0.tri_count) / 160.0f) + std::min(0.20f, float(g0.stable_score) / 64.0f) + (out.hud_present ? 0.05f : 0.0f));
            out.summary =
                std::string("Dominant 3D group from PC 0x") +
                emu::McpServer::json_escape([&]() { char b[16]; std::snprintf(b, sizeof(b), "%08X", g0.source_pc); return std::string(b); }()) +
                (out.hud_present ? " with separate HUD activity." : " with no obvious HUD activity.");
        }
        else
        {
            out.dominant_kind = out.hud_present ? "hud_or_2d" : "empty";
            out.confidence = out.hud_present ? 0.55f : 0.25f;
            out.summary = out.hud_present ? "No grouped 3D objects yet; scene currently looks mostly HUD/2D." : "No grouped scene objects visible yet.";
        }
        return true;
    }


    std::string CoreMcpBackend::scene_vector_snapshot_to_json(const CoreMcpBackend::SceneVectorSnapshot& s)
    {
        std::string out =
            std::string("{") +
            "\"frame_id\":" + std::to_string(s.frame_id) +
            ",\"display\":{\"width\":" + std::to_string(s.display_width) +
            ",\"height\":" + std::to_string(s.display_height) +
            ",\"pal\":" + (s.pal ? "true" : "false") + "}" +
            ",\"scene_summary\":{\"dominant_kind\":\"" + emu::McpServer::json_escape(s.dominant_kind) + "\"" +
            ",\"dominant_source_pc\":" + std::to_string(s.dominant_source_pc) +
            ",\"dominant_root_addr\":" + std::to_string(s.dominant_root_addr) +
            ",\"dominant_group_count\":" + std::to_string(s.dominant_group_count) +
            ",\"hud_present\":" + (s.hud_present ? "true" : "false") +
            ",\"confidence\":" + std::to_string(s.confidence) + "}" +
            ",\"groups\":[";
        for (size_t i = 0; i < s.groups.size(); ++i)
        {
            const auto& g = s.groups[i];
            if (i)
                out += ",";
            out += std::string("{\"group_id\":\"") + emu::McpServer::json_escape(g.group_id) +
                "\",\"source_pc\":" + std::to_string(g.source_pc) +
                ",\"face_idx\":" + std::to_string(g.face_idx) +
                ",\"kind\":\"" + emu::McpServer::json_escape(g.kind) + "\"" +
                ",\"tri_count\":" + std::to_string(g.tri_count) +
                ",\"quad_halves\":" + std::to_string(g.quad_halves) +
                ",\"screen_bbox\":{\"min_x\":" + std::to_string(g.min_x) +
                ",\"min_y\":" + std::to_string(g.min_y) +
                ",\"max_x\":" + std::to_string(g.max_x) +
                ",\"max_y\":" + std::to_string(g.max_y) + "}" +
                ",\"screen_center\":{\"x\":" + std::to_string(g.center_x) +
                ",\"y\":" + std::to_string(g.center_y) + "}" +
                ",\"screen_extent\":{\"w\":" + std::to_string(g.extent_w) +
                ",\"h\":" + std::to_string(g.extent_h) + "}" +
                ",\"ot_z_range\":{\"min\":" + std::to_string(g.min_ot_z) +
                ",\"max\":" + std::to_string(g.max_ot_z) + "}" +
                ",\"material_signature\":\"" + emu::McpServer::json_escape(g.material_signature) + "\"" +
                ",\"seen_frames\":" + std::to_string(g.seen_frames) +
                ",\"stable_score\":" + std::to_string(g.stable_score) +
                ",\"promotion_score\":" + std::to_string(g.promotion_score) +
                ",\"root_addr\":" + std::to_string(g.root_addr) +
                ",\"root_last_pc\":" + std::to_string(g.root_last_pc) +
                ",\"root_confidence\":" + std::to_string(g.root_confidence) +
                ",\"notes\":\"" + emu::McpServer::json_escape(g.notes) + "\"}";
        }
        out += "],\"roots\":[";
        for (size_t i = 0; i < s.roots.size(); ++i)
        {
            const auto& r = s.roots[i];
            if (i)
                out += ",";
            out += std::string("{\"addr\":") + std::to_string(r.addr) +
                ",\"region\":\"" + emu::McpServer::json_escape(r.region) + "\"" +
                ",\"score\":" + std::to_string(r.score) +
                ",\"hits\":" + std::to_string(r.hits) +
                ",\"frame_hits\":" + std::to_string(r.frame_hits) +
                ",\"last_pc\":" + std::to_string(r.last_pc) +
                ",\"gte_reg_mask\":" + std::to_string(r.gte_reg_mask) + "}";
        }
        out += "],\"summary\":\"" + emu::McpServer::json_escape(s.summary) + "\"}";
        return out;
    }


    std::string CoreMcpBackend::scene_delta_to_json(const CoreMcpBackend::SceneVectorSnapshot& prev, const CoreMcpBackend::SceneVectorSnapshot& current, bool has_prev)
    {
        if (!has_prev)
        {
            return std::string("{\"has_previous\":false,\"frame_id\":") + std::to_string(current.frame_id) +
                ",\"summary\":\"No previous scene snapshot available yet.\"}";
        }

        std::unordered_map<std::string, const SceneGroupSnapshot*> prev_map{};
        for (const auto& g : prev.groups)
            prev_map[g.group_id] = &g;
        std::unordered_map<std::string, const SceneGroupSnapshot*> cur_map{};
        for (const auto& g : current.groups)
            cur_map[g.group_id] = &g;

        std::vector<std::string> added{};
        std::vector<std::string> removed{};
        struct MoveInfo { std::string id; float dx; float dy; int dtri; };
        std::vector<MoveInfo> moved{};
        std::string most_central_id{};
        float most_central_shift = 0.0f;
        bool central_changed_size = false;
        std::string semantic_trend{"stable"};

        for (const auto& kv : cur_map)
        {
            auto it = prev_map.find(kv.first);
            if (it == prev_map.end())
            {
                added.push_back(kv.first);
                continue;
            }
            const auto* a = it->second;
            const auto* b = kv.second;
            const float dx = b->center_x - a->center_x;
            const float dy = b->center_y - a->center_y;
            const int dtri = int(b->tri_count) - int(a->tri_count);
            if (std::fabs(dx) >= 2.0f || std::fabs(dy) >= 2.0f || std::abs(dtri) >= 4)
                moved.push_back({kv.first, dx, dy, dtri});

            const float prev_centrality = std::fabs(a->center_x - float(prev.display_width) * 0.5f) + std::fabs(a->center_y - float(prev.display_height) * 0.5f);
            const float cur_centrality = std::fabs(b->center_x - float(current.display_width) * 0.5f) + std::fabs(b->center_y - float(current.display_height) * 0.5f);
            if (most_central_id.empty() || cur_centrality < most_central_shift)
            {
                most_central_id = kv.first;
                most_central_shift = cur_centrality;
                central_changed_size = std::abs(dtri) >= 4 || std::fabs(dx) >= 2.0f || std::fabs(dy) >= 2.0f || std::fabs(cur_centrality - prev_centrality) >= 2.0f;
            }
        }
        for (const auto& kv : prev_map)
        {
            if (cur_map.find(kv.first) == cur_map.end())
                removed.push_back(kv.first);
        }
        std::sort(moved.begin(), moved.end(), [](const MoveInfo& a, const MoveInfo& b) {
            const float am = std::fabs(a.dx) + std::fabs(a.dy) + float(std::abs(a.dtri)) * 0.25f;
            const float bm = std::fabs(b.dx) + std::fabs(b.dy) + float(std::abs(b.dtri)) * 0.25f;
            return am > bm;
        });

        std::string summary;
        if (!moved.empty())
            summary = "Scene changed: dominant groups moved or changed size.";
        else if (!added.empty() || !removed.empty())
            summary = "Scene membership changed without large motion.";
        else
            summary = "Scene looks stable across the last snapshot interval.";

        if (!moved.empty())
        {
            const auto& lead = moved.front();
            if (std::abs(lead.dtri) >= 8)
                semantic_trend = (lead.dtri > 0) ? "approaching_or_growing" : "receding_or_shrinking";
            else if (std::fabs(lead.dx) >= 6.0f || std::fabs(lead.dy) >= 6.0f)
                semantic_trend = "moving_laterally";
            else
                semantic_trend = "changed";
        }
        else if (!added.empty())
        {
            semantic_trend = "new_group_entered";
        }
        else if (!removed.empty())
        {
            semantic_trend = "group_disappeared";
        }

        std::string out =
            std::string("{") +
            "\"has_previous\":true" +
            ",\"previous_frame_id\":" + std::to_string(prev.frame_id) +
            ",\"current_frame_id\":" + std::to_string(current.frame_id) +
            ",\"dominant_changed\":" + (((prev.dominant_source_pc != current.dominant_source_pc) || (prev.dominant_root_addr != current.dominant_root_addr)) ? "true" : "false") +
            ",\"semantic_trend\":\"" + emu::McpServer::json_escape(semantic_trend) + "\"" +
            ",\"added_group_ids\":[";
        for (size_t i = 0; i < added.size(); ++i)
        {
            if (i)
                out += ",";
            out += std::string("\"") + emu::McpServer::json_escape(added[i]) + "\"";
        }
        out += "],\"removed_group_ids\":[";
        for (size_t i = 0; i < removed.size(); ++i)
        {
            if (i)
                out += ",";
            out += std::string("\"") + emu::McpServer::json_escape(removed[i]) + "\"";
        }
        out += "],\"moved_groups\":[";
        const size_t move_limit = std::min<size_t>(moved.size(), 8u);
        for (size_t i = 0; i < move_limit; ++i)
        {
            if (i)
                out += ",";
            out += std::string("{\"group_id\":\"") + emu::McpServer::json_escape(moved[i].id) +
                "\",\"dx\":" + std::to_string(moved[i].dx) +
                ",\"dy\":" + std::to_string(moved[i].dy) +
                ",\"dtri\":" + std::to_string(moved[i].dtri) + "}";
        }
        out += "],\"central_focus\":{\"group_id\":\"" + emu::McpServer::json_escape(most_central_id) +
            "\",\"changed\":" + (central_changed_size ? "true" : "false") + "}" +
            ",\"summary\":\"" + emu::McpServer::json_escape(summary) + "\"}";
        return out;
    }


    std::string CoreMcpBackend::object_candidates_to_json(const CoreMcpBackend::SceneVectorSnapshot& s, uint32_t max_objects)
    {
        std::vector<const SceneGroupSnapshot*> ranked{};
        ranked.reserve(s.groups.size());
        for (const auto& g : s.groups)
            ranked.push_back(&g);
        std::sort(ranked.begin(), ranked.end(), [](const SceneGroupSnapshot* a, const SceneGroupSnapshot* b) {
            if (a->promotion_score != b->promotion_score)
                return a->promotion_score > b->promotion_score;
            if (a->stable_score != b->stable_score)
                return a->stable_score > b->stable_score;
            return a->tri_count > b->tri_count;
        });

        const size_t limit = std::min<size_t>(ranked.size(), max_objects);
        std::string out =
            std::string("{") +
            "\"frame_id\":" + std::to_string(s.frame_id) +
            ",\"object_count\":" + std::to_string((uint32_t)limit) +
            ",\"objects\":[";
        for (size_t i = 0; i < limit; ++i)
        {
            const auto& g = *ranked[i];
            if (i)
                out += ",";
            const float object_conf = std::min(0.97f, 0.20f + std::min(0.35f, float(g.promotion_score) / 180.0f) + std::min(0.25f, float(g.stable_score) / 80.0f) + std::min(0.17f, float(g.tri_count) / 200.0f));
            out += std::string("{\"object_id\":\"") + emu::McpServer::json_escape(g.group_id) +
                "\",\"source_pc\":" + std::to_string(g.source_pc) +
                ",\"face_idx\":" + std::to_string(g.face_idx) +
                ",\"kind\":\"" + emu::McpServer::json_escape(g.kind) + "\"" +
                ",\"tri_count\":" + std::to_string(g.tri_count) +
                ",\"promotion_score\":" + std::to_string(g.promotion_score) +
                ",\"stable_score\":" + std::to_string(g.stable_score) +
                ",\"root_addr\":" + std::to_string(g.root_addr) +
                ",\"root_confidence\":" + std::to_string(g.root_confidence) +
                ",\"object_confidence\":" + std::to_string(object_conf) +
                ",\"screen_center\":{\"x\":" + std::to_string(g.center_x) +
                ",\"y\":" + std::to_string(g.center_y) + "}" +
                ",\"screen_extent\":{\"w\":" + std::to_string(g.extent_w) +
                ",\"h\":" + std::to_string(g.extent_h) + "}" +
                ",\"notes\":\"" + emu::McpServer::json_escape(g.notes) + "\"}";
        }
        out += "],\"summary\":\"";
        if (limit == 0)
            out += "No strong object candidates yet.";
        else
            out += emu::McpServer::json_escape("Top object candidates ranked by promotion score, stability, and size.");
        out += "\"}";
        return out;
    }


    std::string CoreMcpBackend::scene_salience_to_json(const CoreMcpBackend::SceneVectorSnapshot& s, uint32_t max_targets)
    {
        struct Target
        {
            const SceneGroupSnapshot* g{nullptr};
            float salience{0.0f};
            std::string reason{};
        };
        std::vector<Target> ranked{};
        ranked.reserve(s.groups.size());
        const float cx = float(s.display_width) * 0.5f;
        const float cy = float(s.display_height) * 0.5f;
        for (const auto& g : s.groups)
        {
            const float center_dist = std::fabs(g.center_x - cx) + std::fabs(g.center_y - cy);
            const float centrality = 1.0f / (1.0f + center_dist / 64.0f);
            const float size_score = std::min(1.0f, float(g.tri_count) / 80.0f);
            const float stable_score = std::min(1.0f, float(g.stable_score) / 24.0f);
            const float promo_score = std::min(1.0f, float(g.promotion_score) / 100.0f);
            const float salience = 0.30f * centrality + 0.25f * size_score + 0.20f * stable_score + 0.25f * promo_score;
            std::string reason = "central";
            if (g.promotion_score >= 80u)
                reason = "promoted_object";
            else if (g.tri_count >= 48u)
                reason = "large_visible_group";
            else if (g.stable_score >= 8u)
                reason = "stable_group";
            ranked.push_back({&g, salience, reason});
        }
        std::sort(ranked.begin(), ranked.end(), [](const Target& a, const Target& b) {
            if (a.salience != b.salience)
                return a.salience > b.salience;
            return a.g->tri_count > b.g->tri_count;
        });

        const size_t limit = std::min<size_t>(ranked.size(), max_targets);
        std::string out =
            std::string("{") +
            "\"frame_id\":" + std::to_string(s.frame_id) +
            ",\"target_count\":" + std::to_string((uint32_t)limit) +
            ",\"targets\":[";
        for (size_t i = 0; i < limit; ++i)
        {
            const auto& t = ranked[i];
            const auto& g = *t.g;
            if (i)
                out += ",";
            out += std::string("{\"group_id\":\"") + emu::McpServer::json_escape(g.group_id) +
                "\",\"salience\":" + std::to_string(t.salience) +
                ",\"reason\":\"" + emu::McpServer::json_escape(t.reason) + "\"" +
                ",\"tri_count\":" + std::to_string(g.tri_count) +
                ",\"promotion_score\":" + std::to_string(g.promotion_score) +
                ",\"screen_center\":{\"x\":" + std::to_string(g.center_x) +
                ",\"y\":" + std::to_string(g.center_y) + "}" +
                ",\"root_addr\":" + std::to_string(g.root_addr) +
                ",\"notes\":\"" + emu::McpServer::json_escape(g.notes) + "\"}";
        }
        out += "],\"summary\":\"";
        if (limit == 0)
            out += "No salient scene targets yet.";
        else if (limit == 1)
            out += emu::McpServer::json_escape("One dominant current target stands out in the scene.");
        else
            out += emu::McpServer::json_escape("A few dominant current targets stand out by centrality, size, and stability.");
        out += "\"}";
        return out;
    }


    std::string CoreMcpBackend::focus_candidate_to_json(const CoreMcpBackend::SceneVectorSnapshot& s)
    {
        if (s.groups.empty())
            return "{\"has_focus\":false,\"summary\":\"No focus candidate yet.\"}";

        const float cx = float(s.display_width) * 0.5f;
        const float cy = float(s.display_height) * 0.5f;
        const SceneGroupSnapshot* best = nullptr;
        float best_score = -1.0f;
        std::string reason{"none"};
        for (const auto& g : s.groups)
        {
            const float center_dist = std::fabs(g.center_x - cx) + std::fabs(g.center_y - cy);
            const float centrality = 1.0f / (1.0f + center_dist / 64.0f);
            const float size_score = std::min(1.0f, float(g.tri_count) / 80.0f);
            const float stable_score = std::min(1.0f, float(g.stable_score) / 24.0f);
            const float promo_score = std::min(1.0f, float(g.promotion_score) / 100.0f);
            const float root_score = std::min(1.0f, g.root_confidence);
            const float total = 0.28f * centrality + 0.22f * size_score + 0.20f * stable_score + 0.20f * promo_score + 0.10f * root_score;
            if (total > best_score)
            {
                best = &g;
                best_score = total;
                if (g.promotion_score >= 80u)
                    reason = "promoted_and_central";
                else if (centrality > 0.65f)
                    reason = "most_central";
                else if (g.tri_count >= 48u)
                    reason = "largest_visible_group";
                else
                    reason = "best_overall_score";
            }
        }

        return std::string("{\"has_focus\":true") +
            ",\"group_id\":\"" + emu::McpServer::json_escape(best->group_id) + "\"" +
            ",\"source_pc\":" + std::to_string(best->source_pc) +
            ",\"face_idx\":" + std::to_string(best->face_idx) +
            ",\"focus_score\":" + std::to_string(best_score) +
            ",\"reason\":\"" + emu::McpServer::json_escape(reason) + "\"" +
            ",\"root_addr\":" + std::to_string(best->root_addr) +
            ",\"screen_center\":{\"x\":" + std::to_string(best->center_x) +
            ",\"y\":" + std::to_string(best->center_y) + "}" +
            ",\"notes\":\"" + emu::McpServer::json_escape(best->notes) + "\"" +
            ",\"summary\":\"" + emu::McpServer::json_escape("Best current focus candidate for decision-making.") + "\"}";
    }


    std::string CoreMcpBackend::hierarchy_candidates_to_json(const CoreMcpBackend::SceneVectorSnapshot& s, uint32_t max_nodes)
    {
        struct Node
        {
            const SceneGroupSnapshot* g{nullptr};
            int parent{-1};
            float confidence{0.0f};
            std::string reason{};
        };
        std::vector<Node> nodes{};
        const size_t limit = std::min<size_t>(s.groups.size(), max_nodes);
        nodes.reserve(limit);
        for (size_t i = 0; i < limit; ++i)
            nodes.push_back({&s.groups[i], -1, 0.0f, {}});

        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& child = *nodes[i].g;
            for (size_t j = 0; j < nodes.size(); ++j)
            {
                if (i == j)
                    continue;
                const auto& parent = *nodes[j].g;
                if (child.root_addr == 0 || child.root_addr != parent.root_addr)
                    continue;
                if (parent.tri_count < child.tri_count)
                    continue;
                const bool contains =
                    parent.min_x <= child.min_x && parent.min_y <= child.min_y &&
                    parent.max_x >= child.max_x && parent.max_y >= child.max_y;
                if (!contains)
                    continue;
                const float size_ratio = (parent.tri_count != 0) ? (float(child.tri_count) / float(parent.tri_count)) : 0.0f;
                const float conf = std::min(0.90f, 0.35f + std::min(0.25f, parent.root_confidence) + std::min(0.20f, child.root_confidence) + std::min(0.10f, size_ratio));
                if (conf > nodes[i].confidence)
                {
                    nodes[i].parent = int(j);
                    nodes[i].confidence = conf;
                    nodes[i].reason = "shared_root_and_screen_containment";
                }
            }
        }

        std::string out =
            std::string("{") +
            "\"frame_id\":" + std::to_string(s.frame_id) +
            ",\"node_count\":" + std::to_string((uint32_t)nodes.size()) +
            ",\"nodes\":[";
        for (size_t i = 0; i < nodes.size(); ++i)
        {
            const auto& n = nodes[i];
            if (i)
                out += ",";
            out += std::string("{\"group_id\":\"") + emu::McpServer::json_escape(n.g->group_id) +
                "\",\"source_pc\":" + std::to_string(n.g->source_pc) +
                ",\"root_addr\":" + std::to_string(n.g->root_addr) +
                ",\"tri_count\":" + std::to_string(n.g->tri_count) +
                ",\"parent_group_id\":";
            if (n.parent >= 0)
                out += std::string("\"") + emu::McpServer::json_escape(nodes[(size_t)n.parent].g->group_id) + "\"";
            else
                out += "null";
            out += std::string(",\"hierarchy_confidence\":") + std::to_string(n.confidence) +
                ",\"reason\":\"" + emu::McpServer::json_escape(n.reason) + "\"" +
                ",\"notes\":\"" + emu::McpServer::json_escape(n.g->notes) + "\"}";
        }
        out += "],\"summary\":\"";
        bool any_parent = false;
        for (const auto& n : nodes)
        {
            if (n.parent >= 0)
            {
                any_parent = true;
                break;
            }
        }
        if (any_parent)
            out += emu::McpServer::json_escape("Some groups look hierarchically related through shared roots and containment.");
        else
            out += emu::McpServer::json_escape("No strong hierarchy candidate yet; groups still look mostly flat.");
        out += "\"}";
        return out;
    }


        bool CoreMcpBackend::build_sorted_group_obs(gpu::FrameDrawList& dl, std::vector<CoreMcpBackend::GroupObs>& sorted, std::string& err) const
    {
        gpu::Gpu3D* gpu3d = core_.gpu_3d();
        if (!gpu3d)
        {
            err = "gpu3d not initialized";
            return false;
        }

        gpu3d->copy_ready_draw_list(dl);
        update_temporal_group_cache(dl);

        std::unordered_map<uint64_t, GroupObs> groups{};
        const size_t count = std::min(dl.cmds.size(), dl.cmds_3d.size());
        for (size_t i = 0; i < count; ++i)
        {
            const auto& cmd = dl.cmds[i];
            const auto& cmd3d = dl.cmds_3d[i];
            if (cmd3d.origin != gpu::PrimOrigin::origin_3d)
                continue;
            if (cmd3d.face_idx == 0xFFFFFFFFu)
                continue;

            const uint64_t group_key = (uint64_t(cmd3d.source_pc) << 32) | uint64_t(cmd3d.face_idx);
            GroupObs& g = groups[group_key];
            g.source_pc = cmd3d.source_pc;
            g.face_idx = cmd3d.face_idx;
            ++g.tri_count;
            if (cmd3d.is_quad)
                ++g.quad_halves;
            g.min_ot_z = std::min(g.min_ot_z, cmd3d.ot_z);
            g.max_ot_z = std::max(g.max_ot_z, cmd3d.ot_z);
            if (cmd.flags & 0x1u) ++g.textured;
            if (cmd.flags & 0x2u) ++g.semi;
            if (cmd.flags & 0x4u) ++g.raw;
            for (int v = 0; v < 3; ++v)
            {
                g.min_x = std::min<int16_t>(g.min_x, cmd.v[v].x);
                g.min_y = std::min<int16_t>(g.min_y, cmd.v[v].y);
                g.max_x = std::max<int16_t>(g.max_x, cmd.v[v].x);
                g.max_y = std::max<int16_t>(g.max_y, cmd.v[v].y);
            }
        }

        sorted.clear();
        sorted.reserve(groups.size());
        for (auto& kv : groups)
        {
            auto it = temporal_groups_.find(kv.first);
            if (it != temporal_groups_.end())
            {
                kv.second.first_frame = it->second.first_frame;
                kv.second.last_frame = it->second.last_frame;
                kv.second.seen_frames = it->second.seen_frames;
                kv.second.stable_score = it->second.stable_hits;
                if (!kv.second.source_pc)
                    kv.second.source_pc = it->second.source_pc;
            }
            else
            {
                kv.second.first_frame = dl.frame_id;
                kv.second.last_frame = dl.frame_id;
                kv.second.seen_frames = 1;
                kv.second.stable_score = 0;
            }
            sorted.push_back(kv.second);
        }
        std::sort(sorted.begin(), sorted.end(),
            [](const GroupObs& a, const GroupObs& b) {
                if (a.tri_count != b.tri_count)
                    return a.tri_count > b.tri_count;
                if (a.source_pc != b.source_pc)
                    return a.source_pc < b.source_pc;
                return a.face_idx < b.face_idx;
            });
        return true;
    }


    void CoreMcpBackend::update_temporal_group_cache(const gpu::FrameDrawList& dl) const
    {
        if (temporal_last_frame_id_ == dl.frame_id)
            return;
        temporal_last_frame_id_ = dl.frame_id;

        struct Instant
        {
            uint32_t tri_count{0};
            uint32_t min_ot_z{0xFFFFFFFFu};
            uint32_t max_ot_z{0};
            uint32_t textured{0};
            uint32_t semi{0};
            uint32_t raw{0};
        };
        std::unordered_map<uint64_t, Instant> current{};
        const size_t count = std::min(dl.cmds.size(), dl.cmds_3d.size());
        for (size_t i = 0; i < count; ++i)
        {
            const auto& cmd = dl.cmds[i];
            const auto& cmd3d = dl.cmds_3d[i];
            if (cmd3d.origin != gpu::PrimOrigin::origin_3d || cmd3d.face_idx == 0xFFFFFFFFu)
                continue;
            const uint64_t group_key = (uint64_t(cmd3d.source_pc) << 32) | uint64_t(cmd3d.face_idx);
            Instant& inst = current[group_key];
            ++inst.tri_count;
            inst.min_ot_z = std::min(inst.min_ot_z, cmd3d.ot_z);
            inst.max_ot_z = std::max(inst.max_ot_z, cmd3d.ot_z);
            if (cmd.flags & 0x1u) ++inst.textured;
            if (cmd.flags & 0x2u) ++inst.semi;
            if (cmd.flags & 0x4u) ++inst.raw;
        }

        for (const auto& kv : current)
        {
            GroupTemporalState& st = temporal_groups_[kv.first];
            st.source_pc = uint32_t(kv.first >> 32);
            st.face_idx = uint32_t(kv.first & 0xFFFFFFFFu);
            if (st.seen_frames == 0)
                st.first_frame = dl.frame_id;
            st.last_frame = dl.frame_id;
            ++st.seen_frames;
            if (st.last_valid &&
                st.last_tri_count == kv.second.tri_count &&
                st.last_min_ot_z == kv.second.min_ot_z &&
                st.last_max_ot_z == kv.second.max_ot_z)
            {
                ++st.stable_hits;
            }
            st.last_tri_count = kv.second.tri_count;
            st.last_min_ot_z = (kv.second.min_ot_z == 0xFFFFFFFFu) ? 0u : kv.second.min_ot_z;
            st.last_max_ot_z = kv.second.max_ot_z;
            st.last_valid = true;

            MeshCacheCandidate& mc = mesh_cache_candidates_[kv.first];
            mc.source_pc = st.source_pc;
            mc.face_idx = st.face_idx;
            if (mc.seen_frames == 0)
                mc.first_frame = dl.frame_id;
            mc.last_frame = dl.frame_id;
            mc.seen_frames = st.seen_frames;
            mc.last_tri_count = kv.second.tri_count;
            mc.stable_ot_z = st.last_valid && (st.last_min_ot_z == st.last_max_ot_z);
            mc.material_signature =
                std::string("t") + std::to_string(kv.second.textured) +
                "_s" + std::to_string(kv.second.semi) +
                "_r" + std::to_string(kv.second.raw);
            mc.promotion_score = std::min<uint32_t>(100u,
                std::min<uint32_t>(st.seen_frames * 8u, 56u) +
                std::min<uint32_t>(st.stable_hits * 6u, 36u) +
                (mc.stable_ot_z ? 8u : 0u));
            if (mc.promotion_score >= 80u)
                mc.status = "promote_candidate";
            else if (mc.promotion_score >= 45u)
                mc.status = "observe_more";
            else
                mc.status = "early_candidate";
        }
    }



    void CoreMcpBackend::step_hook_trampoline(uint32_t pc, void* user)
    {
        static_cast<CoreMcpBackend*>(user)->on_step_hook(pc);
    }


    void CoreMcpBackend::write_hook_trampoline(uint32_t phys_addr, uint32_t value, uint32_t size, void* user)
    {
        static_cast<CoreMcpBackend*>(user)->on_write_hook(phys_addr, value, size);
    }


    void CoreMcpBackend::on_step_hook(uint32_t pc)
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


    void CoreMcpBackend::on_write_hook(uint32_t phys_addr, uint32_t value, uint32_t size)
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



} // namespace emu
