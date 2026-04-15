#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "igte.h"
#include "gte_snapshot.h"

// Shadow GTE for 3D reconstruction with SXY→vertex lookup table.
//
// Separate codebase from Gte (faithful 2D) — can be patched independently
// for better world-space vertex recovery without risking 2D rendering.
//
// After each RTPS/RTPT, real SXY values are kept (NOT overwritten).
// Instead, a side-table maps sxy_packed → vertex_cache_index.
// Gpu3D looks up polygon vertices in this table to find 3D data.
namespace gte
{

class Gte3D : public IGte
{
  public:
    // Differential lattice encoding constants (public for Gpu3D decoder).
    //
    // Tags live in a "dead zone" [DEAD_MIN, DEAD_MAX] that never overlaps real PS1 screen
    // coordinates ([-1024,1023] = uint16 [0,2047]∪[64512,65535]).
    //
    // Encoding is DIFFERENTIAL: face_idx is encoded in the DIFFERENCE between carrier
    // and reference vertices, not in absolute values. This makes it immune to uniform
    // add/sub applied by the game to all vertices (the offset cancels in the difference).
    //
    // RTPT tags:  V0 = carrier, V1 = reference, V2 = carrier (redundant)
    // RTPS quad:  V3 = carrier (same as V0)
    //
    // Carrier.X = REF_BASE + face_idx_lo * SPACING
    // Carrier.Y = REF_BASE + face_idx_hi * SPACING
    // Reference = REF_BASE (both X and Y)
    //
    // Decode: face_idx_lo = round((carrier.x - ref.x) / SPACING)
    static constexpr uint16_t REF_BASE  = 0x2000u; // 8192 — center of dead zone
    static constexpr uint16_t SPACING   = 8;        // tolerance: ±3 carry from add/sub
    static constexpr uint16_t DEAD_MIN  = 0x0800u;  // 2048 — min dead zone coord
    static constexpr uint16_t DEAD_MAX  = 0x7FFFu;  // 32767 — max (bit15=0, positive)
    static constexpr uint32_t MAX_FACE_INDEX = 0xFFFFu;

    Gte3D();

    void reset() override;

    uint32_t read_data(uint32_t idx) const override;
    void write_data(uint32_t idx, uint32_t v) override;

    uint32_t read_ctrl(uint32_t idx) const override;
    void write_ctrl(uint32_t idx, uint32_t v) override;

    void lwc2(uint32_t gte_reg, uint32_t word) override;
    uint32_t swc2(uint32_t gte_reg) const override;

    int execute(uint32_t cop2_instruction) override;

    // FLAG bit constants (public so gte_divide helper can access them)
    static constexpr uint32_t FLAG_MAC1_OFLOW_POS = 1u << 30;
    static constexpr uint32_t FLAG_MAC2_OFLOW_POS = 1u << 29;
    static constexpr uint32_t FLAG_MAC3_OFLOW_POS = 1u << 28;
    static constexpr uint32_t FLAG_MAC1_OFLOW_NEG = 1u << 27;
    static constexpr uint32_t FLAG_MAC2_OFLOW_NEG = 1u << 26;
    static constexpr uint32_t FLAG_MAC3_OFLOW_NEG = 1u << 25;
    static constexpr uint32_t FLAG_IR1_SAT = 1u << 24;
    static constexpr uint32_t FLAG_IR2_SAT = 1u << 23;
    static constexpr uint32_t FLAG_IR3_SAT = 1u << 22;
    static constexpr uint32_t FLAG_COLOR_R = 1u << 21;
    static constexpr uint32_t FLAG_COLOR_G = 1u << 20;
    static constexpr uint32_t FLAG_COLOR_B = 1u << 19;
    static constexpr uint32_t FLAG_SZ3_OTZ_SAT = 1u << 18;
    static constexpr uint32_t FLAG_DIV_OFLOW = 1u << 17;
    static constexpr uint32_t FLAG_MAC0_OFLOW_POS = 1u << 16;  // DuckStation: mac0_overflow
    static constexpr uint32_t FLAG_MAC0_OFLOW_NEG = 1u << 15;  // DuckStation: mac0_underflow
    static constexpr uint32_t FLAG_SX2_SAT = 1u << 14;         // DuckStation: sx2_saturated
    static constexpr uint32_t FLAG_SY2_SAT = 1u << 13;         // DuckStation: sy2_saturated
    static constexpr uint32_t FLAG_IR0_SAT = 1u << 12;         // DuckStation: ir0_saturated
    static constexpr uint32_t FLAG_ERROR_BITS = 0x7F87E000u;

