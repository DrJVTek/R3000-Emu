#pragma once

#include <cstdint>
#include "igte.h"
#include "gte_snapshot.h"

// GTE (Geometry Transformation Engine) de la PS1.
// Objectif: garder tout le code GTE séparé du CPU pour une base propre et pédagogique.
//
// Dans l'ISA, le GTE est exposé via COP2:
// - MFC2/MTC2 : transferts avec les registres "data" du GTE
// - CFC2/CTC2 : transferts avec les registres "control" du GTE
// - Instructions GTE (RTPS, MVMVA, NCLIP, etc.) via le champ "function" de COP2
namespace gte
{

class Gte : public IGte
{
  public:
    Gte();

    void reset() override;

    // Registres data/control (index 0..31).
    // NOTE: certains registres sont packés/saturés en vrai. On commence simple: stockage brut
    // 32-bit.
    uint32_t read_data(uint32_t idx) const override;
    void write_data(uint32_t idx, uint32_t v) override;

    uint32_t read_ctrl(uint32_t idx) const override;
    void write_ctrl(uint32_t idx, uint32_t v) override;

    // LWC2/SWC2 utilisent le même espace de registres que MTC2/MFC2 (data regs).
    // On garde une API explicite pour rendre l'intention claire dans le CPU.
    void lwc2(uint32_t gte_reg, uint32_t word) override;
    uint32_t swc2(uint32_t gte_reg) const override;

    // Exécute une instruction COP2 "CO" (commande GTE).
    // Retourne le nombre de cycles GTE (>0) si la commande est reconnue, 0 sinon.
    // Cycle counts match real PS1 hardware (PSX-SPX / DuckStation reference).
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

  private:
    struct RtpsTraceVertex
    {
        int32_t vx{0};
        int32_t vy{0};
        int32_t vz{0};
        int64_t mac1_raw{0};
        int64_t mac2_raw{0};
        int64_t mac3_raw{0};
        int32_t ir1{0};
        int32_t ir2{0};
        int32_t ir3_z{0};
        uint32_t sz3{0};
        uint32_t h{0};
        uint32_t quotient{0};
        int64_t sx_accum{0};
        int64_t sy_accum{0};
        int32_t sx_preclamp{0};
        int32_t sy_preclamp{0};
        uint32_t flag_before_push{0};
        bool last{false};
    };

    // Index des registres GTE (data/control) pour rendre le code lisible en live.
    // Data regs (0..31):
    enum DataReg : uint32_t
    {
        D_VXY0 = 0,
        D_VZ0 = 1,
        D_VXY1 = 2,
        D_VZ1 = 3,
        D_VXY2 = 4,
        D_VZ2 = 5,
        D_RGBC = 6,
        D_OTZ = 7,
        D_IR0 = 8,
        D_IR1 = 9,
        D_IR2 = 10,
        D_IR3 = 11,
        D_SXY0 = 12,
        D_SXY1 = 13,
        D_SXY2 = 14,
        D_SXYP = 15,
        D_SZ0 = 16,
        D_SZ1 = 17,
        D_SZ2 = 18,
        D_SZ3 = 19,
        D_RGB0 = 20,
        D_RGB1 = 21,
        D_RGB2 = 22,
        D_RES1 = 23,
        D_MAC0 = 24,
        D_MAC1 = 25,
        D_MAC2 = 26,
        D_MAC3 = 27,
        D_IRGB = 28,
        D_ORGB = 29,
        D_LZCS = 30,
        D_LZCR = 31,
    };

    // Control regs (0..31):
    enum CtrlReg : uint32_t
    {
        C_R11R12 = 0,
        C_R13R21 = 1,
        C_R22R23 = 2,
        C_R31R32 = 3,
        C_R33 = 4,
        C_TRX = 5,
        C_TRY = 6,
        C_TRZ = 7,
        C_L11L12 = 8,
        C_L13L21 = 9,
        C_L22L23 = 10,
        C_L31L32 = 11,
        C_L33 = 12,
        C_RBK = 13,
        C_GBK = 14,
        C_BBK = 15,
        C_LR1LR2 = 16,
        C_LR3LG1 = 17,
        C_LG2LG3 = 18,
        C_LB1LB2 = 19,
        C_LB3 = 20,
        C_RFC = 21,
        C_GFC = 22,
        C_BFC = 23,
        C_OFX = 24,
        C_OFY = 25,
        C_H = 26,
        C_DQA = 27,
        C_DQB = 28,
        C_ZSF3 = 29,
        C_ZSF4 = 30,
        C_FLAG = 31,
    };

    // Helpers fixed-point / saturation (pédago).
    static int32_t s16(uint32_t v);
    static int32_t hi16(uint32_t v);
    static uint32_t pack16(int32_t lo, int32_t hi);

    static int32_t clamp_s16(int32_t v);
    static uint32_t clamp_u16(int32_t v);
    static int32_t clamp_s32(int64_t v);

    // Accès pratique aux composantes des registres packés VXY/SXY.
    int32_t vx(uint32_t n) const;
    int32_t vy(uint32_t n) const;
    int32_t vz(uint32_t n) const;

    void push_sxy(int32_t sx, int32_t sy);
    void push_sz(int32_t sz);
    void push_color(int32_t r, int32_t g, int32_t b, uint8_t code);

