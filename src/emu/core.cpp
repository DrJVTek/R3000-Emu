#include "core.h"

#include <new>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Explicit include to keep tooling in sync with Cpu API.
#include "../r3000/cpu.h"
#include "../log/emu_log.h"

// Boot start time for milestone tracking
static std::chrono::steady_clock::time_point g_boot_start;
static bool g_boot_start_set = false;
static uint64_t g_step_count = 0;

namespace emu
{

// Callback for CDROM garbage SetLoc detection
static void on_garbage_setloc(uint32_t lba, uint32_t disc_end, void* /*user*/)
{
    emu::logf(emu::LogLevel::warn, "CDROM", "Garbage SetLoc: LBA=%u >= disc_end=%u", lba, disc_end);
}

static void set_errf(char* err, size_t cap, const char* fmt, const char* a = nullptr)
{
    if (!err || cap == 0)
        return;
    if (!fmt)
    {
        err[0] = '\0';
        return;
    }
    if (a)
        std::snprintf(err, cap, fmt, a);
    else
        std::snprintf(err, cap, "%s", fmt);
}

Core::Core(rlog::Logger* logger) : logger_(logger), cdrom_(logger), gpu_(logger)
{
    // Version marker - update when making changes!
    emu::logf(emu::LogLevel::warn, "CORE", "R3000-Emu core v6 (vsync_stuck_detect)");
    psx3d_mode_mgr_.reset();
    provenance_profiler_.reset();
}

static std::string psx3d_sanitize_game_id(std::string s)
{
    if (s.empty())
        return "unknown";
    for (char& c : s)
    {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-' || c == '.';
        if (!ok)
            c = '_';
    }
    while (!s.empty() && (s.back() == '.' || s.back() == ' '))
        s.pop_back();
    return s.empty() ? "unknown" : s;
}

Core::~Core()
{
    try_save_psx3d_profile();
}

void Core::set_err(char* err, size_t err_cap, const char* msg) const
{
    set_errf(err, err_cap, msg);
}

bool Core::alloc_ram(uint32_t bytes, char* err, size_t err_cap)
{
    if (ram_)
    {
        set_err(err, err_cap, "RAM already allocated");
        return false;
    }
    if (bytes == 0)
    {
        set_err(err, err_cap, "invalid RAM size");
        return false;
    }
    ram_.reset(new (std::nothrow) uint8_t[bytes]{});
    if (!ram_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    ram_size_ = bytes;
    return true;
}

bool Core::set_bios_copy(const uint8_t* bios, uint32_t bios_size, char* err, size_t err_cap)
{
    bios_.reset();
    bios_size_ = 0;

    if (!bios || bios_size == 0)
        return true; // BIOS optional (load-only mode).

    bios_.reset(new (std::nothrow) uint8_t[bios_size]);
    if (!bios_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    std::memcpy(bios_.get(), bios, bios_size);
    bios_size_ = bios_size;
    return true;
}

void Core::set_log_sinks(const flog::Sink& cdlog, const flog::Sink& gpulog, const flog::Sink& syslog, const flog::Sink& iolog, const flog::Clock& clock)
{
    cdlog_ = cdlog;
    gpulog_ = gpulog;
    syslog_ = syslog;
    iolog_ = iolog;
    clock_ = clock;
    has_clock_ = 1;

    cdrom_.set_log_sinks(cdlog_, iolog_, clock_);
    gpu_.set_log_sinks(gpulog_, iolog_, clock_);
}

void Core::set_text_out(std::FILE* f)
{
    text_out_ = f;
    if (cpu_)
        cpu_->set_text_out(text_out_);
}

void Core::set_text_io_sink(const flog::Sink& s, const flog::Clock& c)
{
    text_io_ = s;
    text_clock_ = c;
    has_text_clock_ = 1;
    if (cpu_)
        cpu_->set_text_io_sink(text_io_, text_clock_);
}

void Core::set_putchar_callback(PutcharCallback cb, void* user)
{
    putchar_cb_ = cb;
    putchar_cb_user_ = user;
    if (cpu_)
        cpu_->set_putchar_callback(cb, user);
}

bool Core::insert_disc(const char* path, char* err, size_t err_cap)
{
    emu::logf(emu::LogLevel::info, "CORE", "insert_disc: path=%s", path ? path : "(null)");
    if (!path || !*path)
    {
        set_err(err, err_cap, "invalid disc path");
        return false;
    }
    bool ok = cdrom_.insert_disc(path, err, err_cap);
    emu::logf(emu::LogLevel::info, "CORE", "insert_disc result: %d", ok);
    if (ok)
    {
        set_psx3d_profile_identity_from_path(path);
        try_load_psx3d_profile();
    }
    return ok;
}

void Core::set_gpu_dump_file(const char* path)
{
    if (path && *path)
        gpu_.set_dump_file(path);
}

void Core::set_compare_file(std::FILE* f)
{
    compare_file_ = f;
}

uint8_t* Core::ram()
{
    return ram_.get();
}

uint32_t Core::ram_size() const
{
    return ram_size_;
}

const uint8_t* Core::bios_data() const
{
    return bios_.get();
}

uint32_t Core::bios_size() const
{
    return bios_size_;
}

bool Core::init_from_image(const loader::LoadedImage& img, const InitOptions& opt, char* err, size_t err_cap)
{
    if (!ram_ || ram_size_ == 0)
    {
        set_err(err, err_cap, "RAM not allocated");
        return false;
    }

    // (Re)create bus/cpu.
    bus_.reset(new (std::nothrow) r3000::Bus(ram_.get(), ram_size_, bios_.get(), bios_size_, &cdrom_, &gpu_, logger_));
    if (!bus_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    cpu_.reset(new (std::nothrow) r3000::Cpu(*bus_, logger_));
    if (!cpu_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }

    // Hook system: pass hooks to Bus for VBlank/write dispatch.
    bus_->set_hooks(&hooks_);

    // Shadow GTE/GPU for 3D tag-based reconstruction.
    gpu_3d_.bind_gte_3d(&gte_3d_);
    bus_->set_gpu_3d(&gpu_3d_);
    bus_->set_gte_3d(&gte_3d_);
    cpu_->set_gte_shadow(&gte_3d_);

    // Bus tracing options (diagnostic only).
    if (has_clock_)
        bus_->set_trace_vector_sink(iolog_, clock_);
    bus_->set_trace_vectors(opt.trace_vectors ? 1 : 0);
    if (opt.watch_u32_enabled)
        bus_->set_watch_ram_u32(opt.watch_u32_phys, 1);

    cpu_->reset(img.entry_pc);

    cpu_->set_pretty(opt.pretty ? 1 : 0);
    cpu_->set_trace_io(opt.trace_io ? 1 : 0);
    cpu_->set_hle_vectors(opt.hle_vectors ? 1 : 0);

    // GPU generates real VBlanks at ~50Hz. Disable HLE pseudo-vblank (~333Hz).
    cpu_->set_use_gpu_vblank(1);

    cpu_->set_loop_detectors(opt.loop_detectors ? 1 : 0);
    cpu_->set_bus_tick_batch(opt.bus_tick_batch);
    cpu_->set_stop_on_high_ram(opt.stop_on_high_ram ? 1 : 0);
    cpu_->set_stop_on_bios_to_ram_nop(opt.stop_on_bios_to_ram_nop ? 1 : 0);
    cpu_->set_stop_on_ram_nop(opt.stop_on_ram_nop ? 1 : 0);
    cpu_->set_camera_analysis_enabled(psx3d_mode_mgr_.analysis_enabled() ? 1 : 0);
    if (opt.stop_on_pc_enabled)
        cpu_->set_stop_on_pc(opt.stop_on_pc, 1);

    // System/io sinks for higher-signal events.
    if (has_clock_)
        cpu_->set_sys_log_sinks(syslog_, iolog_, clock_);

    if (compare_file_)
        cpu_->set_compare_file(compare_file_);

    // Apply pending text sinks.
    if (text_out_)
        cpu_->set_text_out(text_out_);
    if (has_text_clock_ && text_io_.f)
        cpu_->set_text_io_sink(text_io_, text_clock_);
    if (putchar_cb_)
        cpu_->set_putchar_callback(putchar_cb_, putchar_cb_user_);

    // Set up CDROM garbage SetLoc callback for debugging
    cdrom_.set_garbage_setloc_callback(on_garbage_setloc, this);

    // Apply initial registers (loader-provided).
    if (img.has_gp)
        cpu_->set_gpr(28, img.gp);
    if (img.has_sp)
        cpu_->set_gpr(29, img.sp);
    cpu_->set_pc(img.entry_pc);
    last_vblank_seen_ = 0;
    last_gpu3d_frame_seen_ = 0;
    last_gpu3d_refresh_frame_ = 0;
    provenance_profiler_.reset();
    psx3d_analyzed_pcs_.clear();
    psx3d_cam_serial_seen_ = 0;
    psx3d_profile_dirty_ = false;
    psx3d_profile_loaded_ = false;
    try_load_psx3d_profile();

    return true;
}

r3000::Cpu::StepResult Core::step()
{
    if (!cpu_)
    {
        r3000::Cpu::StepResult r{};
        r.kind = r3000::Cpu::StepResult::Kind::halted;
        return r;
    }

    // Initialize boot timer on first step
    if (!g_boot_start_set)
    {
        g_boot_start = std::chrono::steady_clock::now();
        g_boot_start_set = true;
        emu::logf(emu::LogLevel::info, "MILESTONE", "=== BOOT START (step 0) ===");
    }

    const uint32_t pc_before = cpu_->pc();
    const auto res = cpu_->step();
    ++g_step_count;

    // PSX3D analysis refresh trigger (manual queue).
    if (psx3d_mode_mgr_.analysis_enabled() &&
        psx3d_mode_mgr_.analysis_active() &&
        psx3d_mode_mgr_.has_refresh_request())
    {
        const auto req = psx3d_mode_mgr_.consume_refresh_request();
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "analysis refresh request id=%u reason=%s scope=%s",
            req.id,
            req.reason.empty() ? "(none)" : req.reason.c_str(),
            req.scope.empty() ? "(none)" : req.scope.c_str());
        run_psx3d_analysis_refresh(req);
        try_save_psx3d_profile();
        // Return automatically to normal game mode after a refresh pass.
        if (!psx3d_mode_mgr_.has_refresh_request())
            set_psx3d_mode(Psx3dRunMode::game);
    }

    // Generic auto-profiling: detect no-token DMA2 hotspots and auto-request refresh.
    if (psx3d_mode_mgr_.analysis_enabled() && bus_)
    {
        r3000::Bus::Dma2NoHintSummary s{};
        if (bus_->consume_dma2_nohint_summary(s))
        {
            psx3d_last_nohint_top_pcs_.clear();
            psx3d_last_nohint_top_pcs_.reserve(s.top_pcs.size());
            for (const auto& p : s.top_pcs)
            {
                if (p.first != 0)
                    psx3d_last_nohint_top_pcs_.push_back(p.first);
            }
            if (provenance_profiler_.ingest(s))
                psx3d_profile_dirty_ = true;
            last_vblank_seen_ = s.vblank;
            const auto d = provenance_profiler_.decide_refresh(s.vblank);
            if (d.request)
            {
                if (psx3d_analyzed_pcs_.find(d.pc) == psx3d_analyzed_pcs_.end())
                {
                    if (psx3d_mode_mgr_.mode() != Psx3dRunMode::analysis)
                        set_psx3d_mode(Psx3dRunMode::analysis);
                    char scope[64];
                    std::snprintf(scope, sizeof(scope), "pc=0x%08X", d.pc);
                    request_psx3d_analysis_refresh("auto_hotspot", scope);
                    emu::logf(
                        emu::LogLevel::warn,
                        "PSX3D",
                        "auto hotspot refresh pc=0x%08X weight=%u vblank=%u",
                        d.pc,
                        d.weight,
                        s.vblank);
                }
                // Even if already analyzed, ack to apply cooldown and avoid spam.
                provenance_profiler_.ack_refresh(d.pc, s.vblank);
            }
            if (psx3d_profile_dirty_ && (s.vblank % 600u) == 0u)
                try_save_psx3d_profile();
        }
    }

    // Persist camera-root analysis only when it changed.
    if (psx3d_mode_mgr_.analysis_enabled() && cpu_)
    {
        const uint64_t cam_serial = cpu_->camera_candidates_serial();
        if (cam_serial != psx3d_cam_serial_seen_)
        {
            psx3d_cam_serial_seen_ = cam_serial;
            psx3d_profile_dirty_ = true;
        }
    }

    // Generic fallback monitor: detect abnormal 3D->2D collapse and trigger analysis refresh.
    if (psx3d_mode_mgr_.analysis_enabled())
    {
        const uint32_t gframe = gpu_3d_.frame_count();
        if (gframe != last_gpu3d_frame_seen_)
        {
            last_gpu3d_frame_seen_ = gframe;
            const uint32_t tris = gpu_3d_.dbg_last_tris_;
            const uint32_t n3d = gpu_3d_.dbg_last_3d_;
            const uint32_t n2d = gpu_3d_.dbg_last_2d_;
            if (tris >= 128u && n2d > 0)
            {
                const float fallback_ratio = (float)n2d / (float)tris;
                const bool cooldown_ok =
                    (gframe > last_gpu3d_refresh_frame_) &&
                    ((gframe - last_gpu3d_refresh_frame_) >= 120u);
                bool has_unknown_nohint_pc = false;
                for (uint32_t pc : psx3d_last_nohint_top_pcs_)
                {
                    if (pc == 0)
                        continue;
                    if (psx3d_analyzed_pcs_.find(pc) == psx3d_analyzed_pcs_.end())
                    {
                        has_unknown_nohint_pc = true;
                        break;
                    }
                }
                if (fallback_ratio >= 0.20f && cooldown_ok && has_unknown_nohint_pc)
                {
                    char scope[96];
                    std::snprintf(
                        scope,
                        sizeof(scope),
                        "fallback2d f=%u 3d=%u 2d=%u nh=%u stale=%u dec=%u",
                        gframe,
                        n3d,
                        n2d,
                        gpu_3d_.dbg_last_miss_no_hint_,
                        gpu_3d_.dbg_last_miss_hint_stale_,
                        gpu_3d_.dbg_last_miss_decode_fail_);
                    request_psx3d_analysis_refresh("high_fallback_2d", scope);
                    if (psx3d_mode_mgr_.mode() != Psx3dRunMode::analysis)
                        set_psx3d_mode(Psx3dRunMode::analysis);
                    last_gpu3d_refresh_frame_ = gframe;
                    emu::logf(
                        emu::LogLevel::warn,
                        "PSX3D",
                        "fallback monitor refresh frame=%u ratio=%.3f 3d=%u 2d=%u nh=%u stale=%u dec=%u",
                        gframe,
                        (double)fallback_ratio,
                        n3d,
                        n2d,
                        gpu_3d_.dbg_last_miss_no_hint_,
                        gpu_3d_.dbg_last_miss_hint_stale_,
                        gpu_3d_.dbg_last_miss_decode_fail_);
                }
            }
        }
    }

    // Fire per-instruction hooks (zero-cost when no hooks registered)
    if (hooks_.has_step())
        hooks_.fire_step(res.pc);

    // MILESTONE 1: BIOS → Shell/Game (PC goes from 0xBFCxxxxx to 0x800xxxxx)
    if (!milestones_.bios_to_shell_logged)
    {
        const uint32_t pc_after = res.pc;
        const bool was_bios = (pc_before >= 0xBFC00000u && pc_before < 0xC0000000u);
        const bool now_ram = (pc_after >= 0x80000000u && pc_after < 0x80200000u);
        if (was_bios && now_ram)
        {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(now - g_boot_start).count();
            milestones_.bios_to_shell_step = g_step_count;
            milestones_.bios_to_shell_time = elapsed_ms;
            milestones_.bios_to_shell_logged = true;
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "=== BIOS → SHELL/GAME: step=%llu time=%.1fms PC=0x%08X→0x%08X ===",
                (unsigned long long)g_step_count, elapsed_ms, pc_before, pc_after);
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "[DuckStation comparison: BIOS→Shell typically ~1800ms]");
        }
    }

    // MILESTONE 2: First GPU primitives (check GPU frame stats)
    if (!milestones_.first_gpu_prim_logged && milestones_.bios_to_shell_logged)
    {
        const auto& stats = gpu_.frame_stats();
        if (stats.triangles > 0 || stats.quads > 0 || stats.rects > 0)
        {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(now - g_boot_start).count();
            milestones_.first_gpu_prim_step = g_step_count;
            milestones_.first_gpu_prim_time = elapsed_ms;
            milestones_.first_gpu_prim_logged = true;
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "=== FIRST GPU PRIMITIVES: step=%llu time=%.1fms (tri=%u quad=%u rect=%u) ===",
                (unsigned long long)g_step_count, elapsed_ms,
                stats.triangles, stats.quads, stats.rects);
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "[DuckStation comparison: First logo ~2000ms, License text ~4500ms]");
        }
    }

    // MILESTONE 3: After ~200 frames with primitives (license likely done)
    if (!milestones_.license_end_logged && milestones_.first_gpu_prim_logged)
    {
        const uint32_t frame_count = gpu_.vram_frame_count();
        // Ridge Racer: license screen is around frame 150-180, game starts ~200
        if (frame_count >= 200)
        {
            const auto now = std::chrono::steady_clock::now();
            const double elapsed_ms = std::chrono::duration<double, std::milli>(now - g_boot_start).count();
            milestones_.license_end_step = g_step_count;
            milestones_.license_end_time = elapsed_ms;
            milestones_.license_end_logged = true;
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "=== LICENSE END (frame %u): step=%llu time=%.1fms ===",
                frame_count, (unsigned long long)g_step_count, elapsed_ms);
            emu::logf(emu::LogLevel::info, "MILESTONE",
                "[DuckStation comparison: Game starts ~8000-10000ms after boot]");
        }
    }

    return res;
}