    // ── GTE commands (IGte interface) ────────────────────────────────
    void cmd_rtps(uint32_t cmd) override;
    void cmd_rtpt(uint32_t cmd) override;
    void cmd_mvmva(uint32_t cmd) override;
    void cmd_nclip(uint32_t cmd) override;
    void cmd_avsz3(uint32_t cmd) override;
    void cmd_avsz4(uint32_t cmd) override;
    void cmd_sqr(uint32_t cmd) override;
    void cmd_op(uint32_t cmd) override;
    void cmd_ncds(uint32_t cmd) override;
    void cmd_ncdt(uint32_t cmd) override;
    void cmd_nccs(uint32_t cmd) override;
    void cmd_ncct(uint32_t cmd) override;
    void cmd_ncs(uint32_t cmd) override;
    void cmd_nct(uint32_t cmd) override;
    void cmd_cc(uint32_t cmd) override;
    void cmd_cdp(uint32_t cmd) override;
    void cmd_dpcs(uint32_t cmd) override;
    void cmd_dpct(uint32_t cmd) override;
    void cmd_dcpl(uint32_t cmd) override;
    void cmd_intpl(uint32_t cmd) override;
    void cmd_gpf(uint32_t cmd) override;
    void cmd_gpl(uint32_t cmd) override;

    const GteSnapshot& last_snapshot() const override { return last_snapshot_; }

    // Per-vertex cache access (for GPU/UE5 3D component)
    // WARNING: NOT thread-safe for cross-thread access. Use copy_ready_cache() instead.
    const GteCacheVertex* vertex_by_index(uint32_t index) const;
    uint32_t vertex_count() const { return vertex_index_; }

    // Per-face cache access (face-index system: RTPT stores 3 vertices as one face)
    // WARNING: NOT thread-safe for cross-thread access. Use copy_ready_face_cache() instead.
    const GteCacheFace* face_by_index(uint32_t index) const;
    uint32_t face_count() const { return face_index_; }

    // Per-quad cache access (quad-face system: RTPT+RTPS stores 4 vertices)
    // WARNING: NOT thread-safe for cross-thread access. Use copy_ready_quad_cache() instead.
    const GteCacheQuad* quad_by_index(uint32_t index) const;
    uint32_t quad_count() const { return quad_count_; }

    // Thread-safe: copy the ready (previous frame) vertex cache for UE5 consumption.
    // Call this from UE5 game thread alongside copy_ready_draw_list().
    void copy_ready_cache(std::vector<GteCacheVertex>& out) const;

    // Thread-safe: copy the ready (previous frame) face cache for UE5 consumption.
    void copy_ready_face_cache(std::vector<GteCacheFace>& out) const;

    // Thread-safe: copy the ready (previous frame) quad cache for UE5 consumption.
    void copy_ready_quad_cache(std::vector<GteCacheQuad>& out) const;

    // SXY lookup: given a packed SXY value (lo16=SX, hi16=SY), return vertex index.
    // Returns 0xFFFFFFFF if not found.
    uint32_t lookup_by_sxy(uint32_t sxy_packed) const;
    uint32_t lookup_edge_face_by_segment(int16_t sx0, int16_t sy0, int16_t sx1, int16_t sy1) const;
    void set_source_pc(uint32_t pc) { current_source_pc_ = pc; }

    // Frame management: swap buffers and reset index counter
    void swap_frame();

  private:
    // Register enums
    enum DataReg : uint32_t
    {
        D_VXY0 = 0,  D_VZ0 = 1,  D_VXY1 = 2, D_VZ1 = 3,
        D_VXY2 = 4,  D_VZ2 = 5,  D_RGBC = 6, D_OTZ = 7,
        D_IR0 = 8,   D_IR1 = 9,  D_IR2 = 10, D_IR3 = 11,
        D_SXY0 = 12, D_SXY1 = 13, D_SXY2 = 14, D_SXYP = 15,
        D_SZ0 = 16,  D_SZ1 = 17, D_SZ2 = 18, D_SZ3 = 19,
        D_RGB0 = 20, D_RGB1 = 21, D_RGB2 = 22, D_RES1 = 23,
        D_MAC0 = 24, D_MAC1 = 25, D_MAC2 = 26, D_MAC3 = 27,
        D_IRGB = 28, D_ORGB = 29, D_LZCS = 30, D_LZCR = 31,
    };

    enum CtrlReg : uint32_t
    {
        C_R11R12 = 0, C_R13R21 = 1, C_R22R23 = 2, C_R31R32 = 3, C_R33 = 4,
        C_TRX = 5, C_TRY = 6, C_TRZ = 7,
        C_L11L12 = 8, C_L13L21 = 9, C_L22L23 = 10, C_L31L32 = 11, C_L33 = 12,
        C_RBK = 13, C_GBK = 14, C_BBK = 15,
        C_LR1LR2 = 16, C_LR3LG1 = 17, C_LG2LG3 = 18, C_LB1LB2 = 19, C_LB3 = 20,
        C_RFC = 21, C_GFC = 22, C_BFC = 23,
        C_OFX = 24, C_OFY = 25, C_H = 26, C_DQA = 27, C_DQB = 28,
        C_ZSF3 = 29, C_ZSF4 = 30, C_FLAG = 31,
    };