    void check_mac_overflow(int idx, int64_t raw);
    int64_t sign_extend_mac(int idx, int64_t v);  // 44-bit truncation + overflow check
    void set_mac(int idx, int64_t v);
    void set_mac_shifted(int idx, int64_t raw, int shift);
    void set_ir(int idx, int32_t v, int lm);

    // Internal RTPS for single vertex (called by both RTPS and RTPT)
    void rtps_internal(const int32_t V[3], int sf, int lm, bool last);

    // 3D reconstruction: capture snapshot after RTPS/RTPT
    void capture_snapshot(const int32_t (*verts)[3], int count);

    // DuckStation InterpolateColor pattern: MAC+(FC-MAC)*IR0
    void interpolate_color(int64_t in1, int64_t in2, int64_t in3, int shift, int lm);
    // Internal DPCS (takes raw RGB bytes, used by DPCS and DPCT)
    void dpcs_internal(const uint8_t color[3], int shift, int lm);

    // Commandes (subset utile pour démarrer "matrices").
    void cmd_mvmva(uint32_t cmd) override;
    void cmd_rtps(uint32_t cmd) override;
    void cmd_rtpt(uint32_t cmd) override;
    void cmd_nclip(uint32_t cmd) override;
    void cmd_avsz3(uint32_t cmd) override;
    void cmd_avsz4(uint32_t cmd) override;
    void cmd_sqr(uint32_t cmd) override;
    void cmd_gpf(uint32_t cmd) override;
    void cmd_gpl(uint32_t cmd) override;
    void cmd_op(uint32_t cmd) override;
    void cmd_dpcs(uint32_t cmd) override;
    void cmd_intpl(uint32_t cmd) override;
    void cmd_ncds(uint32_t cmd) override;
    void cmd_cdp(uint32_t cmd) override;
    void cmd_ncdt(uint32_t cmd) override;
    void cmd_nccs(uint32_t cmd) override;
    void cmd_cc(uint32_t cmd) override;
    void cmd_ncs(uint32_t cmd) override;
    void cmd_nct(uint32_t cmd) override;
    void cmd_dcpl(uint32_t cmd) override;
    void cmd_dpct(uint32_t cmd) override;
    void cmd_ncct(uint32_t cmd) override;

    // 3D reconstruction: snapshot captured after each RTPS/RTPT.
    GteSnapshot last_snapshot_{};
    uint32_t snapshot_seq_{0};

    // RTPS accumulation: FIFO of last 3 input 3D vertices.
    // After 3 consecutive RTPS calls, this contains the same 3 vertices
    // that the GTE SXY FIFO holds as projected screen coords.
    GteVertex3D rtps_vert_fifo_[3]{};
    RtpsTraceVertex rtps_trace_[3]{};
    int rtps_trace_count_{0};

  public:
    const GteSnapshot& last_snapshot() const override { return last_snapshot_; }

    // ── RTPT diagnostic counters (per-frame, reset at VBlank) ──
    struct RtptDiag {
        uint32_t rtpt_count{0};     // total RTPT calls
        uint32_t v1_eq_v2{0};       // V1==V2 pattern
        uint32_t v0_eq_v2{0};       // V0==V2 pattern
        uint32_t all_same{0};       // V0==V1==V2
        uint32_t all_unique{0};     // all different
        void reset() { rtpt_count = v1_eq_v2 = v0_eq_v2 = all_same = all_unique = 0; }
    };
    const RtptDiag& rtpt_diag() const { return rtpt_diag_; }
    void rtpt_diag_reset() { rtpt_diag_.reset(); }

    // Per-game render quirk: force GTE OFX/OFY (cop2 control 24/25) to be
    // treated as zero inside RTPS/RTPT projection math.
    //
    // Some libgs games (e.g. SCEE Demo One TREX) implement their double-buffer
    // Y shift via SetGeomOffset(x, y) which writes to OFX/OFY. RTPS adds those
    // values to the projected SXY *before* the game emits any GP0 polygon
    // command, so the back/front buffers receive the same model rendered at
    // different Y positions. The front-end (UE5 render component) sees this
    // as the model jumping between two Y positions every frame.
    //
    // Setting this flag makes RTPS/RTPT pretend OFX==OFY==0 *for projection
    // purposes only* — the game's own writes to ctrl_[24]/[25] still go
    // through write_ctrl() and the registers can still be read back correctly
    // (so games that read OFX/OFY for other reasons aren't broken). Only the
    // projection arithmetic in rtps_internal ignores them.
    //
    // Default OFF — turning it on globally would corrupt games that
    // legitimately use OFX/OFY for non-buffer effects (centring, viewport
    // shifts). Per-game opt-in via psx3dprof QUIRK + UE5 UPROPERTY +
    // CLI flag.
    void set_force_geom_offset_zero(bool enabled) { force_geom_offset_zero_ = enabled; }
    bool force_geom_offset_zero() const { return force_geom_offset_zero_; }

  private:
    RtptDiag rtpt_diag_{};

    uint32_t flag_{0};  // Accumulated during command execution

    uint32_t data_[32]{};
    uint32_t ctrl_[32]{};

    // See set_force_geom_offset_zero() above.
    bool force_geom_offset_zero_{false};
};

} // namespace gte
