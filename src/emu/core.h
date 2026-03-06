#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include "../cdrom/cdrom.h"
#include "../gpu/gpu.h"
#include "../gpu/gpu_3d.h"
#include "../gte/gte_3d.h"
#include "../loader/loader.h"
#include "../log/filelog.h"
#include "../log/logger.h"
#include "../r3000/bus.h"
#include "../r3000/cpu.h"
#include "hooks.h"
#include "provenance_hotspot_profiler.h"
#include "psx3d_profile_store.h"
#include "psx3d_mode_manager.h"

namespace emu
{

// Core emulator instance (CLI + UE5 will both use this).
// Keeps the "core" free of any Unreal dependencies.
class Core
{
  public:
    struct InitOptions
    {
        int pretty{0};
        int trace_io{0};
        int hle_vectors{0}; // opt-in only

        int stop_on_high_ram{0};
        int stop_on_bios_to_ram_nop{0};
        int stop_on_ram_nop{0};

        int stop_on_pc_enabled{0};
        uint32_t stop_on_pc{0};

        int trace_vectors{0};
        int watch_u32_enabled{0};
        uint32_t watch_u32_phys{0};

        int loop_detectors{1}; // enable one-shot loop debug dumps (default: on)
        uint32_t bus_tick_batch{1}; // bus tick batching (1=accurate, 32=fast)
    };

    Core(rlog::Logger* logger);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    // Allocate RAM (zeroed). Can be called once per instance.
    bool alloc_ram(uint32_t bytes, char* err, size_t err_cap);

    // Optional: set BIOS ROM (copied into the instance).
    bool set_bios_copy(const uint8_t* bios, uint32_t bios_size, char* err, size_t err_cap);

    // Configure HW log sinks (cdrom/gpu) and system/io sinks (used by core for diagnostics).
    void set_log_sinks(const flog::Sink& cdlog, const flog::Sink& gpulog, const flog::Sink& syslog, const flog::Sink& iolog, const flog::Clock& clock);

    // Configure "outtext" and TEXT duplication sink (optional).
    void set_text_out(std::FILE* f);
    void set_text_io_sink(const flog::Sink& s, const flog::Clock& c);

    // Callback for BIOS putchar (B(3Dh)). Called for each character.
    using PutcharCallback = void(*)(char ch, void* user);
    void set_putchar_callback(PutcharCallback cb, void* user);

    // Insert disc and GPU dump configuration (optional).
    bool insert_disc(const char* path, char* err, size_t err_cap);
    void set_gpu_dump_file(const char* path);

    // Compare-with-DuckStation: write parseable trace at debug-loop PCs to f (logs/compare_r3000.txt).
    void set_compare_file(std::FILE* f);

    // Access RAM for loaders (ELF/PS-X EXE). Valid after alloc_ram().
    uint8_t* ram();
    uint32_t ram_size() const;
    const uint8_t* bios_data() const;
    uint32_t bios_size() const;

    // Finalize: create bus/cpu and reset from a LoadedImage description.
    bool init_from_image(const loader::LoadedImage& img, const InitOptions& opt, char* err, size_t err_cap);

    // Fast boot: read SYSTEM.CNF from CD, load the EXE, set PC/GP/SP. Requires init_from_image() first.
    bool fast_boot_from_cd(char* err, size_t err_cap);

    // Dev kit boot: load PS-EXE from file, set PC/GP/SP, init HLE kernel. Requires init_from_image() first.
    bool fast_boot_from_exe(const char* exe_path, char* err, size_t err_cap);

    // Step execution (1 instruction). Valid after init_from_image().
    r3000::Cpu::StepResult step();

    uint32_t pc() const;
    void set_gpr(uint32_t idx, uint32_t v);
    void set_pc(uint32_t v);

    r3000::Bus* bus();
    r3000::Cpu* cpu();
    gte::Gte* gte();  // nullptr before init_from_image()
    // Shadow systems for 3D reconstruction (differential tag encoding)
    gpu::Gpu3D* gpu_3d() { return &gpu_3d_; }
    gte::Gte3D* gte_3d() { return &gte_3d_; }

    // Hook system: register diagnostic hooks (memory watches, vblank callbacks, etc.)
    Hooks& hooks() { return hooks_; }

    // Controller input (thread-safe, forwarded to Bus).
    void set_pad_buttons(uint16_t v);