uint32_t Core::pc() const
{
    return cpu_ ? cpu_->pc() : 0;
}

void Core::set_gpr(uint32_t idx, uint32_t v)
{
    if (cpu_)
        cpu_->set_gpr(idx, v);
}

void Core::set_pc(uint32_t v)
{
    if (cpu_)
        cpu_->set_pc(v);
}

r3000::Bus* Core::bus()
{
    return bus_.get();
}

r3000::Cpu* Core::cpu()
{
    return cpu_.get();
}

gte::Gte* Core::gte()
{
    return cpu_ ? &cpu_->gte() : nullptr;
}

void Core::set_pad_buttons(uint16_t v)
{
    // Diagnostic: log first non-idle call with address verification
    if (v != 0xFFFFu)
    {
        static uint32_t log_count = 0;
        if (log_count < 10)
        {
            ++log_count;
            emu::logf(emu::LogLevel::warn, "BUS",
                "set_pad(0x%04X) bus=%p g_pad=%p (#%u)",
                v, (void*)bus_.get(),
                bus_ ? r3000::Bus::pad_buttons_addr() : nullptr,
                log_count);
        }
    }
    if (bus_)
        bus_->set_pad_buttons(v);
}

void Core::set_cycle_multiplier(uint32_t n)
{
    if (cpu_)
        cpu_->set_cycle_multiplier(n);
}

