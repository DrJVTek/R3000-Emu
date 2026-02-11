#include "gte.h"
#include <algorithm>
#include "../log/emu_log.h"

namespace gte
{

// PS1 GTE UNR (Unsigned Newton-Raphson) lookup table for division
// This table is used for the approximation: 1/x ≈ (2 - x*unr[i]) * unr[i]
// Indexed by the 8 MSBs of the normalized divisor
static const uint8_t unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3,
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8,
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0,
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A,
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86,
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74,
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64,
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55,
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48,
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x40, 0x3F, 0x3E, 0x3D, 0x3D, 0x3C, 0x3B,
    0x3A, 0x3A, 0x39, 0x38, 0x37, 0x37, 0x36, 0x35, 0x35, 0x34, 0x33, 0x32, 0x32, 0x31, 0x30, 0x30,
    0x2F, 0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25,
    0x24, 0x24, 0x23, 0x22, 0x22, 0x21, 0x21, 0x20, 0x1F, 0x1F, 0x1E, 0x1E, 0x1D, 0x1C, 0x1C, 0x1B,
    0x1B, 0x1A, 0x19, 0x19, 0x18, 0x18, 0x17, 0x17, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x13, 0x12,
    0x12, 0x11, 0x10, 0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0B, 0x0A, 0x0A,
    0x09, 0x09, 0x08, 0x08, 0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02,
    0x01
};

// Count leading zeros (32-bit)
static inline int count_leading_zeros(uint32_t val)
{
    if (val == 0) return 32;
#if defined(__GNUC__) || defined(__clang__)
    return __builtin_clz(val);
#elif defined(_MSC_VER)
    unsigned long idx;
    _BitScanReverse(&idx, val);
    return 31 - idx;
#else
    int n = 0;
    if ((val & 0xFFFF0000) == 0) { n += 16; val <<= 16; }
    if ((val & 0xFF000000) == 0) { n += 8; val <<= 8; }
    if ((val & 0xF0000000) == 0) { n += 4; val <<= 4; }
    if ((val & 0xC0000000) == 0) { n += 2; val <<= 2; }
    if ((val & 0x80000000) == 0) { n += 1; }
    return n;
#endif
}

// PS1 GTE division - simple integer division (for debugging)
// TODO: Replace with UNR table for hardware accuracy once working
// Returns (H * 0x10000) / SZ3, clamped to 0x1FFFF
static uint32_t gte_divide(uint16_t h, uint16_t sz3)
{
    if (sz3 == 0) {
        return 0x1FFFF;
    }

    // Simple integer division: (h << 16) / sz3
    uint32_t quotient = ((uint32_t)h << 16) / (uint32_t)sz3;

    // Clamp to 17-bit max (0x1FFFF)
    if (quotient > 0x1FFFF) {
        quotient = 0x1FFFF;
    }

    return quotient;
}

Gte::Gte()
{
    emu::logf(emu::LogLevel::info, "GTE", "GTE source v4 (simple_div)");
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

void Gte::lwc2(uint32_t gte_reg, uint32_t word)
{
    write_data(gte_reg, word);
}

uint32_t Gte::swc2(uint32_t gte_reg) const
{
    return read_data(gte_reg);
}

// ---------------------------------------
// Helpers "couleur" (pédagogiques)
// ---------------------------------------
static inline uint8_t u8_clamp(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static inline void unpack_rgbc(uint32_t rgbc, int32_t& r, int32_t& g, int32_t& b, uint8_t& c)
{
    r = (int32_t)(rgbc & 0xFFu);
    g = (int32_t)((rgbc >> 8) & 0xFFu);
    b = (int32_t)((rgbc >> 16) & 0xFFu);
    c = (uint8_t)((rgbc >> 24) & 0xFFu);
}

static inline uint32_t pack_rgbc(uint8_t r, uint8_t g, uint8_t b, uint8_t c)
{
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)c << 24);
}

