#pragma once

#include <atomic>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <vector>

#include "../gte/gte_snapshot.h"
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

// Info about the last CPU→VRAM DMA write (for PSXImageSurfaceComponent)
struct CpuVramWriteInfo
{
    uint32_t seq{0};              // Monotonic sequence number (0 = no write yet)
    uint32_t vram_write_seq{0};   // VRAM modification sequence
    uint32_t frame_count{0};      // Frame at which write occurred
    uint16_t x{0}, y{0};         // VRAM destination
    uint16_t w{0}, h{0};         // Size in 16-bit words / rows
    DisplayConfig display{};      // Display config snapshot at write time
};

struct Stage67GpuDebug
{
    uint32_t gpustat{0};
    uint32_t dma_request{0};
    uint32_t gpu_idle{0};
    uint32_t ready_to_send_vram{0};
    uint32_t ready_to_receive_dma{0};
    uint32_t dma_dir{0};
    uint32_t gp0_state{0};
    uint32_t dma_busy_cycles{0};
    uint32_t vram_to_cpu_active{0};
    uint32_t cpu_vram_words_remaining{0};
    uint32_t scanline{0};
    uint32_t display_line_lsb{0};
    uint32_t display_y{0};
    uint32_t h_res{0};
    uint32_t v_res{0};
    uint32_t is_pal{0};
    uint32_t interlace{0};
    uint32_t in_vblank{0};
    uint32_t even_odd_field{0};
    uint32_t frame_count{0};
};

// Draw command vertex for UE5 rendering bridge.
//
// CONTRACT: x,y are raw GP0 polygon coordinates — i.e. the logical screen-space
// values the game submitted to GP0, *without* draw_env.offset applied.
//
// To obtain VRAM/raster coordinates (needed e.g. for clipping against the draw
// area), add draw_env.offset_x/y. To obtain scan-out coordinates, subtract
// display.display_x/y afterwards. Front-ends that just want to display the
// logical screen space (UE5 render components) can consume x,y directly and
// should NOT mix in display_x/y — doing so re-injects the VRAM buffer flip
// and causes double-buffered games to jump between buffer halves.
struct DrawVertex
{
    int16_t x, y;          // raw GP0 polygon coords (pre-offset, logical screen space)
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

// Origin classification for 3D reconstruction
enum class PrimOrigin : uint8_t
{
    origin_3d      = 0,  // GTE-correlated, render as 3D geometry
    origin_2d_hud  = 1,  // Polygon with no GTE match (HUD/menu)
    origin_2d_rect = 2,  // GP0 rect/sprite (always 2D)
    origin_2d_line = 3,  // GP0 line (usually debug/HUD)
};

// Extended draw command with optional 3D reconstruction data.
// Parallel to DrawCmd — one per triangle when correlation is active.
//
// When origin == origin_3d, the inline 3D data (verts_3d, norms, transform, sz)
// is valid — copied from the GTE face cache at push_triangle time.
// UE5 can consume these directly without needing a separate face cache lookup.
struct DrawCmd3D
{
    PrimOrigin origin{PrimOrigin::origin_2d_hud};
    uint32_t source_pc{0};          // Source GTE or producer PC when known

    // Inline 3D data (populated from face cache when origin == origin_3d)
    gte::GteVertex3D verts_3d[3]; // Original 3D vertices (model space)
    int16_t nx[3], ny[3], nz[3]; // Per-vertex normals (from GTE NCS/NCT)
    gte::GteTransform transform;  // RT + TR at projection time
    uint16_t sz[3];               // Depth values (screen Z)

    // Face/quad correlation metadata
    uint32_t face_idx{0xFFFFFFFFu}; // Face cache index (0xFFFFFFFF = no match)
    bool is_quad{false};             // True if this triangle is half of a GP0 quad
    uint8_t quad_half{0};            // 0 = first tri, 1 = second tri of quad

    // OT depth (from DMA2 linked-list traversal).
    // Counts empty nodes (Z boundaries) from back to front.
    // Primitives sharing the same ot_z are at the same OT depth level.
    uint32_t ot_z{0};
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

    // 3D reconstruction: parallel to cmds (same index = same triangle)
    std::vector<DrawCmd3D> cmds_3d;

    FrameDrawList() { cmds.reserve(4096); cmds_3d.reserve(4096); }
    void clear() { cmds.clear(); cmds_3d.clear(); }
    void push(const DrawCmd& c) { cmds.push_back(c); }
    void push_3d(const DrawCmd3D& c3d) { cmds_3d.push_back(c3d); }
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
    // Semi-transparency diagnostics
    uint32_t semi_tris{0};         // Total semi-transparent triangles pushed
    uint32_t semi_mode_count[4]{}; // Per-mode count (0-3)
    // 3D correlation diagnostics (per push_triangle call)
    uint32_t corr_hit{0};          // Triangles that matched GTE entry (exact)
    uint32_t corr_hit_swap{0};     // Triangles matched via swapped-key fallback
    uint32_t corr_miss{0};         // Triangles with no GTE match (2D/HUD)