    // Cycle multiplier for timing accuracy (1=simplified, 2=approximate real R3000)
    void set_cycle_multiplier(uint32_t n);

    // Per-instruction cycle count from last step()
    uint32_t last_cycles() const;

    // PSX3D runtime mode controls (analysis authorization + refresh trigger).
    void set_psx3d_mode(Psx3dRunMode m);
    Psx3dRunMode psx3d_mode() const { return psx3d_mode_mgr_.mode(); }
    void set_psx3d_analysis_enabled(bool enabled);
    bool psx3d_analysis_enabled() const { return psx3d_mode_mgr_.analysis_enabled(); }
    bool psx3d_analysis_active() const { return psx3d_mode_mgr_.analysis_active(); }
    uint32_t request_psx3d_analysis_refresh(const char* reason, const char* scope);
    void set_psx3d_profile_path_override(const char* path);
    Psx3dModeManager& psx3d_mode_manager() { return psx3d_mode_mgr_; }
    // Internal hook dispatchers (registered in Core::init_from_image).
    void on_psx3d_vblank(uint32_t vblank_count);
    void on_psx3d_step_pc(uint32_t pc);

    // Boot milestone tracking (for debug comparison with DuckStation)
    struct BootMilestones
    {
        uint64_t bios_to_shell_step{0};     // When PC first goes to 0x8000xxxx (shell/game)
        uint64_t first_gpu_prim_step{0};    // When first GPU primitive is drawn
        uint64_t license_end_step{0};       // After logo sequence (estimated)
        double bios_to_shell_time{0.0};
        double first_gpu_prim_time{0.0};
        double license_end_time{0.0};
        bool bios_to_shell_logged{false};
        bool first_gpu_prim_logged{false};
        bool license_end_logged{false};
    };
    BootMilestones& milestones() { return milestones_; }

  private:
    void set_err(char* err, size_t err_cap, const char* msg) const;
    void set_psx3d_profile_identity_from_path(const char* path);
    void try_load_psx3d_profile();
    void try_save_psx3d_profile();
    void run_psx3d_analysis_refresh(const Psx3dRefreshRequest& req);
    void refresh_psx3d_step_hook_hotspots(const r3000::Bus::Dma2NoHintSummary& s);

    rlog::Logger* logger_{nullptr};
    BootMilestones milestones_{};
    Hooks hooks_{};
    std::unique_ptr<uint8_t[]> ram_{};
    uint32_t ram_size_{0};

    std::unique_ptr<uint8_t[]> bios_{};
    uint32_t bios_size_{0};

    // Devices are owned by the core instance (still "core", not UE-specific).
    cdrom::Cdrom cdrom_;
    gpu::Gpu gpu_;
    gpu::Gpu3D gpu_3d_;    // Shadow GPU for 3D tag decoding
    gte::Gte3D gte_3d_;    // Shadow GTE for differential tag encoding

    std::unique_ptr<r3000::Bus> bus_{};
    std::unique_ptr<r3000::Cpu> cpu_{};

    std::FILE* compare_file_{nullptr};

    // sinks (optional)
    flog::Sink cdlog_{};
    flog::Sink gpulog_{};
    flog::Sink syslog_{};
    flog::Sink iolog_{};
    flog::Clock clock_{};
    int has_clock_{0};

    // pending text sinks (set before cpu_ exists)
    std::FILE* text_out_{nullptr};
    PutcharCallback putchar_cb_{nullptr};
    void* putchar_cb_user_{nullptr};
    flog::Sink text_io_{};
    flog::Clock text_clock_{};
    int has_text_clock_{0};

    Psx3dModeManager psx3d_mode_mgr_{};
    ProvenanceHotspotProfiler provenance_profiler_{};
    uint32_t last_vblank_seen_{0};
    uint32_t last_gpu3d_frame_seen_{0};
    uint32_t last_gpu3d_refresh_frame_{0};
    std::string psx3d_profile_game_id_{};
    std::string psx3d_profile_path_{};
    bool psx3d_profile_loaded_{false};
    bool psx3d_profile_dirty_{false};
    bool psx3d_profile_override_{false};
    bool psx3d_hooks_registered_{false};
    std::unordered_set<uint32_t> psx3d_analyzed_pcs_{};
    uint64_t psx3d_cam_serial_seen_{0};
    std::vector<uint32_t> psx3d_last_nohint_top_pcs_{};
    std::vector<uint32_t> psx3d_step_hook_pcs_{};
};

} // namespace emu

