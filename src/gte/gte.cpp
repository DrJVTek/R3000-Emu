#include "gte.h"

namespace gte
{

Gte::Gte()
{
    reset();
}

void Gte::reset()
{
    for (int i = 0; i < 32; ++i)
    {
        data_[i] = 0;
        ctrl_[i] = 0;
    }
}

uint32_t Gte::read_data(uint32_t idx) const
{
    return data_[idx & 31u];
}

void Gte::write_data(uint32_t idx, uint32_t v)
{
    data_[idx & 31u] = v;
}

uint32_t Gte::read_ctrl(uint32_t idx) const
{
    return ctrl_[idx & 31u];
}

void Gte::write_ctrl(uint32_t idx, uint32_t v)
{
    ctrl_[idx & 31u] = v;
}

int32_t Gte::s16(uint32_t v)
{
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

int32_t Gte::hi16(uint32_t v)
{
    return (int32_t)(int16_t)((v >> 16) & 0xFFFFu);
}

uint32_t Gte::pack16(int32_t lo, int32_t hi)
{
    return (uint32_t)((uint16_t)lo) | ((uint32_t)((uint16_t)hi) << 16);
}

int32_t Gte::clamp_s16(int32_t v)
{
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return v;
}

uint32_t Gte::clamp_u16(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 0xFFFF)
        return 0xFFFF;
    return (uint32_t)v;
}

int32_t Gte::clamp_s32(int64_t v)
{
    if (v < (int64_t)INT32_MIN)
        return INT32_MIN;
    if (v > (int64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)v;
}

int32_t Gte::vx(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VXY0 : (n == 1) ? D_VXY1 : D_VXY2;
    return s16(data_[idx]);
}

int32_t Gte::vy(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VXY0 : (n == 1) ? D_VXY1 : D_VXY2;
    return hi16(data_[idx]);
}

int32_t Gte::vz(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VZ0 : (n == 1) ? D_VZ1 : D_VZ2;
    return s16(data_[idx]);
}

void Gte::push_sxy(int32_t sx, int32_t sy)
{
    // Pipeline interne GTE: SXY0 <= SXY1 <= SXY2 <= SXYP.
    data_[D_SXY0] = data_[D_SXY1];
    data_[D_SXY1] = data_[D_SXY2];
    data_[D_SXY2] = data_[D_SXYP];
    data_[D_SXYP] = pack16(sx, sy);
}

void Gte::push_sz(uint32_t sz)
{
    // SZ0..SZ3 shift register.
    data_[D_SZ0] = data_[D_SZ1];
    data_[D_SZ1] = data_[D_SZ2];
    data_[D_SZ2] = data_[D_SZ3];
    data_[D_SZ3] = sz;
}

void Gte::set_mac(int idx, int64_t v)
{
    data_[D_MAC0 + (uint32_t)idx] = (uint32_t)clamp_s32(v);
}

void Gte::set_ir(int idx, int32_t v, int lm)
{
    // IR1..IR3 sont saturés. Le bit LM (limit mode) force le min à 0.
    int32_t out = v;
    if (lm && out < 0)
        out = 0;
    out = clamp_s16(out);
    data_[D_IR0 + (uint32_t)idx] = (uint32_t)out;
}

void Gte::cmd_nclip(uint32_t)
{
    // NCLIP: calcule le déterminant 2D (triangle) sur SXY0/1/2.
    const int32_t x0 = s16(data_[D_SXY0]);
    const int32_t y0 = hi16(data_[D_SXY0]);
    const int32_t x1 = s16(data_[D_SXY1]);
    const int32_t y1 = hi16(data_[D_SXY1]);
    const int32_t x2 = s16(data_[D_SXY2]);
    const int32_t y2 = hi16(data_[D_SXY2]);

    const int64_t n = (int64_t)x0 * (y1 - y2) + (int64_t)x1 * (y2 - y0) + (int64_t)x2 * (y0 - y1);
    set_mac(0, n);
}

void Gte::cmd_mvmva(uint32_t cmd)
{
    // MVMVA: multiplication matrice-vecteur avec options.
    // NOTE: pour une version éducative initiale, on implémente une variante utile:
    // - matrice = rotation (R)
    // - vecteur = V0
    // - translation = TR
    //
    // On respecte sf/lm (shift fraction / limit mode) pour IR.
    const int sf = (cmd >> 19) & 1; // 0=pas de shift, 1=>>12 (convention GTE)
    const int lm = (cmd >> 10) & 1;

    // Rotation matrix (3x3) packée en pairs 16-bit.
    const int32_t r11 = s16(ctrl_[C_R11R12]);
    const int32_t r12 = hi16(ctrl_[C_R11R12]);
    const int32_t r13 = s16(ctrl_[C_R13R21]);
    const int32_t r21 = hi16(ctrl_[C_R13R21]);
    const int32_t r22 = s16(ctrl_[C_R22R23]);
    const int32_t r23 = hi16(ctrl_[C_R22R23]);
    const int32_t r31 = s16(ctrl_[C_R31R32]);
    const int32_t r32 = hi16(ctrl_[C_R31R32]);
    const int32_t r33 = s16(ctrl_[C_R33]);

    const int32_t trx = (int32_t)ctrl_[C_TRX];
    const int32_t try_ = (int32_t)ctrl_[C_TRY];
    const int32_t trz = (int32_t)ctrl_[C_TRZ];

    const int32_t x = vx(0);
    const int32_t y = vy(0);
    const int32_t z = vz(0);

    // MAC = (R * V) + TR
    const int64_t mac1 =
        (int64_t)r11 * x + (int64_t)r12 * y + (int64_t)r13 * z + ((int64_t)trx << 12);
    const int64_t mac2 =
        (int64_t)r21 * x + (int64_t)r22 * y + (int64_t)r23 * z + ((int64_t)try_ << 12);
    const int64_t mac3 =
        (int64_t)r31 * x + (int64_t)r32 * y + (int64_t)r33 * z + ((int64_t)trz << 12);

    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);

    const int shift = sf ? 12 : 0;
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

void Gte::cmd_rtps(uint32_t cmd)
{
    // RTPS: rotation+translation+perspective sur V0 -> SXY + SZ.
    //
    // On réutilise MVMVA (variante R*V0+TR) pour produire IR1..3 puis on calcule projection.
    cmd_mvmva(cmd);

    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    // SZ3 reçoit généralement Z (après saturation). Ici on pousse une valeur positive simple.
    const uint32_t sz = (uint32_t)clamp_u16(ir3);
    push_sz(sz);

    // Projection: SX = OFX + (H * IR1) / SZ3 ; SY = OFY + (H * IR2) / SZ3
    const int32_t ofx = (int32_t)ctrl_[C_OFX];
    const int32_t ofy = (int32_t)ctrl_[C_OFY];
    const int32_t h = (int32_t)(ctrl_[C_H] & 0xFFFFu);

    int32_t sx = ofx;
    int32_t sy = ofy;
    if (sz != 0)
    {
        sx = ofx + (int32_t)(((int64_t)h * ir1) / (int32_t)sz);
        sy = ofy + (int32_t)(((int64_t)h * ir2) / (int32_t)sz);
    }

    push_sxy(sx, sy);
}

void Gte::cmd_rtpt(uint32_t cmd)
{
    // RTPT: comme RTPS mais sur V0, V1, V2 (3 points) en une commande.
    //
    // Version pédagogique: on applique 3 fois une logique RTPS (rotation+translation+projection)
    // en réutilisant nos helpers Vn.
    //
    // NOTE: dans le vrai hardware, certains registres/pipelines ont des comportements subtils.
    // Ici on vise un 1er socle correct "dans l'esprit" pour le live.
    for (uint32_t i = 0; i < 3; ++i)
    {
        // Hack simple: on réécrit temporairement V0 = Vi puis on appelle RTPS.
        const uint32_t save_vxy0 = data_[D_VXY0];
        const uint32_t save_vz0 = data_[D_VZ0];

        const uint32_t src_vxy = (i == 0) ? D_VXY0 : (i == 1) ? D_VXY1 : D_VXY2;
        const uint32_t src_vz = (i == 0) ? D_VZ0 : (i == 1) ? D_VZ1 : D_VZ2;
        data_[D_VXY0] = data_[src_vxy];
        data_[D_VZ0] = data_[src_vz];

        cmd_rtps(cmd);

        data_[D_VXY0] = save_vxy0;
        data_[D_VZ0] = save_vz0;
    }
}

void Gte::cmd_avsz3(uint32_t)
{
    // AVSZ3: OTZ = ZSF3 * (SZ1+SZ2+SZ3) >> 12
    // (approx. dans cette version; OTZ est 16-bit en pratique)
    const uint32_t sz1 = data_[D_SZ1] & 0xFFFFu;
    const uint32_t sz2 = data_[D_SZ2] & 0xFFFFu;
    const uint32_t sz3 = data_[D_SZ3] & 0xFFFFu;
    const int32_t zsf3 = s16(ctrl_[C_ZSF3]);

    const int64_t sum = (int64_t)sz1 + (int64_t)sz2 + (int64_t)sz3;
    const int64_t mac0 = (int64_t)zsf3 * sum;
    set_mac(0, mac0);
    data_[D_OTZ] = clamp_u16((int32_t)(mac0 >> 12));
}

void Gte::cmd_avsz4(uint32_t)
{
    // AVSZ4: OTZ = ZSF4 * (SZ0+SZ1+SZ2+SZ3) >> 12
    const uint32_t sz0 = data_[D_SZ0] & 0xFFFFu;
    const uint32_t sz1 = data_[D_SZ1] & 0xFFFFu;
    const uint32_t sz2 = data_[D_SZ2] & 0xFFFFu;
    const uint32_t sz3 = data_[D_SZ3] & 0xFFFFu;
    const int32_t zsf4 = s16(ctrl_[C_ZSF4]);

    const int64_t sum = (int64_t)sz0 + (int64_t)sz1 + (int64_t)sz2 + (int64_t)sz3;
    const int64_t mac0 = (int64_t)zsf4 * sum;
    set_mac(0, mac0);
    data_[D_OTZ] = clamp_u16((int32_t)(mac0 >> 12));
}

void Gte::cmd_sqr(uint32_t cmd)
{
    // SQR: MACi = IRi * IRi (i=1..3), puis IRi = MACi >> (sf?12:0)
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int64_t mac1 = (int64_t)ir1 * (int64_t)ir1;
    const int64_t mac2 = (int64_t)ir2 * (int64_t)ir2;
    const int64_t mac3 = (int64_t)ir3 * (int64_t)ir3;
    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

void Gte::cmd_gpf(uint32_t cmd)
{
    // GPF: MACi = IRi * IR0, IRi = MACi >> (sf?12:0)
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int64_t mac1 = (int64_t)ir1 * ir0;
    const int64_t mac2 = (int64_t)ir2 * ir0;
    const int64_t mac3 = (int64_t)ir3 * ir0;
    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

void Gte::cmd_gpl(uint32_t cmd)
{
    // GPL: MACi = MACi + IRi*IR0, IRi = MACi >> (sf?12:0)
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int32_t mac1_old = (int32_t)data_[D_MAC1];
    const int32_t mac2_old = (int32_t)data_[D_MAC2];
    const int32_t mac3_old = (int32_t)data_[D_MAC3];

    const int64_t mac1 = (int64_t)mac1_old + (int64_t)ir1 * ir0;
    const int64_t mac2 = (int64_t)mac2_old + (int64_t)ir2 * ir0;
    const int64_t mac3 = (int64_t)mac3_old + (int64_t)ir3 * ir0;

    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

int Gte::execute(uint32_t cop2_instruction)
{
    // On extrait le champ "function" sur 6 bits (commande).
    const uint32_t cmd = cop2_instruction & 0x01FF'FFFFu;
    const uint32_t funct = cmd & 0x3Fu;

    switch (funct)
    {
        case 0x06: // NCLIP
            cmd_nclip(cmd);
            return 1;
        case 0x12: // MVMVA
            cmd_mvmva(cmd);
            return 1;
        case 0x01: // RTPS
            cmd_rtps(cmd);
            return 1;
        case 0x30: // RTPT
            cmd_rtpt(cmd);
            return 1;
        case 0x2D: // AVSZ3
            cmd_avsz3(cmd);
            return 1;
        case 0x2E: // AVSZ4
            cmd_avsz4(cmd);
            return 1;
        case 0x28: // SQR
            cmd_sqr(cmd);
            return 1;
        case 0x3D: // GPF
            cmd_gpf(cmd);
            return 1;
        case 0x3E: // GPL
            cmd_gpl(cmd);
            return 1;
        default:
            // Pour une base éducative, on ne “fake” pas: si non implémenté, on laisse MAC/IR
            // inchangés. Le CPU pourra décider de lever RI sur l'instruction COP2 correspondante si
            // voulu.
            return 0;
    }
}

} // namespace gte