    void reset()
    {
        triangles = quads = rects = lines = fills = 0;
        vram_to_vram = cpu_to_vram = vram_to_cpu = 0;
        env_cmds = 0;
        total_words = 0;
        semi_tris = 0;
        semi_mode_count[0] = semi_mode_count[1] = semi_mode_count[2] = semi_mode_count[3] = 0;
        corr_hit = corr_hit_swap = corr_miss = 0;
    }
};

// GPU PS1 - Full GP0 command parser with structured logging
class Gpu
{
  public:
    struct DmaExtremePrimitiveInfo
    {
        bool valid{false};
        int16_t x[3]{};
        int16_t y[3]{};
        uint8_t flags{0};
        uint8_t semi_mode{0};
        uint8_t tex_depth{0};
        uint16_t clut{0};
        uint16_t texpage{0};
        bool corr_valid{false};
        bool corr_swapped{false};
        uint32_t source_pc{0};
        gte::GteVertex3D verts_3d[3]{};
        uint16_t sz[3]{};
        gte::GteTransform transform{};
    };

    // Magic number to detect stale/freed Gpu pointers (Hot Reload issue)
    static constexpr uint32_t kMagicValid = 0x47505531u;  // "GPU1"
    uint32_t magic_{kMagicValid};
    bool is_valid() const { return magic_ == kMagicValid; }

    explicit Gpu(rlog::Logger* logger = nullptr);

    void set_log_sinks(const flog::Sink& gpu_only, const flog::Sink& combined, const flog::Clock& clock);
    void reset_dma_debug_latches();
    bool consume_dma_extreme_primitive(DmaExtremePrimitiveInfo& out);

    // MMIO 32-bit (absolute addresses)
    uint32_t mmio_read32(uint32_t addr);
    void mmio_write32(uint32_t addr, uint32_t v);
    Stage67GpuDebug stage67_debug() const;
    void notify_dma_submit(uint32_t words, bool linked_list);
    void tick_timing(uint32_t cycles);

    void set_dump_file(const char* path);

    // 3D reconstruction: set correlation table (owned by Core, shared with GPU)
    void set_gte_correlation(class GteCorrelationTable* t) { gte_corr_ = t; }

    // VBlank generator (approximate; used to raise IRQ0/I_STAT.bit0).
    int tick_vblank(uint32_t cycles);
    // Swap draw lists + toggle field without scanline counting.
    // Used by external VBlank (worker thread timer).
    void tick_vblank_swap_only();
    uint32_t current_scanline() const;
    uint32_t total_scanlines() const;

    // Access for UE5 bridge
    const DrawEnv& draw_env() const { return draw_env_; }
    const FrameStats& frame_stats() const { return frame_stats_; }
    const uint16_t* vram() const { return vram_.get(); }
    const DisplayConfig& display_config() const { return display_; }

