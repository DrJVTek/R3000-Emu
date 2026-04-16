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
#include "../mdec/mdec.h"

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

static void on_psx3d_vblank_hook(uint32_t vblank_count, void* user)
{
    if (!user)
        return;
    auto* core = reinterpret_cast<Core*>(user);
    core->on_psx3d_vblank(vblank_count);
}

static void on_psx3d_step_hook(uint32_t pc, void* user)
{
    if (!user)
        return;
    auto* core = reinterpret_cast<Core*>(user);
    core->on_psx3d_step_pc(pc);
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
    emu::logf(emu::LogLevel::debug, "CORE", "Core created");
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

static std::string psx3d_normalize_boot_game_id(const char* boot_name)
{
    if (!boot_name || !*boot_name)
        return {};

    std::string s(boot_name);
    for (char& c : s)
    {
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    }

    const size_t slash = s.find_last_of("\\/");
    std::string stem = (slash == std::string::npos) ? s : s.substr(slash + 1);
    const size_t semi = stem.find(';');
    if (semi != std::string::npos)
        stem.resize(semi);
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();

    // Strip well-known PS-X executable file extensions for devkit / --load
    // mode. We do this AFTER the trailing-dot strip and BEFORE the canonical
    // SCUS detection so that:
    //   - TREX.EXE   → TREX        (devkit)
    //   - SLES_004.69 → SLES-004.69 (CD boot, kept by canonical match below)
    // The canonical SCUS form has digits after the dot, never letters, so
    // there is no risk of accidentally matching the extension strip.
    auto ends_with_ci = [](const std::string& haystack, const char* suffix) -> bool {
        const size_t hl = haystack.size();
        const size_t sl = std::strlen(suffix);
        if (sl > hl) return false;
        for (size_t i = 0; i < sl; ++i)
            if (haystack[hl - sl + i] != suffix[i])  // both already upper-case
                return false;
        return true;
    };
    static const char* const kExeExtensions[] = { ".EXE", ".PSX", ".PS-X", ".PS1", ".PSEXE" };
    for (const char* ext : kExeExtensions)
    {
        if (ends_with_ci(stem, ext))
        {
            stem.resize(stem.size() - std::strlen(ext));
            break;
        }
    }
    while (!stem.empty() && (stem.back() == '.' || stem.back() == ' '))
        stem.pop_back();

    // Canonical PSX executable codes are typically of the form:
    //   SCUS_943.00  -> SCUS-943.00
    //   SLUS_000.00  -> SLUS-000.00
    //   SCES_123.45  -> SCES-123.45
    if (stem.size() >= 11 &&
        ((stem[0] >= 'A' && stem[0] <= 'Z') &&
         (stem[1] >= 'A' && stem[1] <= 'Z') &&
         (stem[2] >= 'A' && stem[2] <= 'Z') &&
         (stem[3] >= 'A' && stem[3] <= 'Z')) &&
        stem[4] == '_' &&
        (stem[5] >= '0' && stem[5] <= '9') &&
        (stem[6] >= '0' && stem[6] <= '9') &&
        (stem[7] >= '0' && stem[7] <= '9') &&
        stem[8] == '.' &&
        (stem[9] >= '0' && stem[9] <= '9') &&
        (stem[10] >= '0' && stem[10] <= '9'))
    {
        stem[4] = '-';
        return psx3d_sanitize_game_id(stem);
    }

    return psx3d_sanitize_game_id(stem);
}

Core::~Core()
{
    try_save_psx3d_profile();
    delete mdec_;
    mdec_ = nullptr;
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
        clear_psx3d_profile_identity();
        char boot_file[128]{};
        uint32_t cnf_lba = 0, cnf_size = 0;
        uint8_t cnf_buf[2048]{};
        if (cdrom_.iso9660_find_file("\\SYSTEM.CNF;1", &cnf_lba, &cnf_size) &&
            cnf_lba != 0 &&
            cdrom_.read_sector_2048(cnf_lba, cnf_buf))
        {
            const char* cnf = reinterpret_cast<const char*>(cnf_buf);
            const char* p = std::strstr(cnf, "BOOT");
            if (p)
            {
                p += 4;
                while (*p == ' ' || *p == '\t' || *p == '=') ++p;
                if (std::strncmp(p, "cdrom:", 6) == 0)
                    p += 6;
                while (*p == '\\')
                    ++p;

                size_t i = 0;
                while (*p && *p != '\r' && *p != '\n' && *p != ';' && i < sizeof(boot_file) - 1)
                    boot_file[i++] = *p++;
                boot_file[i] = '\0';
            }
        }
        if (boot_file[0])
        {
            set_psx3d_profile_identity_from_game_id(boot_file);
            try_load_psx3d_profile();
        }
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
    // Register CDROM IRQ callback so edge detection is immediate (like DuckStation's
    // SetLineState) instead of polled. Without this, fast IRQ ack+re-fire sequences
    // (e.g., consecutive sector deliveries in STR streaming) miss the 0→1 edge.
    cdrom_.set_irq_callback([](int irq_state, void* user) {
        auto* bus = static_cast<r3000::Bus*>(user);
        bus->check_cdrom_irq_edge();
    }, bus_.get());
    // Connect CDROM XA-ADPCM output to SPU
    if (bus_->spu())
        cdrom_.set_spu(bus_->spu());
    cpu_.reset(new (std::nothrow) r3000::Cpu(*bus_, logger_));
    if (!cpu_)
    {
        set_err(err, err_cap, "out of memory");
        return false;
    }
    cpu_->set_gte_backend_kind(opt.gte_backend);

    // Hook system: pass hooks to Bus for VBlank/write dispatch.
    bus_->set_hooks(&hooks_);
    if (!psx3d_hooks_registered_)
    {
        const int vblank_idx = hooks_.add_vblank(&on_psx3d_vblank_hook, this);
        const int step_idx = hooks_.add_step(&on_psx3d_step_hook, this);
        psx3d_hooks_registered_ = (vblank_idx >= 0 && step_idx >= 0);
        if (!psx3d_hooks_registered_)
        {
            emu::logf(
                emu::LogLevel::warn,
                "PSX3D",
                "hook registration failed vblank_idx=%d step_idx=%d",
                vblank_idx,
                step_idx);
        }
    }

    // Shadow GTE/GPU for 3D tag-based reconstruction.
    gpu_3d_.bind_gte_3d(&gte_3d_);
    gpu_3d_.bind_bus(bus_.get());
    bus_->set_gpu_3d(&gpu_3d_);
    bus_->set_gte_3d(&gte_3d_);
    cpu_->set_gte_shadow(&gte_3d_);

    // Bus tracing options (diagnostic only).
    if (has_clock_)
        bus_->set_trace_vector_sink(iolog_, clock_);
    bus_->set_trace_vectors(opt.trace_vectors ? 1 : 0);
    if (opt.watch_u32_enabled)
        bus_->set_watch_ram_u32(opt.watch_u32_phys, 1);
    bus_->set_watch_ram_range(opt.watch_ram_range_phys, opt.watch_ram_range_size, opt.watch_ram_range_size ? 1 : 0);
    if (opt.watch_ram_range_size != 0)
    {
        emu::logf(
            emu::LogLevel::warn,
            "RAMWATCH",
            "armed phys=[0x%08X..0x%08X) size=0x%X",
            opt.watch_ram_range_phys,
            opt.watch_ram_range_phys + opt.watch_ram_range_size,
            opt.watch_ram_range_size);
    }
    bus_->set_stack_watch(
        opt.stack_watch_phys,
        opt.stack_watch_size,
        opt.stack_watch_enabled,
        opt.stack_watch_target_enabled,
        opt.stack_watch_target_value);
    if (opt.stack_watch_enabled && opt.stack_watch_size != 0)
    {
        emu::logf(
            emu::LogLevel::warn,
            "STACKWATCH",
            "armed phys=0x%08X size=0x%X target_enabled=%u target_value=0x%08X",
            opt.stack_watch_phys,
            opt.stack_watch_size,
            (unsigned)opt.stack_watch_target_enabled,
            opt.stack_watch_target_value);
    }

    cpu_->reset(img.entry_pc);

    cpu_->set_pretty(opt.pretty ? 1 : 0);
    cpu_->set_trace_io(opt.trace_io ? 1 : 0);
    cpu_->set_hle_vectors(opt.hle_vectors ? 1 : 0);
    cpu_->set_text_hle(opt.text_hle ? 1 : 0);
    init_hle_vectors_ = opt.hle_vectors ? 1 : 0;
    init_text_hle_ = opt.text_hle ? 1 : 0;
    cpu_->set_crash_trace_steps(opt.crash_trace_steps);

    // GPU generates real VBlanks at ~50Hz. Disable HLE pseudo-vblank (~333Hz).
    cpu_->set_use_gpu_vblank(1);

    cpu_->set_loop_detectors(opt.loop_detectors ? 1 : 0);
    cpu_->set_bus_tick_batch(opt.bus_tick_batch);
    // Create the real MDEC decoder once and keep it installed on the bus.
    if (!mdec_)
    {
        mdec_ = new mdec::Mdec();
        bus_->set_mdec(mdec_);
    }
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
    const auto cd_timing_mode =
        (opt.cd_timing_mode == 0)
            ? cdrom::Cdrom::TimingMode::Realistic
            : cdrom::Cdrom::TimingMode::CompatibilityFast;
    cdrom_.set_timing_mode(cd_timing_mode);
    emu::logf(
        emu::LogLevel::warn,
        "CORE",
        "CD timing mode: %s (realistic = fidelity target, compatibility-fast = temporary fallback)",
        (cd_timing_mode == cdrom::Cdrom::TimingMode::Realistic) ? "realistic" : "compatibility-fast");

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
    psx3d_step_hook_pcs_.clear();
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

uint64_t Core::steps() const
{
    return g_step_count;
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

void Core::refresh_psx3d_step_hook_hotspots(const r3000::Bus::Dma2NoHintSummary& s)
{
    std::vector<uint32_t> next;
    next.reserve(kMaxHooks);
    for (const auto& p : s.top_pcs)
    {
        if (next.size() >= (size_t)kMaxHooks)
            break;
        const uint32_t pc = p.first;
        if (pc == 0)
            continue;
        if (psx3d_analyzed_pcs_.find(pc) != psx3d_analyzed_pcs_.end())
            continue;
        next.push_back(pc);
    }
    psx3d_step_hook_pcs_.swap(next);
}

void Core::on_psx3d_vblank(uint32_t /*vblank_count*/)
{
    if (!psx3d_mode_mgr_.analysis_enabled() || !bus_)
        return;

    r3000::Bus::Dma2NoHintSummary s{};
    if (!bus_->consume_dma2_nohint_summary(s))
        return;

    psx3d_last_nohint_top_pcs_.clear();
    psx3d_last_nohint_top_pcs_.reserve(s.top_pcs.size());
    for (const auto& p : s.top_pcs)
    {
        if (p.first != 0)
            psx3d_last_nohint_top_pcs_.push_back(p.first);
    }
    refresh_psx3d_step_hook_hotspots(s);

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
        provenance_profiler_.ack_refresh(d.pc, s.vblank);
    }

    if (psx3d_profile_dirty_ && (s.vblank % 600u) == 0u)
        try_save_psx3d_profile();
}

void Core::on_psx3d_step_pc(uint32_t pc)
{
    if (!psx3d_mode_mgr_.analysis_enabled())
        return;
    if (psx3d_mode_mgr_.mode() == Psx3dRunMode::analysis)
        return;
    if (psx3d_mode_mgr_.has_refresh_request())
        return;
    if (psx3d_analyzed_pcs_.find(pc) != psx3d_analyzed_pcs_.end())
        return;

    bool watched = false;
    for (uint32_t wpc : psx3d_step_hook_pcs_)
    {
        if (wpc == pc)
        {
            watched = true;
            break;
        }
    }
    if (!watched)
        return;

    set_psx3d_mode(Psx3dRunMode::analysis);
    char scope[64];
    std::snprintf(scope, sizeof(scope), "pc=0x%08X", pc);
    request_psx3d_analysis_refresh("hook_breakpoint", scope);
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "hook breakpoint refresh pc=0x%08X",
        pc);
}

void Core::set_psx3d_profile_path_override(const char* path)
{
    if (!path || !*path)
        return;
    std::filesystem::path p(path);
    const std::string ext = p.extension().string();
    if (ext.empty())
    {
        psx3d_profile_root_dir_ = p.string();
        psx3d_profile_override_ = false;
        psx3d_profile_path_.clear();
        psx3d_profile_loaded_ = false;
        gpu_3d_.clear_runtime_mode_rules();
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile root override dir=%s",
            psx3d_profile_root_dir_.c_str());
        if (!psx3d_profile_game_id_.empty())
        {
            psx3d_profile_path_ =
                Psx3dProfileStore::default_profile_path(psx3d_profile_root_dir_, psx3d_profile_game_id_);
            try_load_psx3d_profile();
        }
        return;
    }

    psx3d_profile_root_dir_.clear();
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

void Core::clear_psx3d_profile_identity()
{
    if (psx3d_profile_override_)
        return;
    psx3d_profile_game_id_.clear();
    psx3d_profile_path_.clear();
    psx3d_profile_loaded_ = false;
    gpu_3d_.clear_runtime_mode_rules();
}

void Core::set_psx3d_profile_identity_from_game_id(const char* game_id)
{
    if (psx3d_profile_override_)
        return;
    const std::string id = psx3d_normalize_boot_game_id(game_id);
    psx3d_profile_game_id_ = id.empty() ? "unknown" : id;
    psx3d_profile_path_ = psx3d_profile_root_dir_.empty()
        ? Psx3dProfileStore::default_profile_path(psx3d_profile_game_id_)
        : Psx3dProfileStore::default_profile_path(psx3d_profile_root_dir_, psx3d_profile_game_id_);
    psx3d_profile_loaded_ = false;
    emu::logf(
        emu::LogLevel::warn,
        "PSX3D",
        "profile identity game=%s path=%s",
        psx3d_profile_game_id_.c_str(),
        psx3d_profile_path_.c_str());
}

void Core::set_psx3d_profile_identity_from_path(const char* path)
{
    if (psx3d_profile_override_)
        return;
    if (!path || !*path)
        return;
    std::filesystem::path p(path);
    std::string stem = psx3d_normalize_boot_game_id(p.filename().string().c_str());
    if (stem.empty())
        stem = "unknown";
    psx3d_profile_game_id_ = stem;
    psx3d_profile_path_ = psx3d_profile_root_dir_.empty()
        ? Psx3dProfileStore::default_profile_path(stem)
        : Psx3dProfileStore::default_profile_path(psx3d_profile_root_dir_, stem);
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
        gpu_3d_.clear_runtime_mode_rules();
        psx3d_profile_loaded_ = true;
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile load miss path=%s",
            psx3d_profile_path_.c_str());
        return;
    }

    provenance_profiler_.restore(data.hotspots);
    {
        std::vector<gpu::Gpu3D::RuntimeModeRule> runtime_rules;
        runtime_rules.reserve(data.mode_rules.size());
        for (const auto& rule : data.mode_rules)
        {
            gpu::Gpu3D::RuntimeModeRule rr{};
            switch (rule.mode)
            {
            case Psx3dProfileData::ModeKind::paired_edge_rtpt_gt4:
                rr.mode = gpu::Gpu3D::RuntimeModeKind::paired_edge_rtpt_gt4;
                break;
            case Psx3dProfileData::ModeKind::subdivided_ft4_intpl_rtpt:
                rr.mode = gpu::Gpu3D::RuntimeModeKind::subdivided_ft4_intpl_rtpt;
                break;
            default:
                rr.mode = gpu::Gpu3D::RuntimeModeKind::unknown;
                break;
            }
            switch (rule.link_rule)
            {
            case Psx3dProfileData::LinkRule::packet_edge_pairs:
                rr.link_rule = gpu::Gpu3D::RuntimeLinkRule::packet_edge_pairs;
                break;
            default:
                rr.link_rule = gpu::Gpu3D::RuntimeLinkRule::unknown;
                break;
            }
            rr.priority = rule.priority;
            rr.producer_pc_ranges.reserve(rule.producer_pc_ranges.size());
            for (const auto& r : rule.producer_pc_ranges)
                rr.producer_pc_ranges.push_back({r.start, r.end});
            rr.gte_pc_ranges.reserve(rule.gte_pc_ranges.size());
            for (const auto& r : rule.gte_pc_ranges)
                rr.gte_pc_ranges.push_back({r.start, r.end});
            rr.ot_fill_pc_ranges.reserve(rule.ot_fill_pc_ranges.size());
            for (const auto& r : rule.ot_fill_pc_ranges)
                rr.ot_fill_pc_ranges.push_back({r.start, r.end});
            runtime_rules.push_back(std::move(rr));
        }
        gpu_3d_.set_runtime_mode_rules(runtime_rules);
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile loaded path=%s game=%s modes=%u hotspots=%u analyzed=%u cam=%u",
            psx3d_profile_path_.c_str(),
            data.game_id.c_str(),
            (unsigned)runtime_rules.size(),
            (unsigned)data.hotspots.size(),
            (unsigned)data.analyzed_pcs.size(),
            (unsigned)data.camera_candidates.size());
    }
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

    // Propagate render quirks to the GTE. Each quirk is set unconditionally
    // (true OR false) so that loading a profile with the quirk explicitly
    // disabled overrides any previous toggle that may have been left on
    // (e.g. via CLI flag or UE5 UPROPERTY at boot).
    if (cpu_)
        cpu_->gte().set_force_geom_offset_zero(data.quirks.force_gte_geom_offset_zero);
    gte_3d_.set_force_geom_offset_zero(data.quirks.force_gte_geom_offset_zero);
    if (data.quirks.force_gte_geom_offset_zero)
    {
        emu::logf(
            emu::LogLevel::warn,
            "PSX3D",
            "profile quirk applied: force_gte_geom_offset_zero=1 (game=%s)",
            data.game_id.empty() ? "(none)" : data.game_id.c_str());
    }

    psx3d_profile_data_ = data;  // keep for live edit + save round-trip
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
    // Preserve static per-game mode rules already authored in the profile.
    // Runtime saves should refresh learned hotspots/analyzed PCs/camera data,
    // not erase the mode configuration.
    Psx3dProfileStore::load(psx3d_profile_path_, data);
    data.game_id = psx3d_profile_game_id_.empty() ? "unknown" : psx3d_profile_game_id_;
    data.hotspots = provenance_profiler_.snapshot();
    // Quirks: prefer the in-memory version (which may have been mutated via
    // devkit live-edit) over the freshly-reloaded disk values.
    data.quirks = psx3d_profile_data_.quirks;
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

    set_psx3d_profile_identity_from_game_id(boot_file);
    try_load_psx3d_profile();

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

    // Respect the init mode chosen by the host. Fast boot should not silently
    // switch the core to HLE if the session explicitly requested non-HLE.
    cpu_->set_hle_vectors(init_hle_vectors_ ? 1 : 0);
    cpu_->set_text_hle(init_text_hle_ ? 1 : 0);

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

    emu::logf(emu::LogLevel::info, "CORE", "Fast boot: PC=0x%08X GP=0x%08X SP=0x%08X hle=%d text_hle=%d",
        pc0, gp0, cpu_->gpr(29), init_hle_vectors_, init_text_hle_);
    return true;
}

