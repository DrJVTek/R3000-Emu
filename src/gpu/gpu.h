#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>

#include "../log/filelog.h"
#include "../log/logger.h"

namespace gpu
{

// Draw environment state (GP0 E1h-E6h)
struct DrawEnv
{
    uint32_t texpage_raw{0};     // E1h raw value
    uint32_t tex_window{0};      // E2h
    uint16_t clip_x1{0}, clip_y1{0}; // E3h
    uint16_t clip_x2{0}, clip_y2{0}; // E4h
    int16_t  offset_x{0}, offset_y{0}; // E5h
    uint16_t mask_bits{0};       // E6h
};

// Per-frame GPU statistics
struct FrameStats
{
    uint32_t triangles{0};
    uint32_t quads{0};
    uint32_t rects{0};
    uint32_t lines{0};
    uint32_t fills{0};
    uint32_t vram_to_vram{0};
    uint32_t cpu_to_vram{0};
    uint32_t vram_to_cpu{0};
    uint32_t env_cmds{0};
    uint32_t total_words{0};

    void reset()
    {
        triangles = quads = rects = lines = fills = 0;
        vram_to_vram = cpu_to_vram = vram_to_cpu = 0;
        env_cmds = 0;
        total_words = 0;
    }
};

// GPU PS1 - Full GP0 command parser with structured logging
class Gpu
{
  public:
    explicit Gpu(rlog::Logger* logger = nullptr);

    void set_log_sinks(const flog::Sink& gpu_only, const flog::Sink& combined, const flog::Clock& clock);

    // MMIO 32-bit (absolute addresses)
    uint32_t mmio_read32(uint32_t addr);
    void mmio_write32(uint32_t addr, uint32_t v);

    void set_dump_file(const char* path);

    // VBlank generator (approximate; used to raise IRQ0/I_STAT.bit0).
    int tick_vblank(uint32_t cycles);

    // Access for UE5 bridge
    const DrawEnv& draw_env() const { return draw_env_; }
    const FrameStats& frame_stats() const { return frame_stats_; }
    const uint16_t* vram() const { return vram_.get(); }

  private:
    void dump_u32(uint32_t port, uint32_t v);

    // GP0 command processing
    void gp0_write(uint32_t v);
    void gp0_start_command(uint32_t cmd_word);
    void gp0_execute();
    static int gp0_param_count(uint8_t cmd);

    // GP0 command handlers
    void gp0_fill_rect();
    void gp0_polygon();
    void gp0_line();
    void gp0_rect();
    void gp0_vram_to_vram();
    void gp0_cpu_to_vram_start();
    void gp0_cpu_to_vram_data(uint32_t v);
    void gp0_vram_to_cpu_start();
    void gp0_env_command();

    // GP1 command processing
    void gp1_write(uint32_t v);

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
    uint32_t dma_dir_{0};

    // VRAM backing store (15-bit pixels as u16)
    std::unique_ptr<uint16_t[]> vram_;

    // GP0 command FIFO state machine
    enum class Gp0State : uint8_t
    {
        idle,
        collecting_params,
        receiving_vram_data,
        polyline
    };
    Gp0State gp0_state_{Gp0State::idle};
    uint32_t cmd_buf_[16]{};     // Max 12 words + margin
    int cmd_buf_pos_{0};
    int cmd_words_needed_{0};

    // CPU→VRAM transfer state (GP0 A0h)
    uint16_t cpu_vram_x_{0}, cpu_vram_y_{0};
    uint16_t cpu_vram_w_{0}, cpu_vram_h_{0};
    uint16_t cpu_vram_col_{0}, cpu_vram_row_{0};
    uint32_t cpu_vram_words_remaining_{0};

    // VRAM→CPU transfer state (GP0 C0h + GPUREAD)
    bool vram_to_cpu_active_{false};
    uint16_t read_vram_x_{0}, read_vram_y_{0};
    uint16_t read_vram_w_{0}, read_vram_h_{0};
    uint16_t read_vram_col_{0}, read_vram_row_{0};

    // Polyline state
    uint32_t polyline_color_{0};
    bool polyline_gouraud_{false};
    bool polyline_semi_{false};

    // Draw environment
    DrawEnv draw_env_{};

    // Frame statistics
    FrameStats frame_stats_{};
    uint32_t frame_count_{0};

    uint32_t vblank_div_{0};
    bool in_vblank_{false};

    // PAL: 33868800 Hz / 49.76 Hz ≈ 680688 CPU cycles per frame
    // NTSC: 33868800 Hz / 59.29 Hz ≈ 571088 CPU cycles per frame
    // TODO: switch based on video mode; default to PAL (SCPH-7502)
    //
    // Note: our interpreter is 1-CPI (1 instruction = 1 cycle tick) while the
    // real R3000A averages ~3 CPI. We use the REAL cycle counts here so that
    // VBlanks are spaced correctly relative to instruction count — the kernel
    // exception handler takes a fixed number of instructions regardless of CPI,
    // and must complete before the next VBlank arrives.
    static constexpr uint32_t kVblankPeriodCycles = 680688u;
    // VBlank lasts ~20 scanlines out of 314 total (PAL) ≈ 43370 CPU cycles
    static constexpr uint32_t kVblankDuration = 43370u;

    // Binary GP0 packet capture
    std::FILE* dump_{nullptr};
};

} // namespace gpu
