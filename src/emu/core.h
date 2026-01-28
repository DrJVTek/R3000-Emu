#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "../cdrom/cdrom.h"
#include "../gpu/gpu.h"
#include "../loader/loader.h"
#include "../log/filelog.h"
#include "../log/logger.h"
#include "../r3000/bus.h"
#include "../r3000/cpu.h"

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

    // Step execution (1 instruction). Valid after init_from_image().
    r3000::Cpu::StepResult step();

    uint32_t pc() const;
    void set_gpr(uint32_t idx, uint32_t v);
    void set_pc(uint32_t v);

    r3000::Bus* bus();
    r3000::Cpu* cpu();

  private:
    void set_err(char* err, size_t err_cap, const char* msg) const;

    rlog::Logger* logger_{nullptr};

    std::unique_ptr<uint8_t[]> ram_{};
    uint32_t ram_size_{0};

    std::unique_ptr<uint8_t[]> bios_{};
    uint32_t bios_size_{0};

    // Devices are owned by the core instance (still "core", not UE-specific).
    cdrom::Cdrom cdrom_;
    gpu::Gpu gpu_;

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
    flog::Sink text_io_{};
    flog::Clock text_clock_{};
    int has_text_clock_{0};
};

} // namespace emu