uint32_t Core::last_cycles() const
{
    return cpu_ ? cpu_->last_cycles() : 1;
}

void Core::set_psx3d_mode(Psx3dRunMode m)
{
    psx3d_mode_mgr_.set_mode(m);
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "mode=%s analysis_enabled=%d",
        (m == Psx3dRunMode::analysis) ? "analysis" : "game",
        psx3d_mode_mgr_.analysis_enabled() ? 1 : 0);
}

void Core::set_psx3d_analysis_enabled(bool enabled)
{
    psx3d_mode_mgr_.set_analysis_enabled(enabled);
    if (cpu_)
        cpu_->set_camera_analysis_enabled(enabled ? 1 : 0);
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "analysis_enabled=%d mode=%s",
        enabled ? 1 : 0,
        (psx3d_mode_mgr_.mode() == Psx3dRunMode::analysis) ? "analysis" : "game");
}

uint32_t Core::request_psx3d_analysis_refresh(const char* reason, const char* scope)
{
    const std::string r = reason ? reason : "";
    const std::string s = scope ? scope : "";
    const uint32_t id = psx3d_mode_mgr_.request_refresh(r, s);
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "refresh queued id=%u reason=%s scope=%s",
        id,
        r.empty() ? "(none)" : r.c_str(),
        s.empty() ? "(none)" : s.c_str());
    return id;
}