static inline int32_t fixed_mul8(int32_t a, int32_t b_q12)
{
    // a (0..255) * b (0..4096) -> 0..255 (>>12)
    return (int32_t)(((int64_t)a * (int64_t)b_q12) >> 12);
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

// Clamp screen coordinates to [-1024, 1023] as per PS1 hardware
static int32_t clamp_sxy(int32_t v)
{
    if (v < -1024) return -1024;
    if (v > 1023) return 1023;
    return v;
}

void Gte::push_sxy(int32_t sx, int32_t sy)
{
    // Pipeline interne GTE: SXY0 <= SXY1 <= SXY2 <= SXYP.
    // Clamp to 11-bit signed range [-1024, 1023] to avoid overflow wrapping
    sx = clamp_sxy(sx);
    sy = clamp_sxy(sy);
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

    // SZ3 must be computed from MAC3 >> sf, NOT from IR3!
    // psx-spx: SZ3 = Lm_D(MAC3 >> sf) where Lm_D clamps to [0, 0xFFFF]
    // IR3 = Lm_B3(MAC3 >> sf) which clamps to [-0x8000, 0x7FFF] (different!)
    const int sf = (cmd >> 19) & 1;
    const int shift = sf ? 12 : 0;
    const int64_t mac3 = (int32_t)data_[D_MAC3];  // Sign-extend from stored 32-bit
    const int32_t mac3_shifted = (int32_t)(mac3 >> shift);
    const uint32_t sz = (mac3_shifted < 0) ? 0 : ((mac3_shifted > 0xFFFF) ? 0xFFFF : (uint32_t)mac3_shifted);
    push_sz(sz);

    // Projection: SX = (OFX + IR1*H/SZ3) >> 16 ; SY = (OFY + IR2*H/SZ3) >> 16
    // OFX/OFY are in 16.16 fixed-point format, so final result must be >> 16
    const int64_t ofx = (int32_t)ctrl_[C_OFX];
    const int64_t ofy = (int32_t)ctrl_[C_OFY];
    const int32_t h = (int32_t)(ctrl_[C_H] & 0xFFFFu);

    // Use UNR table-based division (matches real PS1 hardware)
    uint32_t quotient = gte_divide((uint16_t)h, (uint16_t)sz);

    int32_t sx = (int32_t)((ofx + ir1 * (int64_t)quotient) >> 16);
    int32_t sy = (int32_t)((ofy + ir2 * (int64_t)quotient) >> 16);

    push_sxy(sx, sy);
}

void Gte::cmd_rtpt(uint32_t cmd)
{
    // RTPT: transform V0, V1, V2 and store results in SXY0, SXY1, SXY2.
    // We can't just call cmd_rtps 3 times because push_sxy is a shift register
    // that would leave results in wrong positions. Instead, compute directly.

    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;

    // Debug: log first few RTPT calls
    static int rtpt_log_count = 0;
    const bool do_log = (rtpt_log_count++ < 50);

    // Rotation matrix
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

    const int64_t ofx = (int32_t)ctrl_[C_OFX];
    const int64_t ofy = (int32_t)ctrl_[C_OFY];
    const int32_t h = (int32_t)(ctrl_[C_H] & 0xFFFFu);

    const int shift = sf ? 12 : 0;

    if (do_log)
    {
        emu::logf(emu::LogLevel::warn, "GTE", "RTPT: H=%d OFX=%lld OFY=%lld sf=%d",
            h, ofx, ofy, sf);
        emu::logf(emu::LogLevel::warn, "GTE", "  TR=(%d,%d,%d) R11=%d R22=%d R33=%d",
            trx, try_, trz, r11, r22, r33);
    }

    // Process all 3 vertices
    uint32_t sxy_results[3];
    uint32_t sz_results[3];

    for (int i = 0; i < 3; ++i)
    {
        const int32_t vxi = vx(i);
        const int32_t vyi = vy(i);
        const int32_t vzi = vz(i);

        // Rotation + translation
        const int64_t mac1 = (int64_t)r11 * vxi + (int64_t)r12 * vyi + (int64_t)r13 * vzi + ((int64_t)trx << 12);
        const int64_t mac2 = (int64_t)r21 * vxi + (int64_t)r22 * vyi + (int64_t)r23 * vzi + ((int64_t)try_ << 12);
        const int64_t mac3 = (int64_t)r31 * vxi + (int64_t)r32 * vyi + (int64_t)r33 * vzi + ((int64_t)trz << 12);

        const int32_t ir1 = clamp_s16((int32_t)(mac1 >> shift));
        const int32_t ir2 = clamp_s16((int32_t)(mac2 >> shift));
        const int32_t ir3 = clamp_s16((int32_t)(mac3 >> shift));

        // Z value for depth sorting - computed from MAC3 directly, NOT from IR3!
        // psx-spx: SZ3 = Lm_D(MAC3 >> sf) where Lm_D clamps to unsigned [0, 0xFFFF]
        // This is DIFFERENT from IR3 which clamps to signed [-0x8000, +0x7FFF]
        const int32_t mac3_shifted = (int32_t)(mac3 >> shift);
        const uint32_t sz = (mac3_shifted < 0) ? 0 : ((mac3_shifted > 0xFFFF) ? 0xFFFF : (uint32_t)mac3_shifted);
        sz_results[i] = sz;

        // Debug: log when sz=0 (will cause division overflow)
        if (sz == 0)
        {
            static int sz0_log_count = 0;
            if (sz0_log_count++ < 20)
            {
                emu::logf(emu::LogLevel::warn, "GTE", "RTPT SZ=0: V%d in=(%d,%d,%d) mac3=%lld mac3_shifted=%d trz=%d r3x=(%d,%d,%d) sf=%d",
                    i, vxi, vyi, vzi, (long long)mac3, mac3_shifted, trz, r31, r32, r33, shift);
            }
        }

        // Projection using UNR table-based division (matches real PS1 hardware)
        uint32_t quotient = gte_divide((uint16_t)h, (uint16_t)sz);
        int32_t sx = (int32_t)((ofx + ir1 * (int64_t)quotient) >> 16);
        int32_t sy = (int32_t)((ofy + ir2 * (int64_t)quotient) >> 16);

        // Clamp to 11-bit signed range [-1024, 1023]
        const int32_t sx_raw = sx, sy_raw = sy;
        sx = clamp_sxy(sx);
        sy = clamp_sxy(sy);
        sxy_results[i] = pack16(sx, sy);

        // Log when clamping occurs (indicates potential rendering issues)
        if (sx != sx_raw || sy != sy_raw)
        {
            static int clamp_log_count = 0;
            if (clamp_log_count++ < 100)
            {
                emu::logf(emu::LogLevel::warn, "GTE", "RTPT CLAMP: V%d in=(%d,%d,%d) sz=%u quotient=%u raw=(%d,%d)->clamped=(%d,%d)",
                    i, vxi, vyi, vzi, sz, quotient, sx_raw, sy_raw, sx, sy);
            }
        }

        if (do_log)
        {
            emu::logf(emu::LogLevel::warn, "GTE", "  V%d: in=(%d,%d,%d) ir=(%d,%d,%d) sz=%u sx/sy_raw=(%d,%d) clamped=(%d,%d)",
                i, vxi, vyi, vzi, ir1, ir2, ir3, sz, sx_raw, sy_raw, sx, sy);
        }
    }

    // Store results directly in correct registers (NOT using push_sxy shift register)
    data_[D_SXY0] = sxy_results[0];
    data_[D_SXY1] = sxy_results[1];
    data_[D_SXY2] = sxy_results[2];
    data_[D_SXYP] = sxy_results[2];  // SXYP mirrors SXY2

    // SZ pipeline: push all 3 values
    data_[D_SZ1] = sz_results[0];
    data_[D_SZ2] = sz_results[1];
    data_[D_SZ3] = sz_results[2];

    // Set final MAC/IR values from last vertex (V2)
    const int32_t vx2 = vx(2), vy2 = vy(2), vz2 = vz(2);
    const int64_t mac1 = (int64_t)r11 * vx2 + (int64_t)r12 * vy2 + (int64_t)r13 * vz2 + ((int64_t)trx << 12);
    const int64_t mac2 = (int64_t)r21 * vx2 + (int64_t)r22 * vy2 + (int64_t)r23 * vz2 + ((int64_t)try_ << 12);
    const int64_t mac3 = (int64_t)r31 * vx2 + (int64_t)r32 * vy2 + (int64_t)r33 * vz2 + ((int64_t)trz << 12);
    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
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

static inline void shift_rgb_pipeline(uint32_t* data)
{
    // RGB pipeline registers are data regs 20..22.
    data[20] = data[21];
    data[21] = data[22];
}

static inline void light_matrix_mul(
    const uint32_t* ctrl,
    int32_t nx,
    int32_t ny,
    int32_t nz,
    int32_t& out1,
    int32_t& out2,
    int32_t& out3
)
{
    // L matrix (3x3) packée en 16-bit:
    const int32_t l11 = (int32_t)(int16_t)(ctrl[8] & 0xFFFFu);
    const int32_t l12 = (int32_t)(int16_t)((ctrl[8] >> 16) & 0xFFFFu);
    const int32_t l13 = (int32_t)(int16_t)(ctrl[9] & 0xFFFFu);
    const int32_t l21 = (int32_t)(int16_t)((ctrl[9] >> 16) & 0xFFFFu);
    const int32_t l22 = (int32_t)(int16_t)(ctrl[10] & 0xFFFFu);
    const int32_t l23 = (int32_t)(int16_t)((ctrl[10] >> 16) & 0xFFFFu);
    const int32_t l31 = (int32_t)(int16_t)(ctrl[11] & 0xFFFFu);
    const int32_t l32 = (int32_t)(int16_t)((ctrl[11] >> 16) & 0xFFFFu);
    const int32_t l33 = (int32_t)(int16_t)(ctrl[12] & 0xFFFFu);

    const int64_t mac1 = (int64_t)l11 * nx + (int64_t)l12 * ny + (int64_t)l13 * nz;
    const int64_t mac2 = (int64_t)l21 * nx + (int64_t)l22 * ny + (int64_t)l23 * nz;
    const int64_t mac3 = (int64_t)l31 * nx + (int64_t)l32 * ny + (int64_t)l33 * nz;

    out1 = (int32_t)(mac1 >> 12);
    out2 = (int32_t)(mac2 >> 12);
    out3 = (int32_t)(mac3 >> 12);
}

void Gte::cmd_op(uint32_t cmd)
{
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t r11 = s16(ctrl_[C_R11R12]);
    const int32_t r22 = s16(ctrl_[C_R22R23]);
    const int32_t r33 = s16(ctrl_[C_R33]);

    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int64_t mac1 = (int64_t)r22 * ir3 - (int64_t)r33 * ir2;
    const int64_t mac2 = (int64_t)r33 * ir1 - (int64_t)r11 * ir3;
    const int64_t mac3 = (int64_t)r11 * ir2 - (int64_t)r22 * ir1;

    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

void Gte::cmd_dpcs(uint32_t)
{
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t fc_r = (int32_t)(ctrl_[C_RFC] & 0xFFu);
    const int32_t fc_g = (int32_t)(ctrl_[C_GFC] & 0xFFu);
    const int32_t fc_b = (int32_t)(ctrl_[C_BFC] & 0xFFu);

    const int32_t out_r = r + (int32_t)(((int64_t)(fc_r - r) * ir0) >> 12);
    const int32_t out_g = g + (int32_t)(((int64_t)(fc_g - g) * ir0) >> 12);
    const int32_t out_b = b + (int32_t)(((int64_t)(fc_b - b) * ir0) >> 12);

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(u8_clamp(out_r), u8_clamp(out_g), u8_clamp(out_b), code);
}

void Gte::cmd_intpl(uint32_t cmd)
{
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int32_t fc1 = ((int32_t)(ctrl_[C_RFC] & 0xFFu)) << 4;
    const int32_t fc2 = ((int32_t)(ctrl_[C_GFC] & 0xFFu)) << 4;
    const int32_t fc3 = ((int32_t)(ctrl_[C_BFC] & 0xFFu)) << 4;

    const int64_t mac1 = (int64_t)ir1 + (((int64_t)(fc1 - ir1) * ir0) >> 12);
    const int64_t mac2 = (int64_t)ir2 + (((int64_t)(fc2 - ir2) * ir0) >> 12);
    const int64_t mac3 = (int64_t)ir3 + (((int64_t)(fc3 - ir3) * ir0) >> 12);

    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> shift), lm);
    set_ir(2, (int32_t)(mac2 >> shift), lm);
    set_ir(3, (int32_t)(mac3 >> shift), lm);
}

void Gte::cmd_ncs(uint32_t cmd)
{
    const int lm = (cmd >> 10) & 1;

    // Step 1: IR = L * Normal (Light matrix * Normal vector)
    int32_t ir1, ir2, ir3;
    const int32_t nx = vx(0), ny = vy(0), nz = vz(0);
    light_matrix_mul(ctrl_, nx, ny, nz, ir1, ir2, ir3);

    set_ir(1, ir1, lm);
    set_ir(2, ir2, lm);
    set_ir(3, ir3, lm);

    // Step 2: MAC = BK + LC * IR (Background + Color matrix * light intensity)
    // Color matrix (LC) is 3x3, packed in ctrl regs 16-20
    const int32_t lr1 = s16(ctrl_[C_LR1LR2]);
    const int32_t lr2 = hi16(ctrl_[C_LR1LR2]);
    const int32_t lr3 = s16(ctrl_[C_LR3LG1]);
    const int32_t lg1 = hi16(ctrl_[C_LR3LG1]);
    const int32_t lg2 = s16(ctrl_[C_LG2LG3]);
    const int32_t lg3 = hi16(ctrl_[C_LG2LG3]);
    const int32_t lb1 = s16(ctrl_[C_LB1LB2]);
    const int32_t lb2 = hi16(ctrl_[C_LB1LB2]);
    const int32_t lb3 = s16(ctrl_[C_LB3]);

    // Background color (BK) - 32-bit values
    const int64_t rbk = (int32_t)ctrl_[C_RBK];
    const int64_t gbk = (int32_t)ctrl_[C_GBK];
    const int64_t bbk = (int32_t)ctrl_[C_BBK];

    // Get IR values after light matrix multiplication
    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    // MAC = BK + LC * IR (BK is shifted left by 12 bits in the calculation)
    const int64_t mac1 = (rbk << 12) + (int64_t)lr1 * i1 + (int64_t)lr2 * i2 + (int64_t)lr3 * i3;
    const int64_t mac2 = (gbk << 12) + (int64_t)lg1 * i1 + (int64_t)lg2 * i2 + (int64_t)lg3 * i3;
    const int64_t mac3 = (bbk << 12) + (int64_t)lb1 * i1 + (int64_t)lb2 * i2 + (int64_t)lb3 * i3;

    set_mac(1, mac1);
    set_mac(2, mac2);
    set_mac(3, mac3);
    set_ir(1, (int32_t)(mac1 >> 12), lm);
    set_ir(2, (int32_t)(mac2 >> 12), lm);
    set_ir(3, (int32_t)(mac3 >> 12), lm);

    // Step 3: RGB2 = MAC >> 4 (convert to 8-bit color, with saturation)
    uint8_t code = (uint8_t)((data_[D_RGBC] >> 24) & 0xFFu);
    const int32_t out_r = (int32_t)(mac1 >> 16);  // >> 12 then >> 4 = >> 16
    const int32_t out_g = (int32_t)(mac2 >> 16);
    const int32_t out_b = (int32_t)(mac3 >> 16);

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(u8_clamp(out_r), u8_clamp(out_g), u8_clamp(out_b), code);
}

void Gte::cmd_nct(uint32_t cmd)
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t save_vxy0 = data_[D_VXY0];
        const uint32_t save_vz0 = data_[D_VZ0];
        const uint32_t src_vxy = (i == 0) ? D_VXY0 : (i == 1) ? D_VXY1 : D_VXY2;
        const uint32_t src_vz = (i == 0) ? D_VZ0 : (i == 1) ? D_VZ1 : D_VZ2;
        data_[D_VXY0] = data_[src_vxy];
        data_[D_VZ0] = data_[src_vz];
        cmd_ncs(cmd);
        data_[D_VXY0] = save_vxy0;
        data_[D_VZ0] = save_vz0;
    }
}