    // Helpers
    static int32_t s16(uint32_t v);
    static int32_t hi16(uint32_t v);
    static uint32_t pack16(int32_t lo, int32_t hi);
    static int32_t clamp_s16(int32_t v);
    static uint32_t clamp_u16(int32_t v);
    static int32_t clamp_s32(int64_t v);

    int32_t vx(uint32_t n) const;
    int32_t vy(uint32_t n) const;
    int32_t vz(uint32_t n) const;

    void push_sxy(int32_t sx, int32_t sy);
    void push_sz(int32_t sz);
    void push_color(int32_t r, int32_t g, int32_t b, uint8_t code);

    void check_mac_overflow(int idx, int64_t raw);
    int64_t sign_extend_mac(int idx, int64_t v);
    void set_mac(int idx, int64_t v);
    void set_mac_shifted(int idx, int64_t raw, int shift);
    void set_ir(int idx, int32_t v, int lm);

    void rtps_internal(const int32_t V[3], int sf, int lm, bool last);
    void capture_snapshot(const int32_t (*verts)[3], int count);
    void interpolate_color(int64_t in1, int64_t in2, int64_t in3, int shift, int lm);
    void dpcs_internal(const uint8_t color[3], int shift, int lm);

    // Capture V0 normal from data regs (for NCS/NCT/NCDS/NCDT/NCCS/NCCT)
    void capture_normal();

    // ── Tagging + SXY lookup table ──────────────────────────────────
    static uint16_t encode_coord(uint32_t index_8bits);  // backward compat for standalone RTPS
    uint32_t store_vertex(const GteSnapshot& snap, int vert_idx);
    uint32_t store_face(const GteSnapshot& snap);
    void store_quad(const GteSnapshot& rtps_snap, uint32_t face_idx);
    void tag_sxy(uint32_t sxy_reg, uint32_t vertex_idx);  // backward compat
    void tag_face_differential(uint32_t face_idx);
    void after_rtps();
    void after_rtpt();

    // Core GTE state
    GteSnapshot last_snapshot_{};
    uint32_t snapshot_seq_{0};
    uint32_t current_source_pc_{0};
    GteVertex3D rtps_vert_fifo_[3]{};
    uint32_t flag_{0};
    uint32_t data_[32]{};
    uint32_t ctrl_[32]{};

    // Per-game render quirk: force OFX/OFY to zero in projection math.
    // See gte::Gte::set_force_geom_offset_zero() in gte.h for the full
    // policy comment. The shadow GTE must mirror the primary GTE for the
    // 3D reconstruction path to stay consistent.
    bool force_geom_offset_zero_{false};
  public:
    void set_force_geom_offset_zero(bool enabled) { force_geom_offset_zero_ = enabled; }
    bool force_geom_offset_zero() const { return force_geom_offset_zero_; }
  private:

    // Vertex cache + SXY lookup tables (double-buffered)
    uint32_t vertex_index_{0};
    int16_t last_normal_[3]{0, 0, 0};
    std::vector<GteCacheVertex> write_cache_;
    std::vector<GteCacheVertex> read_cache_;
    std::unordered_map<uint32_t, uint32_t> write_sxy_table_;
    std::unordered_map<uint32_t, uint32_t> read_sxy_table_;
    std::unordered_map<uint64_t, uint32_t> write_edge_face_table_;
    std::unordered_map<uint64_t, uint32_t> read_edge_face_table_;

    // Face cache (face-index system): RTPT stores all 3 vertices as one face.
    // Differential encoding: face_idx in differences between carrier/reference vertices.
    uint32_t face_index_{0};
    std::vector<GteCacheFace> write_face_cache_;
    std::vector<GteCacheFace> read_face_cache_;

    // Quad cache: RTPS immediately after RTPT extends the face to 4 vertices.
    // Indexed by the same face_idx as the tri face cache.
    uint32_t quad_count_{0};  // number of quads written this frame
    std::vector<GteCacheQuad> write_quad_cache_;
    std::vector<GteCacheQuad> read_quad_cache_;

    // Differential state: saved by after_rtpt(), consumed by after_rtps()
    uint16_t last_carrier_x_{0}, last_carrier_y_{0};
    uint32_t last_face_idx_{0xFFFFFFFFu};
    bool last_was_rtpt_{false};
    GteSnapshot last_rtpt_snapshot_{};  // saved for quad face construction

    mutable std::mutex cache_mutex_;  // protects read caches during swap_frame / copy
};

} // namespace gte
