#pragma once

#include <cstdint>
#include <mutex>

#include "igpu.h"
#include "gpu.h" // DrawCmd, DrawCmd3D, DrawEnv, FrameDrawList

namespace gte { class Gte3D; }

namespace gpu
{

// Shadow GPU for 3D SXY-lookup-based reconstruction.
//
// Receives the same GP0/GP1 command stream as the primary GPU but does NOT
// rasterize to VRAM. Instead it parses ALL commands and builds a draw list
// with raw (unclipped, no sign_extend_11) coordinates for tag decoding.
//
// Full GP0 parser: handles polygons, lines, rects, fills, polylines,
// VRAM transfers, and env commands. This prevents desync with the primary GPU.
class Gpu3D : public IGpu
{
  public:
    Gpu3D();

    // Bind to shadow GTE for SXY→vertex lookup
    void bind_gte_3d(gte::Gte3D* g) { gte_3d_ = g; }

    // IGpu interface
    void reset() override;
    void gp0(uint32_t word) override;
    void gp1(uint32_t word) override;
    void on_vblank() override;
    void copy_ready_draw_list(FrameDrawList& out) const override;
    uint32_t frame_count() const override { return frame_count_; }

  private:
    // GP0 command processing
    void gp0_start_command(uint32_t cmd_word);
    void gp0_execute();
    void gp0_polygon();
    void gp0_fill_rect();
    void gp0_line();
    void gp0_rect();
    void gp0_env_command();
    static int gp0_param_count(uint8_t cmd);

    // Push triangle to active draw list (no rejection, no clipping)
    void push_triangle(
        uint16_t raw_x0, uint16_t raw_y0, uint8_t r0, uint8_t g0, uint8_t b0, uint8_t u0, uint8_t v0,
        uint16_t raw_x1, uint16_t raw_y1, uint8_t r1, uint8_t g1, uint8_t b1, uint8_t u1, uint8_t v1,
        uint16_t raw_x2, uint16_t raw_y2, uint8_t r2, uint8_t g2, uint8_t b2, uint8_t u2, uint8_t v2,
        uint16_t clut, uint16_t texpage, uint8_t flags, uint8_t semi_mode, uint8_t tex_depth,
        PrimOrigin origin,
        uint32_t face_idx, bool is_quad, uint8_t quad_half = 0);

    // GP0 state machine
    enum class Gp0State : uint8_t
    {
        idle,
        collecting_params,
        skipping_vram_data,
        skipping_polyline,
    };
    Gp0State gp0_state_{Gp0State::idle};
    uint32_t cmd_buf_[16]{};
    int cmd_buf_pos_{0};
    int cmd_words_needed_{0};

    // VRAM transfer skip state
    uint32_t vram_words_remaining_{0};

    // Polyline skip state
    bool polyline_gouraud_{false};
    int polyline_phase_{0}; // 0=expect first color/vertex, 1=expect vertex after color

    // Draw environment (only offset matters for coord normalization)
    DrawEnv draw_env_{};

    // Double-buffered draw lists
    FrameDrawList draw_lists_[2];
    int draw_active_{0};
    mutable std::mutex draw_list_mutex_;
    uint32_t frame_count_{0};

    // Display mode (GP1(08h) h_res): 0=256,1=320,2=512,3=640,4=368
    uint8_t h_res_{1}; // default 320

    // Shadow GTE reference for SXY lookup
    gte::Gte3D* gte_3d_{nullptr};

    // Debug counters (public for CLI diagnostic — saved at VBlank before reset)
  public:
    uint32_t dbg_gp0_words_{0};
    uint32_t dbg_gp0_cmds_{0};
    uint32_t dbg_vram_skips_{0};
    Gp0State dbg_state_{Gp0State::idle};
    uint32_t dbg_vram_remaining_{0};
  private:
    uint32_t gp0_words_accum_{0};
    uint32_t gp0_cmds_accum_{0};
    uint32_t gp0_vram_skips_accum_{0};
};

} // namespace gpu