void Gte::cmd_nccs(uint32_t cmd)
{
    // NCCS = NCS + CC (color modulation, NOT depth cueing)
    cmd_ncs(cmd);
    cmd_cc(cmd);
}

void Gte::cmd_ncct(uint32_t cmd)
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t save_vxy0 = data_[D_VXY0];
        const uint32_t save_vz0 = data_[D_VZ0];
        const uint32_t src_vxy = (i == 0) ? D_VXY0 : (i == 1) ? D_VXY1 : D_VXY2;
        const uint32_t src_vz = (i == 0) ? D_VZ0 : (i == 1) ? D_VZ1 : D_VZ2;
        data_[D_VXY0] = data_[src_vxy];
        data_[D_VZ0] = data_[src_vz];
        cmd_nccs(cmd);
        data_[D_VXY0] = save_vxy0;
        data_[D_VZ0] = save_vz0;
    }
}

void Gte::cmd_cc(uint32_t)
{
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int32_t out_r = fixed_mul8(r, i1);
    const int32_t out_g = fixed_mul8(g, i2);
    const int32_t out_b = fixed_mul8(b, i3);

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(u8_clamp(out_r), u8_clamp(out_g), u8_clamp(out_b), code);
}

void Gte::cmd_ncds(uint32_t cmd)
{
    // NCDS = NCS (lighting) + CC (color modulation) + DPCS (depth cue)
    // Step 1: NCS calculates light intensities in IR1/IR2/IR3
    // But we don't want NCS to write RGB2 yet, so we do the lighting inline:
    const int lm = (cmd >> 10) & 1;

    // Light matrix * Normal -> IR (light intensities)
    const int32_t nx = vx(0), ny = vy(0), nz = vz(0);
    int32_t ir1, ir2, ir3;

    // L matrix
    const int32_t l11 = s16(ctrl_[C_L11L12]);
    const int32_t l12 = hi16(ctrl_[C_L11L12]);
    const int32_t l13 = s16(ctrl_[C_L13L21]);
    const int32_t l21 = hi16(ctrl_[C_L13L21]);
    const int32_t l22 = s16(ctrl_[C_L22L23]);
    const int32_t l23 = hi16(ctrl_[C_L22L23]);
    const int32_t l31 = s16(ctrl_[C_L31L32]);
    const int32_t l32 = hi16(ctrl_[C_L31L32]);
    const int32_t l33 = s16(ctrl_[C_L33]);

    int64_t mac1 = (int64_t)l11 * nx + (int64_t)l12 * ny + (int64_t)l13 * nz;
    int64_t mac2 = (int64_t)l21 * nx + (int64_t)l22 * ny + (int64_t)l23 * nz;
    int64_t mac3 = (int64_t)l31 * nx + (int64_t)l32 * ny + (int64_t)l33 * nz;
    ir1 = clamp_s16((int32_t)(mac1 >> 12));
    ir2 = clamp_s16((int32_t)(mac2 >> 12));
    ir3 = clamp_s16((int32_t)(mac3 >> 12));
    if (lm) { ir1 = (std::max)(0, ir1); ir2 = (std::max)(0, ir2); ir3 = (std::max)(0, ir3); }

    // BK + LC * IR -> MAC (lit color before vertex color)
    const int32_t lr1 = s16(ctrl_[C_LR1LR2]);
    const int32_t lr2 = hi16(ctrl_[C_LR1LR2]);
    const int32_t lr3 = s16(ctrl_[C_LR3LG1]);
    const int32_t lg1 = hi16(ctrl_[C_LR3LG1]);
    const int32_t lg2 = s16(ctrl_[C_LG2LG3]);
    const int32_t lg3 = hi16(ctrl_[C_LG2LG3]);
    const int32_t lb1 = s16(ctrl_[C_LB1LB2]);
    const int32_t lb2 = hi16(ctrl_[C_LB1LB2]);
    const int32_t lb3 = s16(ctrl_[C_LB3]);
    const int64_t rbk = (int32_t)ctrl_[C_RBK];
    const int64_t gbk = (int32_t)ctrl_[C_GBK];
    const int64_t bbk = (int32_t)ctrl_[C_BBK];

    mac1 = (rbk << 12) + (int64_t)lr1 * ir1 + (int64_t)lr2 * ir2 + (int64_t)lr3 * ir3;
    mac2 = (gbk << 12) + (int64_t)lg1 * ir1 + (int64_t)lg2 * ir2 + (int64_t)lg3 * ir3;
    mac3 = (bbk << 12) + (int64_t)lb1 * ir1 + (int64_t)lb2 * ir2 + (int64_t)lb3 * ir3;
    ir1 = clamp_s16((int32_t)(mac1 >> 12));
    ir2 = clamp_s16((int32_t)(mac2 >> 12));
    ir3 = clamp_s16((int32_t)(mac3 >> 12));
    if (lm) { ir1 = (std::max)(0, ir1); ir2 = (std::max)(0, ir2); ir3 = (std::max)(0, ir3); }

    // Step 2: CC - multiply RGBC by IR (color modulation)
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t col_r = (r * ir1) >> 12;  // RGBC * IR >> 12 (IR is 1.3.12 format)
    const int32_t col_g = (g * ir2) >> 12;
    const int32_t col_b = (b * ir3) >> 12;

    // Step 3: Depth cueing - interpolate toward Far Color using IR0
    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t fc_r = (int32_t)(ctrl_[C_RFC] & 0xFFu);
    const int32_t fc_g = (int32_t)(ctrl_[C_GFC] & 0xFFu);
    const int32_t fc_b = (int32_t)(ctrl_[C_BFC] & 0xFFu);

    const int32_t out_r = col_r + (int32_t)(((int64_t)(fc_r - col_r) * ir0) >> 12);
    const int32_t out_g = col_g + (int32_t)(((int64_t)(fc_g - col_g) * ir0) >> 12);
    const int32_t out_b = col_b + (int32_t)(((int64_t)(fc_b - col_b) * ir0) >> 12);

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(u8_clamp(out_r), u8_clamp(out_g), u8_clamp(out_b), code);

    // Update IR/MAC registers
    set_ir(1, ir1, lm);
    set_ir(2, ir2, lm);
    set_ir(3, ir3, lm);
}