    // CPU→VRAM write tracking for PSXImageSurfaceComponent
    void copy_last_cpu_vram_write(CpuVramWriteInfo& out) const
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        out = last_cpu_vram_write_;
    }
    void copy_recent_cpu_vram_writes(std::vector<CpuVramWriteInfo>& out) const
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        out.clear();
        out.reserve(recent_cpu_vram_write_count_);
        for (uint32_t i = 0; i < recent_cpu_vram_write_count_; ++i)
        {
            const uint32_t index =
                (recent_cpu_vram_write_head_ + kRecentCpuVramWriteHistory - recent_cpu_vram_write_count_ + i)
                % kRecentCpuVramWriteHistory;
            out.push_back(recent_cpu_vram_writes_[index]);
        }
    }
    // Called internally when CPU→VRAM DMA write completes
    void record_cpu_vram_write(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
    {
        std::lock_guard<std::mutex> lock(draw_list_mutex_);
        ++vram_write_seq_;
        ++cpu_vram_write_seq_;
        last_cpu_vram_write_.seq = cpu_vram_write_seq_;
        last_cpu_vram_write_.vram_write_seq = vram_write_seq_;
        last_cpu_vram_write_.frame_count = frame_count_;
        last_cpu_vram_write_.x = x;
        last_cpu_vram_write_.y = y;
        last_cpu_vram_write_.w = w;
        last_cpu_vram_write_.h = h;
        last_cpu_vram_write_.display = display_;

        recent_cpu_vram_writes_[recent_cpu_vram_write_head_] = last_cpu_vram_write_;
        recent_cpu_vram_write_head_ = (recent_cpu_vram_write_head_ + 1u) % kRecentCpuVramWriteHistory;
        if (recent_cpu_vram_write_count_ < kRecentCpuVramWriteHistory)
            ++recent_cpu_vram_write_count_;

        // Check if this write overlaps the current display area
        if (display_.display_enabled && display_.width() > 0 && display_.height() > 0)
        {
            const uint16_t dx = display_.display_x;
            const uint16_t dy = display_.display_y;
            const uint16_t dw = display_.width();
            const uint16_t dh = display_.height();
            // Overlap test
            if (x < dx + dw && x + w > dx && y < dy + dh && y + h > dy)
                display_area_written_ = true;
        }
    }

    // MDEC activity: called from bus when DMA1 (MDEC out) completes
    void notify_mdec_output() { mdec_active_ = true; }

    // Query: has MDEC produced output since last reset?
    bool has_mdec_activity() const { return mdec_active_; }

    // Query: has MDEC decoded AND a write touched the display area since last reset?
    bool has_mdec_display_content() const { return mdec_active_ && display_area_written_; }

    // Reset after VideoComponent consumes the state
    void reset_mdec_display_flags() { mdec_active_ = false; display_area_written_ = false; }

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

    void set_skip_fill_rect(bool skip) { skip_fill_rect_ = skip; }
    bool skip_fill_rect() const { return skip_fill_rect_; }

    // Get previous frame stats (saved before reset, for stuck detection)
    const FrameStats& prev_frame_stats() const { return prev_frame_stats_; }

    /// Monotonically increasing counter bumped on every VRAM write (fill, CPU→VRAM, VRAM→VRAM).
    /// UE5 can compare against its own copy to skip texture uploads when nothing changed.
    uint32_t vram_write_seq() const { return vram_write_seq_; }

  private:
    struct DynamicStatusBits
    {
        uint32_t dma_request{0};
        uint32_t gpu_idle{0};
        uint32_t ready_to_send_vram{0};
        uint32_t ready_to_receive_dma{0};
    };

    void dump_u32(uint32_t port, uint32_t v);
    DynamicStatusBits compute_dynamic_status_bits() const;
    uint32_t build_gpustat() const;

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
    uint32_t dma_busy_cycles_{0};

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
    uint32_t gpustat_suspicious_log_count_{0};

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

    // Frame statistics (frame_stats_ written by CPU, read/reset by GPU thread)
    FrameStats frame_stats_{};
    FrameStats prev_frame_stats_{};
    std::atomic<uint32_t> frame_count_{0};  // GPU thread writes, UE5 render reads
    std::atomic<uint32_t> vram_frame_{0};   // GPU thread writes, UE5 render reads

    // Display configuration (GP1)
    DisplayConfig display_{};

    // CPU→VRAM write tracking
    CpuVramWriteInfo last_cpu_vram_write_{};
    static constexpr uint32_t kRecentCpuVramWriteHistory = 8;
    std::array<CpuVramWriteInfo, kRecentCpuVramWriteHistory> recent_cpu_vram_writes_{};
    uint32_t recent_cpu_vram_write_head_{0};
    uint32_t recent_cpu_vram_write_count_{0};
    uint32_t vram_write_seq_{0};
    uint32_t cpu_vram_write_seq_{0};
    bool mdec_active_{false};        // MDEC has produced output since last reset
    bool display_area_written_{false}; // A write overlapped the display area

    // Double-buffered draw command lists for UE5 bridge
    FrameDrawList draw_lists_[2];
    int draw_active_{0};
    mutable std::mutex draw_list_mutex_; // Protects draw list swap/access

    // 3D reconstruction: correlation table (owned by Core)
    GteCorrelationTable* gte_corr_{nullptr};

    // Draw area clipping toggle (default: on = standard PS1; off = VR mode)
    // Default OFF: draw-area clipping conflicts with the DOUBLE-BUFFER COLLAPSE
    // policy used by the front-end render components (clip rect lives in raster
    // space and flips with the buffer, same as draw_offset). Revisit together
    // with render-to-texture support. See push_triangle() and the policy block
    // in PSX2DRenderComponent.cpp.
    bool clip_to_draw_area_{false};
    bool skip_fill_rect_{false};

    uint32_t vblank_div_{0};
    bool in_vblank_{false};
    std::atomic<bool> even_odd_field_{false}; // GPU thread toggles, CPU reads via GPUSTAT

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
    DmaExtremePrimitiveInfo dma_extreme_primitive_{};
};

} // namespace gpu