void Core::run_psx3d_analysis_refresh(const Psx3dRefreshRequest& req)
{
    uint32_t added = 0;
    if (req.scope.rfind("pc=0x", 0) == 0)
    {
        uint32_t pc = 0;
        if (std::sscanf(req.scope.c_str(), "pc=0x%X", &pc) == 1 && pc != 0)
        {
            if (psx3d_analyzed_pcs_.insert(pc).second)
                ++added;
            provenance_profiler_.ack_refresh(pc, last_vblank_seen_);
        }
    }
    else if (req.scope == "global" || req.scope.empty())
    {
        const auto snaps = provenance_profiler_.snapshot();
        for (const auto& s : snaps)
        {
            if (s.pc == 0)
                continue;
            if (psx3d_analyzed_pcs_.insert(s.pc).second)
                ++added;
            provenance_profiler_.ack_refresh(s.pc, last_vblank_seen_);
        }
    }
    else
    {
        // Fallback scopes (ex: fallback2d ...) still trigger a useful refresh:
        // first learn current-frame DMA2 nohint PCs, then fill from cumulative hotspots.
        uint32_t budget = 32;
        for (uint32_t pc : psx3d_last_nohint_top_pcs_)
        {
            if (budget == 0)
                break;
            if (pc == 0)
                continue;
            if (psx3d_analyzed_pcs_.insert(pc).second)
            {
                ++added;
                --budget;
            }
            provenance_profiler_.ack_refresh(pc, last_vblank_seen_);
        }

        auto snaps = provenance_profiler_.snapshot();
        std::sort(snaps.begin(), snaps.end(), [](const auto& a, const auto& b) {
            if (a.total_words != b.total_words)
                return a.total_words > b.total_words;
            return a.pc < b.pc;
        });
        for (const auto& s : snaps)
        {
            if (budget == 0)
                break;
            if (s.pc == 0)
                continue;
            if (psx3d_analyzed_pcs_.insert(s.pc).second)
            {
                ++added;
                --budget;
            }
            provenance_profiler_.ack_refresh(s.pc, last_vblank_seen_);
        }
    }

    if (added > 0)
        psx3d_profile_dirty_ = true;
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "analysis pass done id=%u reason=%s scope=%s added_pcs=%u total_analyzed=%u",
        req.id,
        req.reason.empty() ? "(none)" : req.reason.c_str(),
        req.scope.empty() ? "(none)" : req.scope.c_str(),
        added,
        (uint32_t)psx3d_analyzed_pcs_.size());
}