void Gte::cmd_ncdt(uint32_t cmd)
{
    for (uint32_t i = 0; i < 3; ++i)
    {
        const uint32_t save_vxy0 = data_[D_VXY0];
        const uint32_t save_vz0 = data_[D_VZ0];
        const uint32_t src_vxy = (i == 0) ? D_VXY0 : (i == 1) ? D_VXY1 : D_VXY2;
        const uint32_t src_vz = (i == 0) ? D_VZ0 : (i == 1) ? D_VZ1 : D_VZ2;
        data_[D_VXY0] = data_[src_vxy];
        data_[D_VZ0] = data_[src_vz];
        cmd_ncds(cmd);
        data_[D_VXY0] = save_vxy0;
        data_[D_VZ0] = save_vz0;
    }
}

void Gte::cmd_cdp(uint32_t cmd)
{
    cmd_cc(cmd);
    cmd_dpcs(cmd);
}

void Gte::cmd_dcpl(uint32_t cmd)
{
    const uint32_t save = data_[D_RGBC];
    data_[D_RGBC] = data_[D_RGB2];
    cmd_dpcs(cmd);
    data_[D_RGBC] = save;
}

void Gte::cmd_dpct(uint32_t cmd)
{
    const uint32_t save = data_[D_RGBC];
    for (int i = 0; i < 3; ++i)
    {
        data_[D_RGBC] = data_[D_RGB0 + (uint32_t)i];
        cmd_dpcs(cmd);
    }
    data_[D_RGBC] = save;
}

