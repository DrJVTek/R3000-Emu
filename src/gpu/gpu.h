#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

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

// Display configuration (GP1 registers)
struct DisplayConfig
{
    uint16_t display_x{0}, display_y{0};       // GP1(05h) display start in VRAM
    uint16_t h_range_x1{0x200};                // GP1(06h) horizontal range
    uint16_t h_range_x2{0xC00};
    uint16_t v_range_y1{0x010};                // GP1(07h) vertical range
    uint16_t v_range_y2{0x100};
    uint8_t h_res{0};                          // GP1(08h) 0=256,1=320,2=512,3=640,4=368
    uint8_t v_res{0};                          // 0=240,1=480
    bool is_pal{true};
    bool color_24bit{false};
    bool interlace{false};
    bool display_enabled{true};                // GP1(03h)

    uint16_t width() const
    {
        static constexpr uint16_t w[] = {256, 320, 512, 640, 368};
        return (h_res < 5) ? w[h_res] : 320;
    }
    uint16_t height() const
    {
        return v_res ? 480 : (is_pal ? 256 : 240);
    }
};

// Draw command vertex for UE5 rendering bridge
struct DrawVertex
{
    int16_t x, y;          // PS1 screen coords (after draw offset)
    uint8_t r, g, b;       // vertex color
    uint8_t u, v;           // texture coords (0-255)
};

// A single draw command (always triangle; quads/rects split by GPU)
struct DrawCmd
{
    DrawVertex v[3];
    uint16_t clut;          // CLUT location in VRAM
    uint16_t texpage;       // texture page info (X base, Y base, depth, semi mode)
    uint8_t flags;          // bit0=textured, bit1=semi_transparent, bit2=raw_texture
    uint8_t semi_mode;      // semi-transparency mode 0-3
    uint8_t tex_depth;      // 0=4bit, 1=8bit, 2=15bit direct
    uint8_t _pad;
};

// Per-frame draw command list (double-buffered for GPU / UE5)
struct FrameDrawList
{
    std::vector<DrawCmd> cmds;
    uint32_t frame_id{0};

    // Snapshot of GPU state at frame-swap time.
    // UE5 MUST use these instead of reading live GPU state to avoid
    // race conditions (display_y / clip / offset change mid-frame).
    DrawEnv draw_env{};
    DisplayConfig display{};

    FrameDrawList() { cmds.reserve(4096); }
    void clear() { cmds.clear(); }
    void push(const DrawCmd& c) { cmds.push_back(c); }
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
    // Magic number to detect stale/freed Gpu pointers (Hot Reload issue)
    static constexpr uint32_t kMagicValid = 0x47505531u;  // "GPU1"
    uint32_t magic_{kMagicValid};
    bool is_valid() const { return magic_ == kMagicValid; }

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
    const DisplayConfig& display_config() const { return display_; }

    /// Get the ready draw list (previous frame's commands).
    /// WARNING: Not thread-safe if called from UE5 while emulator is running.
    /// Prefer copy_ready_draw_list() for thread-safe access.
    const FrameDrawList& ready_draw_list() const { return draw_lists_[1 - draw_active_]; }

    /// Thread-safe copy of the ready draw list. Use this from UE5.
    /// The mutex ensures the list isn't being swapped/cleared during copy.
    void copy_ready_draw_list(FrameDrawList& out) const
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        out = draw_lists_[1 - draw_active_];
    }

    /// Thread-safe copy of VRAM. Use this from UE5 to avoid race conditions.
    /// @param out Buffer to copy into (must be at least kVramPixels * sizeof(uint16_t) = 1MB)
    /// @param out_seq Output: the vram_write_seq at copy time (for dirty tracking)
    void copy_vram(uint16_t* out, uint32_t& out_seq) const
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        std::memcpy(out, vram_.get(), kVramPixels * sizeof(uint16_t));
        out_seq = vram_write_seq_;
    }

    /// Thread-safe check of VRAM write sequence (for dirty tracking without full copy)
    uint32_t vram_write_seq_locked() const
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        return vram_write_seq_;
    }

    uint32_t vram_frame_count() const { return vram_frame_; }

    // Total VBlank count since init
    uint32_t frame_count() const { return frame_count_; }

    /// Enable/disable draw area clipping for push_triangle.
    /// When disabled (VR mode), all primitives pass through regardless of clip region.
    /// Default: true (standard PS1 behavior).
    void set_clip_to_draw_area(bool enabled) { clip_to_draw_area_ = enabled; }
    bool clip_to_draw_area() const { return clip_to_draw_area_; }

    // Get previous frame stats (saved before reset, for stuck detection)
    const FrameStats& prev_frame_stats() const { return prev_frame_stats_; }

    /// Monotonically increasing counter bumped on every VRAM write (fill, CPU→VRAM, VRAM→VRAM).
    /// UE5 can compare against its own copy to skip texture uploads when nothing changed.
    uint32_t vram_write_seq() const { return vram_write_seq_; }

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

    // Draw command helpers (push to per-frame list for UE5)
    void push_triangle(
        int16_t x0, int16_t y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
        int16_t x1, int16_t y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
        int16_t x2, int16_t y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
        uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth);

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
    bool polyline_has_prev_{false};
    int16_t polyline_prev_x_{0}, polyline_prev_y_{0};
    uint8_t polyline_prev_r_{0}, polyline_prev_g_{0}, polyline_prev_b_{0};
    int polyline_vertex_phase_{0}; // 0=expect color (if gouraud) or XY, 1=expect XY after color

    // Draw environment
    DrawEnv draw_env_{};

    // Frame statistics
    FrameStats frame_stats_{};
    FrameStats prev_frame_stats_{};  // Saved before reset for stuck detection
    uint32_t frame_count_{0};
    uint32_t vram_frame_{0};

    // Display configuration (GP1)
    DisplayConfig display_{};

    // Double-buffered draw command lists for UE5 bridge
    FrameDrawList draw_lists_[2];
    int draw_active_{0};
    mutable std::mutex draw_list_mutex_; // Protects draw list swap/access

    // VRAM write tracking (bumped on fill, cpu→vram, vram→vram)
    uint32_t vram_write_seq_{0};

    // Draw area clipping toggle (default: on = standard PS1; off = VR mode)
    bool clip_to_draw_area_{true};

    uint32_t vblank_div_{0};
    bool in_vblank_{false};
    bool even_odd_field_{false}; // Toggles each VBlank for GPUSTAT bit 31

    // PAL: 33868800 Hz / 49.76 Hz ≈ 680688 CPU cycles per frame
    // NTSC: 33868800 Hz / 59.29 Hz ≈ 571088 CPU cycles per frame
    //
    // Note: our interpreter is 1-CPI (1 instruction = 1 cycle tick) while the
    // real R3000A averages ~3 CPI. We use the REAL cycle counts here so that
    // VBlanks are spaced correctly relative to instruction count — the kernel
    // exception handler takes a fixed number of instructions regardless of CPI,
    // and must complete before the next VBlank arrives.
    static constexpr uint32_t kVblankPeriodCyclesPal  = 680688u;
    static constexpr uint32_t kVblankPeriodCyclesNtsc = 571088u;
    // VBlank lasts ~20 scanlines out of 314 total (PAL) ≈ 43370 CPU cycles
    // NTSC: ~20 scanlines out of 263 total ≈ 36334 CPU cycles
    static constexpr uint32_t kVblankDurationPal  = 43370u;
    static constexpr uint32_t kVblankDurationNtsc = 36334u;

    // Binary GP0 packet capture
    std::FILE* dump_{nullptr};
};

} // namespace gpu