void Core::set_psx3d_profile_path_override(const char* path)
{
    if (!path || !*path)
        return;
    std::filesystem::path p(path);
    psx3d_profile_path_ = p.string();
    psx3d_profile_game_id_ = psx3d_sanitize_game_id(p.stem().string());
    if (psx3d_profile_game_id_.empty())
        psx3d_profile_game_id_ = "unknown";
    psx3d_profile_override_ = true;
    psx3d_profile_loaded_ = false;
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "profile override path=%s game=%s",
        psx3d_profile_path_.c_str(),
        psx3d_profile_game_id_.c_str());
    try_load_psx3d_profile();
}

void Core::set_psx3d_profile_identity_from_path(const char* path)
{
    if (psx3d_profile_override_)
        return;
    if (!path || !*path)
        return;
    std::filesystem::path p(path);
    std::string stem = psx3d_sanitize_game_id(p.stem().string());
    if (stem.empty())
        stem = "unknown";
    psx3d_profile_game_id_ = stem;
    psx3d_profile_path_ = Psx3dProfileStore::default_profile_path(stem);
    psx3d_profile_loaded_ = false;
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "profile identity game=%s path=%s",
        psx3d_profile_game_id_.c_str(),
        psx3d_profile_path_.c_str());
}

void Core::try_load_psx3d_profile()
{
    if (psx3d_profile_loaded_)
        return;
    if (psx3d_profile_path_.empty())
        return;

    Psx3dProfileData data{};
    if (!Psx3dProfileStore::load(psx3d_profile_path_, data))
    {
        psx3d_profile_loaded_ = true;
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile load miss path=%s",
            psx3d_profile_path_.c_str());
        return;
    }

    provenance_profiler_.restore(data.hotspots);
    psx3d_analyzed_pcs_.clear();
    for (uint32_t pc : data.analyzed_pcs)
    {
        if (pc != 0)
            psx3d_analyzed_pcs_.insert(pc);
    }
    if (cpu_)
    {
        std::vector<r3000::Cpu::CameraCandidateSnapshot> cams;
        cams.reserve(data.camera_candidates.size());
        for (const auto& c : data.camera_candidates)
        {
            r3000::Cpu::CameraCandidateSnapshot s{};
            s.addr = c.addr;
            s.hits = c.hits;
            s.frame_hits = c.frame_hits;
            s.first_vblank = c.first_vblank;
            s.last_vblank = c.last_vblank;
            s.last_pc = c.last_pc;
            s.gte_reg_mask = c.gte_reg_mask;
            cams.push_back(s);
        }
        cpu_->restore_camera_candidates(cams);
        psx3d_cam_serial_seen_ = cpu_->camera_candidates_serial();
    }
    psx3d_profile_loaded_ = true;
    psx3d_profile_dirty_ = false;
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "profile loaded path=%s hotspots=%u analyzed=%u cam=%u game=%s",
        psx3d_profile_path_.c_str(),
        (uint32_t)data.hotspots.size(),
        (uint32_t)psx3d_analyzed_pcs_.size(),
        (uint32_t)data.camera_candidates.size(),
        data.game_id.empty() ? "(none)" : data.game_id.c_str());
}