int Gte::execute(uint32_t cop2_instruction)
{
    // On extrait le champ "function" sur 6 bits (commande).
    const uint32_t cmd = cop2_instruction & 0x01FF'FFFFu;
    const uint32_t funct = cmd & 0x3Fu;

    // Debug counter for lighting commands
    static int light_log_count = 0;

    switch (funct)
    {
        case 0x06: // NCLIP
            cmd_nclip(cmd);
            return 1;
        case 0x0C: // OP
            cmd_op(cmd);
            return 1;
        case 0x10: // DPCS
            cmd_dpcs(cmd);
            return 1;
        case 0x11: // INTPL
            cmd_intpl(cmd);
            return 1;
        case 0x12: // MVMVA
            cmd_mvmva(cmd);
            return 1;
        case 0x13: // NCDS
            cmd_ncds(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCDS: RGBC=%08X -> RGB2=%08X", data_[D_RGBC], data_[D_RGB2]);
            return 1;
        case 0x14: // CDP
            cmd_cdp(cmd);
            return 1;
        case 0x16: // NCDT
            cmd_ncdt(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCDT: -> RGB0=%08X RGB1=%08X RGB2=%08X",
                    data_[D_RGB0], data_[D_RGB1], data_[D_RGB2]);
            return 1;
        case 0x1B: // NCCS
            cmd_nccs(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCCS: RGBC=%08X -> RGB2=%08X", data_[D_RGBC], data_[D_RGB2]);
            return 1;
        case 0x1C: // CC
            cmd_cc(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "CC: RGBC=%08X IR=(%d,%d,%d) -> RGB2=%08X",
                    data_[D_RGBC], (int16_t)data_[D_IR1], (int16_t)data_[D_IR2], (int16_t)data_[D_IR3], data_[D_RGB2]);
            return 1;
        case 0x1E: // NCS
            cmd_ncs(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCS: V0=(%d,%d,%d) -> RGB2=%08X",
                    vx(0), vy(0), vz(0), data_[D_RGB2]);
            return 1;
        case 0x20: // NCT
            cmd_nct(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCT: -> RGB0=%08X RGB1=%08X RGB2=%08X",
                    data_[D_RGB0], data_[D_RGB1], data_[D_RGB2]);
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
        case 0x29: // DCPL
            cmd_dcpl(cmd);
            return 1;
        case 0x2A: // DPCT
            cmd_dpct(cmd);
            return 1;
        case 0x3D: // GPF
            cmd_gpf(cmd);
            return 1;
        case 0x3E: // GPL
            cmd_gpl(cmd);
            return 1;
        case 0x3F: // NCCT
            cmd_ncct(cmd);
            if (light_log_count++ < 20)
                emu::logf(emu::LogLevel::warn, "GTE", "NCCT: -> RGB0=%08X RGB1=%08X RGB2=%08X",
                    data_[D_RGB0], data_[D_RGB1], data_[D_RGB2]);
            return 1;
        default:
            // Pour une base éducative, on ne “fake” pas: si non implémenté, on laisse MAC/IR
            // inchangés. Le CPU pourra décider de lever RI sur l'instruction COP2 correspondante si
            // voulu.
            return 0;
    }
}

} // namespace gte
