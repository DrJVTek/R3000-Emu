#pragma once

#include <cstdint>

// GTE (Geometry Transformation Engine) de la PS1.
// Objectif: garder tout le code GTE séparé du CPU pour une base propre et pédagogique.
//
// Dans l'ISA, le GTE est exposé via COP2:
// - MFC2/MTC2 : transferts avec les registres "data" du GTE
// - CFC2/CTC2 : transferts avec les registres "control" du GTE
// - Instructions GTE (RTPS, MVMVA, NCLIP, etc.) via le champ "function" de COP2
namespace gte
{

class Gte
{
  public:
    Gte();

    void reset();

    // Registres data/control (index 0..31).
    // NOTE: certains registres sont packés/saturés en vrai. On commence simple: stockage brut
    // 32-bit.
    uint32_t read_data(uint32_t idx) const;
    void write_data(uint32_t idx, uint32_t v);

    uint32_t read_ctrl(uint32_t idx) const;
    void write_ctrl(uint32_t idx, uint32_t v);

    // LWC2/SWC2 utilisent le même espace de registres que MTC2/MFC2 (data regs).
    // On garde une API explicite pour rendre l'intention claire dans le CPU.
    void lwc2(uint32_t gte_reg, uint32_t word);
    uint32_t swc2(uint32_t gte_reg) const;

    // Exécute une instruction COP2 "CO" (commande GTE).
    // On attend typiquement le mot d'instruction complet (opcode 0x12 inclus), car les bits
    // de contrôle (sf/lm/mx/v/tx) sont dedans.
    // Retourne 1 si la commande est reconnue/implémentée, 0 sinon.
    int execute(uint32_t cop2_instruction);

  private:
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
    void push_sz(uint32_t sz);

    void set_mac(int idx, int64_t v);
    void set_ir(int idx, int32_t v, int lm);

    // Commandes (subset utile pour démarrer "matrices").
    void cmd_mvmva(uint32_t cmd);
    void cmd_rtps(uint32_t cmd);
    void cmd_rtpt(uint32_t cmd);
    void cmd_nclip(uint32_t cmd);
    void cmd_avsz3(uint32_t cmd);
    void cmd_avsz4(uint32_t cmd);
    void cmd_sqr(uint32_t cmd);
    void cmd_gpf(uint32_t cmd);
    void cmd_gpl(uint32_t cmd);
    void cmd_op(uint32_t cmd);
    void cmd_dpcs(uint32_t cmd);
    void cmd_intpl(uint32_t cmd);
    void cmd_ncds(uint32_t cmd);
    void cmd_cdp(uint32_t cmd);
    void cmd_ncdt(uint32_t cmd);
    void cmd_nccs(uint32_t cmd);
    void cmd_cc(uint32_t cmd);
    void cmd_ncs(uint32_t cmd);
    void cmd_nct(uint32_t cmd);
    void cmd_dcpl(uint32_t cmd);
    void cmd_dpct(uint32_t cmd);
    void cmd_ncct(uint32_t cmd);

    uint32_t data_[32]{};
    uint32_t ctrl_[32]{};
};

} // namespace gte