void Core::set_psx_params(const int32_t* params, uint32_t count)
{
    psx_params_.clear();
    if (params && count > 0)
    {
        psx_params_.assign(params, params + count);
        emu::logf(emu::LogLevel::info, "CORE", "PSX params set: %u ints", count);
    }
}

// ---------------------------------------------------------------------------
// Dev kit boot:
// - devkit: direct EXE boot with minimal hardware bootstrap, no forced HLE
// - devkit_hle: explicit HLE-friendly bootstrap for tooling/legacy paths
// ---------------------------------------------------------------------------
bool Core::fast_boot_from_exe(const char* exe_path, ExeBootMode mode, char* err, size_t err_cap)
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

    // 3. Initialize minimal hardware state for direct EXE boot.
    // Keep the host-selected HLE/text-HLE mode; only the devkit_hle variant
    // installs extra kernel bootstrap structures.
    cpu_->set_hle_vectors(init_hle_vectors_ ? 1 : 0);
    cpu_->set_text_hle(init_text_hle_ ? 1 : 0);
    cpu_->set_use_gpu_vblank(1);

    // Match the practical interrupt baseline used by the old devkit path, but
    // keep the bootstrap structures reserved for the explicit HLE variant.
    {
        r3000::Bus::MemFault mf{};
        bus_->write_u32(0x1F80'1074u, 0x0009u, mf); // VBLANK + DMA
    }
    cpu_->set_cop0(12, (1u << 0) | (1u << 8) | (1u << 10));

    if (mode == ExeBootMode::devkit_hle)
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
    else
    {
        cpu_->set_hle_tcb_addr(0);
    }

    // Inject PSX params into scratchpad RAM if set.
    // The params are written as int32 values at physical address 0x1F800200
    // (scratchpad RAM, 1KB, always mapped). $a1 is pointed at 0x1F800200
    // so the EXE's main() receives them as its second argument.
    // TREX expects: param[0]=mode (1=attract), param[1]=timeout_sec.
    if (!psx_params_.empty() && bus_)
    {
        constexpr uint32_t kScratchpadBase = 0x1F80'0200u;
        r3000::Bus::MemFault mf{};
        for (uint32_t i = 0; i < (uint32_t)psx_params_.size() && i < 32; ++i)
        {
            bus_->write_u32(kScratchpadBase + i * 4, (uint32_t)psx_params_[i], mf);
        }
        cpu_->set_gpr(4, (uint32_t)psx_params_.size());  // $a0 = count
        cpu_->set_gpr(5, kScratchpadBase);                // $a1 = pointer to params
        emu::logf(emu::LogLevel::info, "CORE",
            "PSX params injected: %u ints at 0x%08X ($a0=%u $a1=0x%08X)",
            (unsigned)psx_params_.size(), kScratchpadBase,
            (unsigned)psx_params_.size(), kScratchpadBase);
    }

    emu::logf(
        emu::LogLevel::info,
        "CORE",
        "Dev kit boot: mode=%s %s PC=0x%08X SP=0x%08X hle=%d text_hle=%d",
        (mode == ExeBootMode::devkit_hle) ? "devkit-hle" : "devkit",
        exe_path,
        img.entry_pc,
        cpu_->gpr(29),
        init_hle_vectors_,
        init_text_hle_);
    return true;
}

