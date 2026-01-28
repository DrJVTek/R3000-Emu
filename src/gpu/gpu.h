#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "../log/filelog.h"
#include "../log/logger.h"

namespace gpu
{

// GPU PS1 (modèle minimal, orienté "boot BIOS" + pont Unreal)
//
// Objectifs immédiats:
// - Fournir des registres MMIO GP0/GP1 compatibles (write) + lecture status (read)
// - Ne pas faire crasher le BIOS quand il sonde le GPU
// - Capturer les paquets GP0 (optionnel) pour pouvoir les rejouer côté rendu (Unreal)
class Gpu
{
  public:
    explicit Gpu(rlog::Logger* logger = nullptr);

    // Logs dédiés (optionnels).
    // - gpu_only: logs GPU uniquement
    // - combined: logs "IO" (CD + GPU + system)
    void set_log_sinks(const flog::Sink& gpu_only, const flog::Sink& combined, const flog::Clock& clock);

    // MMIO 32-bit (adresses absolues)
    uint32_t mmio_read32(uint32_t addr);
    void mmio_write32(uint32_t addr, uint32_t v);

    void set_dump_file(const char* path);

    // VBlank generator (approximate; used to raise IRQ0/I_STAT.bit0).
    // This is not cycle-accurate; it exists to model the presence of GPU-driven VBlank
    // without adding a full video timing pipeline yet.
    int tick_vblank(uint32_t cycles);

  private:
    void dump_u32(uint32_t port, uint32_t v);

    static constexpr uint32_t kVramWidth = 1024u;
    static constexpr uint32_t kVramHeight = 512u;
    static constexpr uint32_t kVramPixels = kVramWidth * kVramHeight;

    rlog::Logger* logger_{nullptr};

    flog::Sink log_gpu_{};
    flog::Sink log_io_{};
    flog::Clock clock_{};
    int has_clock_{0};

    // GPU status register (0x1F801814)
    uint32_t status_{0};
    uint32_t dma_dir_{0}; // GP1(04h) 0..3, reflected in GPUSTAT.29-30 and GPUSTAT.25

    // Minimal VRAM backing store (15-bit pixels, stored as u16).
    // Used for VRAM->CPU transfers (GP0(C0h) + GPUREAD).
    // Heap-allocated to avoid stack overflow (~1MB).
    std::unique_ptr<uint16_t[]> vram_;

    enum class Gp0Pending : uint8_t
    {
        none = 0,
        vram_to_cpu = 0xC0
    };
    Gp0Pending gp0_pending_{Gp0Pending::none};
    uint8_t gp0_params_left_{0};

    // VRAM->CPU transfer state (GP0(C0h) ... then read GPUREAD).
    bool vram_to_cpu_active_{false};
    uint16_t vram_x_{0};
    uint16_t vram_y_{0};
    uint16_t vram_w_{0};
    uint16_t vram_h_{0};
    uint16_t vram_col_{0};
    uint16_t vram_row_{0};

    uint32_t vblank_div_{0};
    // Approximate VBlank period for bring-up.
    // Faster than real hardware (100k vs ~565k) to make the boot sequence run quicker.
    static constexpr uint32_t kVblankPeriodCycles = 100000u;

    // Simple GP0 packet capture
    std::FILE* dump_{nullptr};
};

} // namespace gpu