void Core::try_save_psx3d_profile()
{
    if (psx3d_profile_path_.empty())
        return;
    if (!psx3d_profile_dirty_)
        return;

    Psx3dProfileData data{};
    data.game_id = psx3d_profile_game_id_.empty() ? "unknown" : psx3d_profile_game_id_;
    data.hotspots = provenance_profiler_.snapshot();
    data.analyzed_pcs.reserve(psx3d_analyzed_pcs_.size());
    for (uint32_t pc : psx3d_analyzed_pcs_)
        data.analyzed_pcs.push_back(pc);
    if (cpu_)
    {
        const auto cams = cpu_->camera_candidates_snapshot();
        data.camera_candidates.reserve(cams.size());
        for (const auto& s : cams)
        {
            Psx3dProfileData::CameraCandidate c{};
            c.addr = s.addr;
            c.hits = s.hits;
            c.frame_hits = s.frame_hits;
            c.first_vblank = s.first_vblank;
            c.last_vblank = s.last_vblank;
            c.last_pc = s.last_pc;
            c.gte_reg_mask = s.gte_reg_mask;
            data.camera_candidates.push_back(c);
        }
    }
    if (Psx3dProfileStore::save(psx3d_profile_path_, data))
    {
        psx3d_profile_dirty_ = false;
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile saved path=%s hotspots=%u analyzed=%u cam=%u",
            psx3d_profile_path_.c_str(),
            (uint32_t)data.hotspots.size(),
            (uint32_t)data.analyzed_pcs.size(),
            (uint32_t)data.camera_candidates.size());
    }
    else
    {
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile save failed path=%s",
            psx3d_profile_path_.c_str());
    }
}

