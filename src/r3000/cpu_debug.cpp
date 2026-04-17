// ────────────────────────────────────────────────────────────────────────────
//  cpu_debug.cpp — debug-only Cpu:: methods extracted from cpu.cpp
// ────────────────────────────────────────────────────────────────────────────
//
// This TU holds the bodies of methods that are NOT on the interpreter hot-path
// and don't affect emulation semantics:
//
//   - Cpu::record_crash_trace / Cpu::dump_crash_trace
//       Ring-buffer of last N instructions for post-mortem after a CPU fault.
//
//   - Cpu::observe_camera_mtc2 / Cpu::maybe_log_camera_candidates
//     Cpu::camera_candidates_snapshot / Cpu::restore_camera_candidates
//       GTE MTC2 observer used to identify candidate "camera matrix root"
//       addresses for 3D reconstruction (VR geometry inference).
//
// Declarations live in cpu.h (unchanged).  Moving the bodies here lets
// cpu.cpp focus on the interpreter dispatch.  No behavioural change.
// ────────────────────────────────────────────────────────────────────────────

#include "cpu.h"

#include <algorithm>
#include <vector>

#include "../log/emu_log.h"
#include "cpu_helpers.h"   // is_camera_track_addr

namespace r3000
{

// ── Crash trace ────────────────────────────────────────────────────────────

void Cpu::record_crash_trace(uint32_t instr)
{
    if (!crash_trace_enabled_)
        return;

    CrashTraceEntry& e = crash_trace_[crash_trace_pos_];
    e.pc = pc_;
    e.instr = instr;
    e.hi = hi_;
    e.lo = lo_;
    e.status = cop0_[COP0_STATUS];
    e.cause = cop0_[COP0_CAUSE];
    e.epc = cop0_[COP0_EPC];
    e.badvaddr = cop0_[COP0_BADVADDR];
    for (uint32_t i = 0; i < 32; ++i)
        e.gpr[i] = gpr_[i];

    crash_trace_pos_ = (crash_trace_pos_ + 1u) % kCrashTraceCapacity;
    if (crash_trace_count_ < kCrashTraceCapacity)
        ++crash_trace_count_;
}

void Cpu::dump_crash_trace(const char* reason) const
{
    if (!crash_trace_enabled_ || crash_trace_dump_count_ == 0 || crash_trace_count_ == 0)
        return;

    const uint32_t count = (crash_trace_dump_count_ < crash_trace_count_) ? crash_trace_dump_count_ : crash_trace_count_;
    emu::logf(emu::LogLevel::error, "CPU", "CrashTrace dump: reason=%s entries=%u", reason ? reason : "unknown", count);

    const uint32_t start = (crash_trace_pos_ + kCrashTraceCapacity - count) % kCrashTraceCapacity;
    for (uint32_t i = 0; i < count; ++i)
    {
        const CrashTraceEntry& e = crash_trace_[(start + i) % kCrashTraceCapacity];
        emu::logf(
            emu::LogLevel::error,
            "CPU",
            "CrashTrace[%03u] pc=0x%08X instr=0x%08X hi=0x%08X lo=0x%08X sr=0x%08X cause=0x%08X epc=0x%08X bad=0x%08X",
            i,
            e.pc, e.instr, e.hi, e.lo, e.status, e.cause, e.epc, e.badvaddr);
        emu::logf(
            emu::LogLevel::error,
            "CPU",
            "CrashTrace[%03u] v0=0x%08X v1=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X t0=0x%08X t1=0x%08X",
            i,
            e.gpr[2], e.gpr[3], e.gpr[4], e.gpr[5], e.gpr[6], e.gpr[7], e.gpr[8], e.gpr[9]);
        emu::logf(
            emu::LogLevel::error,
            "CPU",
            "CrashTrace[%03u] t2=0x%08X t3=0x%08X t4=0x%08X t5=0x%08X t6=0x%08X t7=0x%08X s0=0x%08X s1=0x%08X",
            i,
            e.gpr[10], e.gpr[11], e.gpr[12], e.gpr[13], e.gpr[14], e.gpr[15], e.gpr[16], e.gpr[17]);
        emu::logf(
            emu::LogLevel::error,
            "CPU",
            "CrashTrace[%03u] s2=0x%08X s3=0x%08X s4=0x%08X s5=0x%08X s6=0x%08X s7=0x%08X t8=0x%08X t9=0x%08X",
            i,
            e.gpr[18], e.gpr[19], e.gpr[20], e.gpr[21], e.gpr[22], e.gpr[23], e.gpr[24], e.gpr[25]);
        emu::logf(
            emu::LogLevel::error,
            "CPU",
            "CrashTrace[%03u] k0=0x%08X k1=0x%08X gp=0x%08X sp=0x%08X fp=0x%08X ra=0x%08X at=0x%08X",
            i,
            e.gpr[26], e.gpr[27], e.gpr[28], e.gpr[29], e.gpr[30], e.gpr[31], e.gpr[1]);
    }
}

// ── Camera-root candidate tracking (3D reconstruction) ─────────────────────

void Cpu::observe_camera_mtc2(uint32_t pc, uint32_t gte_data_reg, uint32_t cpu_src_reg)
{
    if (!camera_analysis_enabled_)
        return;
    if ((gte_data_reg & 31u) > 11u)
        return;
    const uint32_t s = cpu_src_reg & 31u;
    if (s == 0u || !reg_last_mem_valid_[s])
        return;

    const uint32_t vb = bus_.vblank_count();
    const uint32_t load_vb = reg_last_mem_vblank_[s];
    if (vb < load_vb || (vb - load_vb) > 3u)
        return;

    const uint32_t addr = reg_last_mem_addr_[s] & ~3u;
    if (!is_camera_track_addr(addr, bus_.ram_size()))
        return;
    auto it = camera_root_candidates_.find(addr);
    if (it == camera_root_candidates_.end())
    {
        CameraRootCandidate c{};
        c.addr = addr;
        c.first_vblank = vb;
        c.last_vblank = vb;
        c.last_pc = pc;
        c.hits = 1;
        c.frame_hits = 1;
        c.gte_reg_mask = (1u << (gte_data_reg & 31u));
        camera_root_candidates_.emplace(addr, c);
        ++camera_candidates_serial_;
        return;
    }

    CameraRootCandidate& c = it->second;
    ++c.hits;
    if (c.last_vblank != vb)
        ++c.frame_hits;
    c.last_vblank = vb;
    c.last_pc = pc;
    c.gte_reg_mask |= (1u << (gte_data_reg & 31u));
    ++camera_candidates_serial_;
}

void Cpu::maybe_log_camera_candidates()
{
    if (!camera_analysis_enabled_)
        return;
    const uint32_t vb = bus_.vblank_count();
    if (vb == camera_last_vblank_seen_)
        return;
    camera_last_vblank_seen_ = vb;
    if (camera_root_candidates_.empty())
        return;

    if (vb < 120u || (vb - camera_last_log_vblank_) < 120u)
        return;
    camera_last_log_vblank_ = vb;

    std::vector<const CameraRootCandidate*> top;
    top.reserve(camera_root_candidates_.size());
    for (const auto& kv : camera_root_candidates_)
        top.push_back(&kv.second);

    std::sort(top.begin(), top.end(),
        [this](const CameraRootCandidate* a, const CameraRootCandidate* b) {
            const int a_main = (a->addr < bus_.ram_size()) ? 1 : 0;
            const int b_main = (b->addr < bus_.ram_size()) ? 1 : 0;
            if (a_main != b_main)
                return a_main > b_main; // Prefer true game RAM candidates
            if (a->frame_hits != b->frame_hits)
                return a->frame_hits > b->frame_hits;
            if (a->hits != b->hits)
                return a->hits > b->hits;
            return a->addr < b->addr;
        });

    const uint32_t n = (uint32_t)top.size() > 6u ? 6u : (uint32_t)top.size();
    emu::logf(
        emu::LogLevel::debug,
        "CAM_ROOT",
        "vblank=%u candidates=%u top=%u",
        vb,
        (uint32_t)top.size(),
        n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const CameraRootCandidate* c = top[i];
        emu::logf(
            emu::LogLevel::debug,
            "CAM_ROOT",
            "  #%u addr=0x%08X region=%s frame_hits=%u hits=%u regs=0x%03X last_pc=0x%08X",
            i,
            c->addr,
            (c->addr < bus_.ram_size()) ? "ram" : "scratch",
            c->frame_hits,
            c->hits,
            c->gte_reg_mask & 0xFFFu,
            c->last_pc);
    }
}

std::vector<Cpu::CameraCandidateSnapshot> Cpu::camera_candidates_snapshot() const
{
    std::vector<CameraCandidateSnapshot> out;
    out.reserve(camera_root_candidates_.size());
    for (const auto& kv : camera_root_candidates_)
    {
        const CameraRootCandidate& c = kv.second;
        CameraCandidateSnapshot s{};
        s.addr = c.addr;
        s.hits = c.hits;
        s.frame_hits = c.frame_hits;
        s.first_vblank = c.first_vblank;
        s.last_vblank = c.last_vblank;
        s.last_pc = c.last_pc;
        s.gte_reg_mask = c.gte_reg_mask;
        out.push_back(s);
    }
    return out;
}

void Cpu::restore_camera_candidates(const std::vector<CameraCandidateSnapshot>& in)
{
    camera_root_candidates_.clear();
    camera_last_vblank_seen_ = 0;
    camera_last_log_vblank_ = 0;
    for (const auto& s : in)
    {
        if (s.addr == 0 || s.hits == 0)
            continue;
        if (!is_camera_track_addr(s.addr, bus_.ram_size()))
            continue;
        CameraRootCandidate c{};
        c.addr = s.addr;
        c.hits = s.hits;
        c.frame_hits = s.frame_hits;
        c.first_vblank = s.first_vblank;
        c.last_vblank = s.last_vblank;
        c.last_pc = s.last_pc;
        c.gte_reg_mask = s.gte_reg_mask;
        camera_root_candidates_[c.addr] = c;
        if (c.last_vblank > camera_last_vblank_seen_)
            camera_last_vblank_seen_ = c.last_vblank;
    }
    ++camera_candidates_serial_;
}

} // namespace r3000