bool Core::boot_bios_with_exe(const char* exe_path, const char* cd_path,
                              const InitOptions& opt, char* err, size_t err_cap)
{
    if (!exe_path || !*exe_path)
    {
        set_err(err, err_cap, "boot_bios_with_exe: exe_path is required");
        return false;
    }
    if (!bios_data() || bios_size() == 0)
    {
        set_err(err, err_cap, "boot_bios_with_exe: BIOS must be set before calling this");
        return false;
    }

    // Insert disc: real CD (for runtime reads) or in-memory virtual disc.
    // The virtual disc exposes SYSTEM.CNF pointing to BOOT.EXE so the BIOS
    // boot sequence can find and load the EXE without a physical disc.
    if (cd_path && *cd_path)
    {
        emu::logf(emu::LogLevel::info, "CORE",
            "boot_bios_with_exe: real CD=%s, EXE=%s", cd_path, exe_path);

        // With a real CD we still need the BIOS to boot OUR exe, not the
        // game on the CD. We therefore insert the virtual disc so SYSTEM.CNF
        // points to BOOT.EXE, then the BIOS loads our EXE from the virtual
        // disc. The real CD can then be inserted by the caller after the BIOS
        // has finished booting if runtime CD access is needed.
        //
        // For now: insert the virtual disc for BIOS boot. The real CD path
        // is stored so the caller can swap it in after boot if required.
        // (A future extension could intercept the first ReadN and hotswap.)
        if (!cdrom_.insert_virtual_exe_disc(exe_path, err, err_cap))
            return false;
    }
    else
    {
        emu::logf(emu::LogLevel::info, "CORE",
            "boot_bios_with_exe: virtual disc from EXE=%s", exe_path);
        if (!cdrom_.insert_virtual_exe_disc(exe_path, err, err_cap))
            return false;
    }

    // Track psx3dprof identity from EXE path (same as fast_boot_from_exe).
    set_psx3d_profile_identity_from_path(exe_path);
    try_load_psx3d_profile();

    // Build a boot image that starts at the BIOS reset vector.
    // init_from_image creates bus+cpu and sets the initial PC.
    loader::LoadedImage bios_img{};
    bios_img.entry_pc = 0xBFC0'0000u; // MIPS reset vector → BIOS ROM
    bios_img.has_sp   = 1;
    bios_img.sp       = 0x801F'FFF0u;

    // BIOS boot must NOT use HLE vectors — the real BIOS installs its own.
    InitOptions bios_opt = opt;
    bios_opt.hle_vectors = 0;

    if (!init_from_image(bios_img, bios_opt, err, err_cap))
        return false;

    emu::logf(emu::LogLevel::info, "CORE",
        "BIOS devkit boot ready: PC=0xBFC00000, virtual disc active, EXE=%s",
        exe_path);
    return true;
}

} // namespace emu