bool Core::fast_boot_from_cd(char* err, size_t err_cap)
{
    if (!cpu_ || !bus_ || !ram_)
    {
        set_err(err, err_cap, "core not initialized");
        return false;
    }

    // 1. Read SYSTEM.CNF from the CD
    // Diagnostic: try reading PVD sector (16) directly
    {
        uint8_t pvd[2048]{};
        bool pvd_ok = cdrom_.read_sector_2048(16, pvd);
        emu::logf(emu::LogLevel::info, "CORE", "PVD sector 16 read: %s, magic=%c%c%c%c%c type=%d",
            pvd_ok ? "ok" : "FAIL",
            pvd[1], pvd[2], pvd[3], pvd[4], pvd[5], pvd[0]);
    }

    uint32_t cnf_lba = 0, cnf_size = 0;
    if (!cdrom_.iso9660_find_file("\\SYSTEM.CNF;1", &cnf_lba, &cnf_size))
    {
        set_err(err, err_cap, "SYSTEM.CNF not found on disc");
        return false;
    }

    // Read SYSTEM.CNF (usually < 2048 bytes)
    uint8_t cnf_buf[2048]{};
    if (!cdrom_.read_sector_2048(cnf_lba, cnf_buf))
    {
        set_err(err, err_cap, "failed to read SYSTEM.CNF sector");
        return false;
    }

    // Parse "BOOT = cdrom:\\<filename>;1" line
    char boot_file[128]{};
    {
        const char* cnf = reinterpret_cast<const char*>(cnf_buf);
        const char* p = std::strstr(cnf, "BOOT");
        if (!p)
        {
            set_err(err, err_cap, "BOOT entry not found in SYSTEM.CNF");
            return false;
        }
        // Skip "BOOT" and any spaces/= characters
        p += 4;
        while (*p == ' ' || *p == '\t' || *p == '=') ++p;
        // Skip "cdrom:" or "cdrom:\\" prefix
        if (std::strncmp(p, "cdrom:", 6) == 0) p += 6;
        while (*p == '\\') ++p;

        size_t i = 0;
        while (*p && *p != '\r' && *p != '\n' && *p != ';' && i < sizeof(boot_file) - 2)
            boot_file[i++] = *p++;
        boot_file[i] = '\0';
    }

    if (!boot_file[0])
    {
        set_err(err, err_cap, "empty BOOT filename in SYSTEM.CNF");
        return false;
    }

    emu::logf(emu::LogLevel::info, "CORE", "Fast boot: loading %s from CD", boot_file);

    // 2. Find the EXE file on disc
    char iso_path[140]{};
    std::snprintf(iso_path, sizeof(iso_path), "\\%s;1", boot_file);
    uint32_t exe_lba = 0, exe_size = 0;
    if (!cdrom_.iso9660_find_file(iso_path, &exe_lba, &exe_size))
    {
        set_err(err, err_cap, "boot EXE not found on disc");
        return false;
    }

    emu::logf(emu::LogLevel::info, "CORE", "EXE found: LBA=%u size=%u", exe_lba, exe_size);

    // 3. Read the full EXE into a temp buffer
    const uint32_t sector_count = (exe_size + 2047) / 2048;
    std::unique_ptr<uint8_t[]> exe_buf(new (std::nothrow) uint8_t[sector_count * 2048]{});
    if (!exe_buf)
    {
        set_err(err, err_cap, "out of memory for EXE");
        return false;
    }
    for (uint32_t s = 0; s < sector_count; ++s)
    {
        if (!cdrom_.read_sector_2048(exe_lba + s, exe_buf.get() + s * 2048))
        {
            set_err(err, err_cap, "failed to read EXE sector from disc");
            return false;
        }
    }

    // 4. Parse PS-X EXE header
    if (exe_size < 0x800 || std::memcmp(exe_buf.get(), "PS-X EXE", 8) != 0)
    {
        set_err(err, err_cap, "boot file is not a valid PS-X EXE");
        return false;
    }

    auto read_u32_le = [](const uint8_t* p) -> uint32_t {
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    };

    const uint32_t pc0 = read_u32_le(exe_buf.get() + 0x10);
    const uint32_t gp0 = read_u32_le(exe_buf.get() + 0x14);
    const uint32_t t_addr = read_u32_le(exe_buf.get() + 0x18);
    const uint32_t t_size = read_u32_le(exe_buf.get() + 0x1C);
    const uint32_t b_addr = read_u32_le(exe_buf.get() + 0x28);
    const uint32_t b_size = read_u32_le(exe_buf.get() + 0x2C);
    const uint32_t sp_addr = read_u32_le(exe_buf.get() + 0x30);
    const uint32_t sp_size = read_u32_le(exe_buf.get() + 0x34);

    // Convert KSEG0/KSEG1 to physical
    auto virt_to_phys = [](uint32_t v) -> uint32_t {
        return v & 0x1FFF'FFFFu;
    };

    const uint32_t t_phys = virt_to_phys(t_addr);
    if ((uint64_t)t_phys + t_size > ram_size_)
    {
        set_err(err, err_cap, "EXE text segment exceeds RAM");
        return false;
    }

    // 5. Copy text segment to RAM
    std::memcpy(ram_.get() + t_phys, exe_buf.get() + 0x800, t_size);
    emu::logf(emu::LogLevel::info, "CORE", "Loaded text: 0x%08X -> phys 0x%08X (%u bytes)", t_addr, t_phys, t_size);
    emu::logf(emu::LogLevel::info, "CORE", "EXE header: BSS=0x%08X size=%u, SP=0x%08X size=%u",
        b_addr, b_size, sp_addr, sp_size);

    // Zero BSS segment (fast_boot was missing this!)
    if (b_size != 0)
    {
        const uint32_t b_phys = virt_to_phys(b_addr);
        if ((uint64_t)b_phys + b_size <= ram_size_)
        {
            std::memset(ram_.get() + b_phys, 0, b_size);
            emu::logf(emu::LogLevel::info, "CORE", "Zeroed BSS: 0x%08X -> phys 0x%08X (%u bytes)", b_addr, b_phys, b_size);
        }
    }

    // Diagnostic: check if 0x8007BCF4 is in loaded range
    {
        const uint32_t probe = 0x0007'BCF4u; // physical addr of 0x8007BCF4
        bool in_text = (probe >= t_phys && probe < t_phys + t_size);
        bool in_bss = (b_size != 0 && probe >= virt_to_phys(b_addr) && probe < virt_to_phys(b_addr) + b_size);
        emu::logf(emu::LogLevel::info, "CORE", "Probe 0x8007BCF4: in_text=%d in_bss=%d byte=0x%02X%02X%02X%02X",
            in_text, in_bss,
            ram_.get()[probe], ram_.get()[probe+1], ram_.get()[probe+2], ram_.get()[probe+3]);
    }

    // 6. Set CPU state
    cpu_->set_pc(pc0);
    cpu_->set_gpr(28, gp0); // GP
    if (sp_size != 0)
        cpu_->set_gpr(29, sp_addr + sp_size); // SP = stack base + size
    else if (sp_addr != 0)
        cpu_->set_gpr(29, sp_addr); // SP = sp_addr when size=0
    else
        cpu_->set_gpr(29, 0x801F'FF00u); // default SP

    // 7. Initialize minimal hardware state for game code

    // Enable HLE vectors so A0/B0/C0 calls + exception vector are intercepted
    cpu_->set_hle_vectors(1);

    // GPU generates real VBlanks at ~50Hz. Disable HLE pseudo-vblank (~333Hz)
    // which would corrupt game timing if both fire.
    cpu_->set_use_gpu_vblank(1);

    // Set I_MASK for VBLANK + CDROM + DMA
    {
        r3000::Bus::MemFault mf{};
        bus_->write_u32(0x1F80'1074u, 0x000Du, mf); // VBLANK(0) + CDROM(2) + DMA(3)
    }

    // Set COP0 Status: IEc=1, IM2=1, IM0=1 (enable hardware + software interrupts)
    cpu_->set_cop0(12, (1u << 0) | (1u << 8) | (1u << 10)); // SR = 0x00000501

    // Initialize kernel data structures (PCB/TCB) so BIOS calls work
    {
        constexpr uint32_t kPcbAddr = 0x0200u;
        constexpr uint32_t kTcbAddr = 0x0300u;
        constexpr uint32_t kTcbSize = 0xC0u;

        std::memset(ram_.get() + kPcbAddr, 0, 0x10);
        std::memset(ram_.get() + kTcbAddr, 0, kTcbSize);

        // TCB[0x00] = 0x4000 (active flag)
        auto w32 = [&](uint32_t addr, uint32_t val) {
            std::memcpy(ram_.get() + addr, &val, 4);
        };
        w32(kTcbAddr, 0x4000u);

        // TCB[0x94] = saved Status with IEp=1 (bit 2) + IM2=1 (bit 10)
        w32(kTcbAddr + 0x94, (1u << 2) | (1u << 10));

        // PCB[0] = pointer to TCB (KSEG0)
        w32(kPcbAddr, 0x80000000u | kTcbAddr);

        // [0x108] = PCB address (KSEG0)
        w32(0x108, 0x80000000u | kPcbAddr);

        cpu_->set_hle_tcb_addr(kTcbAddr);

        emu::logf(emu::LogLevel::info, "CORE", "Kernel data: PCB=0x%X TCB=0x%X [0x108]=0x%08X",
            kPcbAddr, kTcbAddr, 0x80000000u | kPcbAddr);
    }

    // Debug: watch writes to 0x8007BCF4 (filename buffer)
    bus_->set_watch_ram_u32(0x0007'BCF4u, 1);

    emu::logf(emu::LogLevel::info, "CORE", "Fast boot: PC=0x%08X GP=0x%08X SP=0x%08X", pc0, gp0, cpu_->gpr(29));
    return true;
}

// ---------------------------------------------------------------------------
// Dev kit boot: load PS-EXE from file + HLE kernel init
// ---------------------------------------------------------------------------
bool Core::fast_boot_from_exe(const char* exe_path, char* err, size_t err_cap)
{
    if (!cpu_ || !bus_ || !ram_)
    {
        set_err(err, err_cap, "core not initialized");
        return false;
    }
    set_psx3d_profile_identity_from_path(exe_path);
    try_load_psx3d_profile();

    // 1. Load EXE file into RAM
    loader::LoadedImage img{};
    if (!loader::load_file_into_ram(exe_path, loader::Format::auto_detect,
            ram_.get(), ram_size_, &img, err, err_cap))
    {
        return false; // err already set by loader
    }

    // 2. Set CPU state from EXE header
    cpu_->set_pc(img.entry_pc);
    if (img.has_gp)
        cpu_->set_gpr(28, img.gp);
    if (img.has_sp)
        cpu_->set_gpr(29, img.sp);
    else
        cpu_->set_gpr(29, 0x801F'FF00u);

    // 3. Initialize minimal hardware state (same as fast_boot_from_cd)

    cpu_->set_hle_vectors(1);
    cpu_->set_use_gpu_vblank(1);

    // I_MASK: VBLANK(0) + DMA(3)
    {
        r3000::Bus::MemFault mf{};
        bus_->write_u32(0x1F80'1074u, 0x0009u, mf); // bits 0+3
    }

    // COP0 Status: IEc=1, IM2=1, IM0=1
    cpu_->set_cop0(12, (1u << 0) | (1u << 8) | (1u << 10));

    // PCB/TCB kernel structures
    {
        constexpr uint32_t kPcbAddr = 0x0200u;
        constexpr uint32_t kTcbAddr = 0x0300u;
        constexpr uint32_t kTcbSize = 0xC0u;

        std::memset(ram_.get() + kPcbAddr, 0, 0x10);
        std::memset(ram_.get() + kTcbAddr, 0, kTcbSize);

        auto w32 = [&](uint32_t addr, uint32_t val) {
            std::memcpy(ram_.get() + addr, &val, 4);
        };
        w32(kTcbAddr, 0x4000u);
        w32(kTcbAddr + 0x94, (1u << 2) | (1u << 10));
        w32(kPcbAddr, 0x80000000u | kTcbAddr);
        w32(0x108, 0x80000000u | kPcbAddr);

        cpu_->set_hle_tcb_addr(kTcbAddr);
    }

    emu::logf(emu::LogLevel::info, "CORE", "Dev kit boot: %s PC=0x%08X SP=0x%08X",
        exe_path, img.entry_pc, cpu_->gpr(29));
    return true;
}

} // namespace emu

