#include "gte.h"
#include <algorithm>
#include "../log/emu_log.h"

namespace gte
{

// PS1 GTE UNR (Unsigned Newton-Raphson) lookup table - exact hardware values
// (257 entries, from DuckStation / psx-spx)
static const uint8_t unr_table[257] = {
    0xFF, 0xFD, 0xFB, 0xF9, 0xF7, 0xF5, 0xF3, 0xF1, 0xEF, 0xEE, 0xEC, 0xEA, 0xE8, 0xE6, 0xE4, 0xE3, //
    0xE1, 0xDF, 0xDD, 0xDC, 0xDA, 0xD8, 0xD6, 0xD5, 0xD3, 0xD1, 0xD0, 0xCE, 0xCD, 0xCB, 0xC9, 0xC8, //  00h..3Fh
    0xC6, 0xC5, 0xC3, 0xC1, 0xC0, 0xBE, 0xBD, 0xBB, 0xBA, 0xB8, 0xB7, 0xB5, 0xB4, 0xB2, 0xB1, 0xB0, //
    0xAE, 0xAD, 0xAB, 0xAA, 0xA9, 0xA7, 0xA6, 0xA4, 0xA3, 0xA2, 0xA0, 0x9F, 0x9E, 0x9C, 0x9B, 0x9A, //
    0x99, 0x97, 0x96, 0x95, 0x94, 0x92, 0x91, 0x90, 0x8F, 0x8D, 0x8C, 0x8B, 0x8A, 0x89, 0x87, 0x86, //
    0x85, 0x84, 0x83, 0x82, 0x81, 0x7F, 0x7E, 0x7D, 0x7C, 0x7B, 0x7A, 0x79, 0x78, 0x77, 0x75, 0x74, //  40h..7Fh
    0x73, 0x72, 0x71, 0x70, 0x6F, 0x6E, 0x6D, 0x6C, 0x6B, 0x6A, 0x69, 0x68, 0x67, 0x66, 0x65, 0x64, //
    0x63, 0x62, 0x61, 0x60, 0x5F, 0x5E, 0x5D, 0x5D, 0x5C, 0x5B, 0x5A, 0x59, 0x58, 0x57, 0x56, 0x55, //
    0x54, 0x53, 0x53, 0x52, 0x51, 0x50, 0x4F, 0x4E, 0x4D, 0x4D, 0x4C, 0x4B, 0x4A, 0x49, 0x48, 0x48, //
    0x47, 0x46, 0x45, 0x44, 0x43, 0x43, 0x42, 0x41, 0x40, 0x3F, 0x3F, 0x3E, 0x3D, 0x3C, 0x3C, 0x3B, //  80h..BFh
    0x3A, 0x39, 0x39, 0x38, 0x37, 0x36, 0x36, 0x35, 0x34, 0x33, 0x33, 0x32, 0x31, 0x31, 0x30, 0x2F, //
    0x2E, 0x2E, 0x2D, 0x2C, 0x2C, 0x2B, 0x2A, 0x2A, 0x29, 0x28, 0x28, 0x27, 0x26, 0x26, 0x25, 0x24, //
    0x24, 0x23, 0x22, 0x22, 0x21, 0x20, 0x20, 0x1F, 0x1E, 0x1E, 0x1D, 0x1D, 0x1C, 0x1B, 0x1B, 0x1A, //
    0x19, 0x19, 0x18, 0x18, 0x17, 0x16, 0x16, 0x15, 0x15, 0x14, 0x14, 0x13, 0x12, 0x12, 0x11, 0x11, //  C0h..FFh
    0x10, 0x0F, 0x0F, 0x0E, 0x0E, 0x0D, 0x0D, 0x0C, 0x0C, 0x0B, 0x0A, 0x0A, 0x09, 0x09, 0x08, 0x08, //
    0x07, 0x07, 0x06, 0x06, 0x05, 0x05, 0x04, 0x04, 0x03, 0x03, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, //
    0x00 // extra entry for index 256
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

// Hardware-accurate PS1 GTE UNR division (matches DuckStation exactly)
// Uses Newton-Raphson reciprocal approximation with the UNR lookup table.
// flag_out: bit set if divide overflow
static uint32_t gte_divide(uint32_t h, uint32_t sz3, uint32_t& flag_out)
{
    if (sz3 * 2 <= h)
    {
        flag_out |= Gte::FLAG_DIV_OFLOW;
        // INSTRUMENTATION (temporary, near-clip dino debug):
        // Count how often the divide-overflow path is hit. Log a burst at the
        // start, then a periodic summary, so we know whether close-up vertices
        // routinely trigger this on TREX.
        static uint32_t div_oflow_count = 0;
        static uint32_t div_oflow_total = 0;
        ++div_oflow_total;
        if (div_oflow_count < 5)
        {
            ++div_oflow_count;
            emu::logf(emu::LogLevel::warn, "GTE",
                "DIV_OFLOW #%u total=%u h=%u sz3=%u", div_oflow_count, div_oflow_total, h, sz3);
        }
        else if ((div_oflow_total % 200) == 0)
        {
            emu::logf(emu::LogLevel::warn, "GTE",
                "DIV_OFLOW total=%u (last h=%u sz3=%u)", div_oflow_total, h, sz3);
        }
        return 0x1FFFF;
    }

    // Count leading zeros of 16-bit SZ3
    const uint32_t shift = (sz3 == 0) ? 16 : (uint32_t)count_leading_zeros((uint16_t)sz3) - 16;
    uint32_t lhs = h << shift;
    uint32_t rhs = sz3 << shift;

    // Newton-Raphson: approximate 1/rhs using UNR table
    const uint32_t divisor = rhs | 0x8000;
    const int32_t x = (int32_t)(0x101 + (uint32_t)unr_table[((divisor & 0x7FFF) + 0x40) >> 7]);
    const int32_t d = ((int32_t)(divisor) * -x + 0x80) >> 8;
    const uint32_t recip = (uint32_t)((x * (0x20000 + d) + 0x80) >> 8);

    // Final: (lhs * recip + 0x8000) >> 16
    const uint32_t result = (uint32_t)(((uint64_t)lhs * (uint64_t)recip + 0x8000ULL) >> 16);

    return (result < 0x1FFFF) ? result : 0x1FFFF;
}

Gte::Gte()
{
    emu::logf(emu::LogLevel::warn, "GTE", "GTE v3 (AVSZ3 error-level instrumented)");
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
    idx &= 31u;
    switch (idx)
    {
    case D_SXYP: // reg 15: mirror of SXY2 (DuckStation: return r32[14])
        return data_[D_SXY2];
    case D_IRGB: // reg 28: computed from IR1/IR2/IR3 (same as ORGB)
    case D_ORGB: // reg 29: computed from IR1/IR2/IR3
    {
        // DuckStation: clamp(IR / 0x80, 0, 0x1F) — signed division
        const auto c5 = [](int32_t ir) -> uint32_t {
            int32_t v = ir / 0x80;
            if (v < 0) v = 0;
            if (v > 0x1F) v = 0x1F;
            return (uint32_t)v;
        };
        return c5((int32_t)data_[D_IR1])
             | (c5((int32_t)data_[D_IR2]) << 5)
             | (c5((int32_t)data_[D_IR3]) << 10);
    }
    case D_LZCR: // reg 31: count leading zeros/ones of LZCS
    {
        const uint32_t lzcs = data_[D_LZCS];
        uint32_t val = ((int32_t)lzcs < 0) ? ~lzcs : lzcs;
        return (val == 0u) ? 32 : count_leading_zeros(val);
    }
    default:
        return data_[idx];
    }
}

void Gte::write_data(uint32_t idx, uint32_t v)
{
    idx &= 31u;
    switch (idx)
    {
    case 1:  // VZ0
    case 3:  // VZ1
    case 5:  // VZ2
    case 8:  // IR0
    case 9:  // IR1
    case 10: // IR2
    case 11: // IR3
        // sign-extend 16-bit value to 32-bit (DuckStation behavior)
        data_[idx] = (uint32_t)(int32_t)(int16_t)(uint16_t)v;
        return;
    case 7:  // OTZ
    case 16: // SZ0
    case 17: // SZ1
    case 18: // SZ2
    case 19: // SZ3
        // zero-extend 16-bit value (DuckStation behavior)
        data_[idx] = (uint32_t)(uint16_t)v;
        return;
    case D_SXYP: // reg 15: writing pushes the SXY FIFO
        data_[D_SXY0] = data_[D_SXY1];
        data_[D_SXY1] = data_[D_SXY2];
        data_[D_SXY2] = v;
        return;
    case D_IRGB: // reg 28: packed 5-bit per component → sets IR1/IR2/IR3
        data_[D_IRGB] = v & 0x7FFFu;
        data_[D_IR1] = (uint32_t)(int32_t)(int16_t)(uint16_t)((v & 0x1Fu) * 0x80u);
        data_[D_IR2] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v >> 5) & 0x1Fu) * 0x80u);
        data_[D_IR3] = (uint32_t)(int32_t)(int16_t)(uint16_t)(((v >> 10) & 0x1Fu) * 0x80u);
        return;
    case D_LZCS: // reg 30: store value, LZCR computed on read
        data_[D_LZCS] = v;
        return;
    case D_ORGB: // reg 29: read-only
    case D_LZCR: // reg 31: read-only
        return;
    default:
        data_[idx] = v;
        return;
    }
}

uint32_t Gte::read_ctrl(uint32_t idx) const
{
    return ctrl_[idx & 31u];
}

void Gte::write_ctrl(uint32_t idx, uint32_t v)
{
    idx &= 31u;
    switch (idx)
    {
    // Single 16-bit control regs that need sign-extension (DuckStation behavior)
    case 4:  // R33     (C_R33)
    case 12: // L33     (C_L33)
    case 20: // LB3/LR33 (C_LB3)
    case 26: // H       (C_H)
    case 27: // DQA     (C_DQA)
    case 29: // ZSF3    (C_ZSF3)
    case 30: // ZSF4    (C_ZSF4)
        ctrl_[idx] = (uint32_t)(int32_t)(int16_t)(uint16_t)v;
        return;
    case 31: // FLAG
        ctrl_[idx] = v & 0x7FFFF000u;
        // Update error bit (bit 31 = OR of error bits)
        if (ctrl_[idx] & FLAG_ERROR_BITS)
            ctrl_[idx] |= (1u << 31);
        return;
    default:
        ctrl_[idx] = v;
        return;
    }
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
    if (sx < -1024) { flag_ |= FLAG_SX2_SAT; sx = -1024; }
    else if (sx > 1023) { flag_ |= FLAG_SX2_SAT; sx = 1023; }
    if (sy < -1024) { flag_ |= FLAG_SY2_SAT; sy = -1024; }
    else if (sy > 1023) { flag_ |= FLAG_SY2_SAT; sy = 1023; }
    const uint32_t val = pack16(sx, sy);
    data_[D_SXY0] = data_[D_SXY1];
    data_[D_SXY1] = data_[D_SXY2];
    data_[D_SXY2] = val;
}

void Gte::push_sz(int32_t sz)
{
    if (sz < 0) { flag_ |= FLAG_SZ3_OTZ_SAT; sz = 0; }
    else if (sz > 0xFFFF) { flag_ |= FLAG_SZ3_OTZ_SAT; sz = 0xFFFF; }
    data_[D_SZ0] = data_[D_SZ1];
    data_[D_SZ1] = data_[D_SZ2];
    data_[D_SZ2] = data_[D_SZ3];
    data_[D_SZ3] = (uint32_t)sz;
}

static inline void shift_rgb_pipeline(uint32_t* data)
{
    // RGB pipeline registers are data regs 20..22.
    data[20] = data[21];
    data[21] = data[22];
}

void Gte::push_color(int32_t r, int32_t g, int32_t b, uint8_t code)
{
    // Clamp to 0..255 and set FLAG_COLOR bits on saturation
    uint8_t cr, cg, cb;
    if (r < 0) { cr = 0; flag_ |= FLAG_COLOR_R; }
    else if (r > 255) { cr = 255; flag_ |= FLAG_COLOR_R; }
    else cr = (uint8_t)r;

    if (g < 0) { cg = 0; flag_ |= FLAG_COLOR_G; }
    else if (g > 255) { cg = 255; flag_ |= FLAG_COLOR_G; }
    else cg = (uint8_t)g;

    if (b < 0) { cb = 0; flag_ |= FLAG_COLOR_B; }
    else if (b > 255) { cb = 255; flag_ |= FLAG_COLOR_B; }
    else cb = (uint8_t)b;

    // Diagnostic: log first 20 color outputs + any black results (tunnels debug)
    static int color_log_count = 0;
    const bool is_black = (cr == 0 && cg == 0 && cb == 0);
    static int black_count = 0;
    if (color_log_count < 20 || (is_black && black_count < 10))
    {
        if (is_black) ++black_count;
        ++color_log_count;
        emu::logf(emu::LogLevel::warn, "GTE",
            "push_color #%d: raw(%d,%d,%d) -> clamped(%u,%u,%u) code=%02X RGBC=0x%08X IR=(%d,%d,%d) MAC=(%d,%d,%d) FC=(%d,%d,%d) IR0=%d%s",
            color_log_count, r, g, b, cr, cg, cb, code,
            data_[D_RGBC],
            (int32_t)data_[D_IR1], (int32_t)data_[D_IR2], (int32_t)data_[D_IR3],
            (int32_t)data_[D_MAC1], (int32_t)data_[D_MAC2], (int32_t)data_[D_MAC3],
            (int32_t)ctrl_[C_RFC], (int32_t)ctrl_[C_GFC], (int32_t)ctrl_[C_BFC],
            (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu),
            is_black ? " BLACK!" : "");
    }

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(cr, cg, cb, code);
}

// check_mac_overflow: check 44-bit overflow on the FULL (pre-shift) value
void Gte::check_mac_overflow(int idx, int64_t raw)
{
    if (idx == 0)
    {
        if (raw > (int64_t)INT32_MAX)  flag_ |= FLAG_MAC0_OFLOW_POS;
        if (raw < (int64_t)INT32_MIN)  flag_ |= FLAG_MAC0_OFLOW_NEG;
    }
    else
    {
        static constexpr int64_t MAC_MAX = (int64_t(1) << 43) - 1;
        static constexpr int64_t MAC_MIN = -(int64_t(1) << 43);
        const uint32_t pos_flag = (idx == 1) ? FLAG_MAC1_OFLOW_POS :
                                  (idx == 2) ? FLAG_MAC2_OFLOW_POS : FLAG_MAC3_OFLOW_POS;
        const uint32_t neg_flag = (idx == 1) ? FLAG_MAC1_OFLOW_NEG :
                                  (idx == 2) ? FLAG_MAC2_OFLOW_NEG : FLAG_MAC3_OFLOW_NEG;
        if (raw > MAC_MAX) flag_ |= pos_flag;
        if (raw < MAC_MIN) flag_ |= neg_flag;
    }
}

// sign_extend_mac: Emulates the 44-bit accumulator on real GTE hardware.
// DuckStation calls this SignExtendMACResult — it checks overflow flags on the
// intermediate value, then sign-extends (truncates) to 44 bits. This MUST be
// called between each addition in a dot-product to match hardware behavior.
int64_t Gte::sign_extend_mac(int idx, int64_t v)
{
    check_mac_overflow(idx, v);
    // Sign-extend from 44 bits: shift left 20 then arithmetic shift right 20
    return (v << (64 - 44)) >> (64 - 44);
}

// set_mac: DuckStation behavior - check overflow on raw value, store shifted value
void Gte::set_mac(int idx, int64_t v)
{
    check_mac_overflow(idx, v);
    data_[D_MAC0 + (uint32_t)idx] = (uint32_t)(int32_t)v;  // Truncate to 32-bit
}

// set_mac_shifted: check overflow on raw (pre-shift), store raw >> shift
// This matches DuckStation's TruncateAndSetMAC which checks BEFORE shifting
void Gte::set_mac_shifted(int idx, int64_t raw, int shift)
{
    check_mac_overflow(idx, raw);
    data_[D_MAC0 + (uint32_t)idx] = (uint32_t)(int32_t)(raw >> shift);
}

void Gte::set_ir(int idx, int32_t v, int lm)
{
    const int32_t lo = (idx == 0) ? 0 : (lm ? 0 : -32768);
    const int32_t hi = (idx == 0) ? 0x1000 : 32767;
    int32_t out = v;
    if (out < lo || out > hi)
    {
        // Set saturation flag
        const uint32_t sat_flag = (idx == 0) ? FLAG_IR0_SAT :
                                  (idx == 1) ? FLAG_IR1_SAT :
                                  (idx == 2) ? FLAG_IR2_SAT : FLAG_IR3_SAT;
        flag_ |= sat_flag;
        if (out < lo) out = lo;
        else          out = hi;
    }
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

    // INSTRUMENTATION (temporary, near-clip dino debug):
    // Log when NCLIP returns ≤ 0 (game culls) AND any vertex looks saturated
    // (= reaches the [-1024..1023] bound). Rate-limited to ~10 / frame to
    // avoid spam. The static counter resets every ~1000 calls; in practice
    // this means a small burst per frame which is enough to spot the pattern.
    static uint32_t nclip_cull_log_count = 0;
    static uint32_t nclip_cull_log_total = 0;
    if (n <= 0)
    {
        const bool sat0 = (x0 == -1024 || x0 == 1023 || y0 == -1024 || y0 == 1023);
        const bool sat1 = (x1 == -1024 || x1 == 1023 || y1 == -1024 || y1 == 1023);
        const bool sat2 = (x2 == -1024 || x2 == 1023 || y2 == -1024 || y2 == 1023);
        if ((sat0 || sat1 || sat2) && nclip_cull_log_count < 10)
        {
            ++nclip_cull_log_count;
            ++nclip_cull_log_total;
            emu::logf(emu::LogLevel::warn, "GTE",
                "NCLIP cull #%u (sat) MAC0=%lld v0=(%d,%d) v1=(%d,%d) v2=(%d,%d)",
                nclip_cull_log_total, (long long)n, x0, y0, x1, y1, x2, y2);
        }
    }
    // Periodic reset so each "burst" of frames produces fresh logs.
    static uint32_t nclip_call_count = 0;
    if (++nclip_call_count >= 1000)
    {
        nclip_call_count = 0;
        nclip_cull_log_count = 0;
    }
}

void Gte::cmd_mvmva(uint32_t cmd)
{
    // MVMVA: configurable matrix * vector + translation
    // Bits 17-18: multiply matrix (0=RT, 1=LLM, 2=LCM, 3=buggy)
    // Bits 15-16: multiply vector (0=V0, 1=V1, 2=V2, 3=IR)
    // Bits 13-14: translation vector (0=TR, 1=BK, 2=FC, 3=zero)
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int mx = (cmd >> 17) & 3;
    const int vv = (cmd >> 15) & 3;
    const int tv = (cmd >> 13) & 3;

    // Select matrix (3x3 packed as 16-bit pairs)
    int32_t m[3][3];
    uint32_t m_base;
    switch (mx) {
        case 0: m_base = C_R11R12; break; // Rotation
        case 1: m_base = C_L11L12; break; // Light
        case 2: m_base = C_LR1LR2; break; // Color
        default: // Buggy matrix (mx=3) - matches PS1 hardware bug
            m[0][0] = -(int16_t)((uint16_t)(data_[D_RGBC] & 0xFF) << 4);
            m[0][1] = (int16_t)((uint16_t)(data_[D_RGBC] & 0xFF) << 4);
            m[0][2] = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFF);
            m[1][0] = s16(ctrl_[C_R13R21]); // R13
            m[1][1] = s16(ctrl_[C_R13R21]); // R13 (duplicated, hardware bug)
            m[1][2] = s16(ctrl_[C_R13R21]); // R13 (duplicated)
            m[2][0] = hi16(ctrl_[C_R22R23]); // R23
            m[2][1] = hi16(ctrl_[C_R22R23]); // R23 (duplicated)
            m[2][2] = hi16(ctrl_[C_R22R23]); // R23 (duplicated)
            m_base = 0xFF; // sentinel
            break;
    }
    if (mx < 3) {
        m[0][0] = s16(ctrl_[m_base + 0]);
        m[0][1] = hi16(ctrl_[m_base + 0]);
        m[0][2] = s16(ctrl_[m_base + 1]);
        m[1][0] = hi16(ctrl_[m_base + 1]);
        m[1][1] = s16(ctrl_[m_base + 2]);
        m[1][2] = hi16(ctrl_[m_base + 2]);
        m[2][0] = s16(ctrl_[m_base + 3]);
        m[2][1] = hi16(ctrl_[m_base + 3]);
        m[2][2] = s16(ctrl_[m_base + 4]);
    }

    // Select vector
    int32_t Vx, Vy, Vz;
    switch (vv) {
        case 0: Vx = vx(0); Vy = vy(0); Vz = vz(0); break;
        case 1: Vx = vx(1); Vy = vy(1); Vz = vz(1); break;
        case 2: Vx = vx(2); Vy = vy(2); Vz = vz(2); break;
        default: // IR vector
            Vx = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
            Vy = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
            Vz = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);
            break;
    }

    // Select translation vector
    int32_t T[3];
    switch (tv) {
        case 0: T[0] = (int32_t)ctrl_[C_TRX]; T[1] = (int32_t)ctrl_[C_TRY]; T[2] = (int32_t)ctrl_[C_TRZ]; break;
        case 1: T[0] = (int32_t)ctrl_[C_RBK]; T[1] = (int32_t)ctrl_[C_GBK]; T[2] = (int32_t)ctrl_[C_BBK]; break;
        case 2: T[0] = (int32_t)ctrl_[C_RFC]; T[1] = (int32_t)ctrl_[C_GFC]; T[2] = (int32_t)ctrl_[C_BFC]; break;
        default: T[0] = 0; T[1] = 0; T[2] = 0; break;
    }

    // MAC = T*4096 + M*V (translation is shifted left by 12)
    // For tv=2 (FC), DuckStation uses a special "buggy" path.
    // We implement the standard path for now which works for most games.
    const int64_t mac1 = sign_extend_mac(1, sign_extend_mac(1, ((int64_t)T[0] << 12) + (int64_t)m[0][0] * Vx) + (int64_t)m[0][1] * Vy) + (int64_t)m[0][2] * Vz;
    const int64_t mac2 = sign_extend_mac(2, sign_extend_mac(2, ((int64_t)T[1] << 12) + (int64_t)m[1][0] * Vx) + (int64_t)m[1][1] * Vy) + (int64_t)m[1][2] * Vz;
    const int64_t mac3 = sign_extend_mac(3, sign_extend_mac(3, ((int64_t)T[2] << 12) + (int64_t)m[2][0] * Vx) + (int64_t)m[2][1] * Vy) + (int64_t)m[2][2] * Vz;

    const int shift = sf ? 12 : 0;
    set_mac_shifted(1, mac1, shift);
    set_mac_shifted(2, mac2, shift);
    set_mac_shifted(3, mac3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
}

// Internal RTPS that processes a single vertex (V[3]) with given shift/lm.
// 'last' controls whether DQA/DQB depth cueing is computed (only on last vertex).
void Gte::rtps_internal(const int32_t V[3], int sf, int lm, bool last)
{
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

    const int shift = sf ? 12 : 0;

    // dot3: (TR << 12) + R * V  (full 64-bit precision, matches DuckStation)
    const int64_t x = sign_extend_mac(1, sign_extend_mac(1, ((int64_t)trx << 12) + (int64_t)r11 * V[0]) + (int64_t)r12 * V[1]) + (int64_t)r13 * V[2];
    const int64_t y = sign_extend_mac(2, sign_extend_mac(2, ((int64_t)try_ << 12) + (int64_t)r21 * V[0]) + (int64_t)r22 * V[1]) + (int64_t)r23 * V[2];
    const int64_t z = sign_extend_mac(3, sign_extend_mac(3, ((int64_t)trz << 12) + (int64_t)r31 * V[0]) + (int64_t)r32 * V[1]) + (int64_t)r33 * V[2];

    // DuckStation: check 44-bit overflow on raw value, then store >> shift
    set_mac_shifted(1, x, shift);
    set_mac_shifted(2, y, shift);
    set_mac_shifted(3, z, shift);

    // IR1/IR2 = clamp(MAC1/2, lm)
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);

    // IR3 first write: from z >> 12 with lm=false (DuckStation step)
    set_ir(3, (int32_t)(z >> 12), false);

    // SZ3 = clamp(z >> 12, 0, 0xFFFF) - ALWAYS z >> 12 regardless of sf!
    const int32_t z_shifted = (int32_t)(z >> 12);
    push_sz(z_shifted);

    // Perspective projection
    const uint32_t h = ctrl_[C_H] & 0xFFFFu;
    const uint32_t sz3 = data_[D_SZ3];
    const uint32_t quotient = gte_divide(h, sz3, flag_);

    // Quirk: force_geom_offset_zero (see set_force_geom_offset_zero in gte.h).
    // When enabled, RTPS pretends OFX==OFY==0 for projection purposes — used
    // by games that bake double-buffer Y shifts into the GTE GeomOffset.
    const int64_t ofx = force_geom_offset_zero_ ? (int64_t)0 : (int64_t)(int32_t)ctrl_[C_OFX];
    const int64_t ofy = force_geom_offset_zero_ ? (int64_t)0 : (int64_t)(int32_t)ctrl_[C_OFY];
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);

    const int64_t Sx = (int64_t)quotient * (int64_t)ir1 + ofx;
    const int64_t Sy = (int64_t)quotient * (int64_t)ir2 + ofy;

    if (rtps_trace_count_ < 3)
    {
        RtpsTraceVertex& trace = rtps_trace_[rtps_trace_count_++];
        trace.vx = V[0];
        trace.vy = V[1];
        trace.vz = V[2];
        trace.mac1_raw = x;
        trace.mac2_raw = y;
        trace.mac3_raw = z;
        trace.ir1 = ir1;
        trace.ir2 = ir2;
        trace.ir3_z = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);
        trace.sz3 = sz3;
        trace.h = h;
        trace.quotient = quotient;
        trace.sx_accum = Sx;
        trace.sy_accum = Sy;
        trace.sx_preclamp = (int32_t)(Sx >> 16);
        trace.sy_preclamp = (int32_t)(Sy >> 16);
        trace.flag_before_push = flag_;
        trace.last = last;
    }

    // Check MAC0 overflow on Sx and Sy (DuckStation: CheckMACOverflow<0>)
    // FLAG bit 13 (MAC0_OFLOW_POS) is in error bits → sets bit 31.
    // Games checking FLAG after RTPS use this to reject overflowed vertices.
    check_mac_overflow(0, Sx);
    check_mac_overflow(0, Sy);

    push_sxy((int32_t)(Sx >> 16), (int32_t)(Sy >> 16));

    // IR3 second write: from MAC3 with instruction's lm flag (DuckStation step)
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // DQA/DQB depth cueing (only on last vertex)
    if (last)
    {
        const int32_t dqa = (int32_t)(int16_t)(ctrl_[C_DQA] & 0xFFFFu);
        const int32_t dqb = (int32_t)ctrl_[C_DQB];
        const int64_t depth = (int64_t)quotient * (int64_t)dqa + (int64_t)dqb;
        set_mac(0, depth);
        set_ir(0, (int32_t)(depth >> 12), true);
    }
}

// 3D reconstruction: capture GTE state after RTPS/RTPT for correlation with GPU polygons.
// verts: array of 1 or 3 vertex triplets (x,y,z). count: 1 (RTPS) or 3 (RTPT).
void Gte::capture_snapshot(const int32_t (*verts)[3], int count)
{
    auto& snap = last_snapshot_;

    if (count == 3)
    {
        // RTPT: 3 vertices projected at once. Copy directly.
        snap.vertex_count = 3;
        for (int i = 0; i < 3; ++i)
            snap.vertices[i] = {verts[i][0], verts[i][1], verts[i][2]};
    }
    else
    {
        // RTPS: 1 vertex at a time. Accumulate in a 3-element FIFO.
        // After 3 consecutive RTPS calls, the FIFO matches the SXY FIFO order.
        rtps_vert_fifo_[0] = rtps_vert_fifo_[1];
        rtps_vert_fifo_[1] = rtps_vert_fifo_[2];
        rtps_vert_fifo_[2] = {verts[0][0], verts[0][1], verts[0][2]};

        snap.vertex_count = 3;  // Always expose as 3-vertex for correlation
        snap.vertices[0] = rtps_vert_fifo_[0];
        snap.vertices[1] = rtps_vert_fifo_[1];
        snap.vertices[2] = rtps_vert_fifo_[2];
    }

    // Always read all 3 SXY FIFO values. For RTPS, the hardware FIFO
    // (push_sxy) shifts SXY0←SXY1←SXY2←new on each call, so after
    // 3 RTPS calls SXY0/1/2 contain the 3 projected screen coords.
    snap.sx[0] = (int16_t)(data_[D_SXY0] & 0xFFFFu);
    snap.sy[0] = (int16_t)(data_[D_SXY0] >> 16);
    snap.sx[1] = (int16_t)(data_[D_SXY1] & 0xFFFFu);
    snap.sy[1] = (int16_t)(data_[D_SXY1] >> 16);
    snap.sx[2] = (int16_t)(data_[D_SXY2] & 0xFFFFu);
    snap.sy[2] = (int16_t)(data_[D_SXY2] >> 16);
    snap.sz[0] = (uint16_t)(data_[D_SZ1] & 0xFFFFu);
    snap.sz[1] = (uint16_t)(data_[D_SZ2] & 0xFFFFu);
    snap.sz[2] = (uint16_t)(data_[D_SZ3] & 0xFFFFu);

    // Capture RT rotation matrix from packed ctrl registers:
    // ctrl[0]=R11|R12, ctrl[1]=R13|R21, ctrl[2]=R22|R23, ctrl[3]=R31|R32, ctrl[4]=R33
    snap.transform.rt[0] = (int16_t)(ctrl_[C_R11R12] & 0xFFFFu);       // R11
    snap.transform.rt[1] = (int16_t)(ctrl_[C_R11R12] >> 16);            // R12
    snap.transform.rt[2] = (int16_t)(ctrl_[C_R13R21] & 0xFFFFu);       // R13
    snap.transform.rt[3] = (int16_t)(ctrl_[C_R13R21] >> 16);            // R21
    snap.transform.rt[4] = (int16_t)(ctrl_[C_R22R23] & 0xFFFFu);       // R22
    snap.transform.rt[5] = (int16_t)(ctrl_[C_R22R23] >> 16);            // R23
    snap.transform.rt[6] = (int16_t)(ctrl_[C_R31R32] & 0xFFFFu);       // R31
    snap.transform.rt[7] = (int16_t)(ctrl_[C_R31R32] >> 16);            // R32
    snap.transform.rt[8] = (int16_t)(ctrl_[C_R33] & 0xFFFFu);          // R33

    // Translation vector
    snap.transform.tr[0] = (int32_t)ctrl_[C_TRX];
    snap.transform.tr[1] = (int32_t)ctrl_[C_TRY];
    snap.transform.tr[2] = (int32_t)ctrl_[C_TRZ];

    snap.sequence_id = ++snapshot_seq_;
    snap.valid = 1;
}

void Gte::cmd_rtps(uint32_t cmd)
{
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    rtps_trace_count_ = 0;
    const int32_t V[3] = { vx(0), vy(0), vz(0) };
    rtps_internal(V, sf, lm, true);

    // 3D reconstruction: capture snapshot after RTPS (single vertex)
    capture_snapshot(&V, 1);
}

void Gte::cmd_rtpt(uint32_t cmd)
{
    // RTPT: RTPS on V0, V1, V2. DQA/DQB only on last vertex.
    // Uses push_sxy shift register naturally (3 pushes → SXY0=V0, SXY1=V1, SXY2=V2).
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    rtps_trace_count_ = 0;

    const int32_t V0[3] = { vx(0), vy(0), vz(0) };
    const int32_t V1[3] = { vx(1), vy(1), vz(1) };
    const int32_t V2[3] = { vx(2), vy(2), vz(2) };

    rtps_internal(V0, sf, lm, false);
    rtps_internal(V1, sf, lm, false);
    rtps_internal(V2, sf, lm, true);

    // 3D reconstruction: capture snapshot after RTPT (3 vertices)
    const int32_t verts[3][3] = {
        {V0[0], V0[1], V0[2]},
        {V1[0], V1[1], V1[2]},
        {V2[0], V2[1], V2[2]}
    };
    capture_snapshot(verts, 3);

    // RTPT vertex pattern diagnostic
    ++rtpt_diag_.rtpt_count;
    const bool eq01 = (V0[0]==V1[0] && V0[1]==V1[1] && V0[2]==V1[2]);
    const bool eq12 = (V1[0]==V2[0] && V1[1]==V2[1] && V1[2]==V2[2]);
    const bool eq02 = (V0[0]==V2[0] && V0[1]==V2[1] && V0[2]==V2[2]);
    if (eq01 && eq12) ++rtpt_diag_.all_same;
    else if (eq12) ++rtpt_diag_.v1_eq_v2;
    else if (eq02) ++rtpt_diag_.v0_eq_v2;
    else if (!eq01) ++rtpt_diag_.all_unique;
}

void Gte::cmd_avsz3(uint32_t)
{
    // AVSZ3: OTZ = ZSF3 * (SZ1+SZ2+SZ3) >> 12
    const uint32_t sz1 = data_[D_SZ1] & 0xFFFFu;
    const uint32_t sz2 = data_[D_SZ2] & 0xFFFFu;
    const uint32_t sz3 = data_[D_SZ3] & 0xFFFFu;
    const int32_t zsf3 = s16(ctrl_[C_ZSF3]);

    const int64_t sum = (int64_t)sz1 + (int64_t)sz2 + (int64_t)sz3;
    const int64_t mac0 = (int64_t)zsf3 * sum;
    set_mac(0, mac0);
    const uint32_t otz = clamp_u16((int32_t)(mac0 >> 12));
    data_[D_OTZ] = otz;

    // INSTRUMENTATION (temporary): log AVSZ3 results at ERROR level
    static uint32_t avsz3_log_count = 0;
    static uint32_t avsz3_zero_count = 0;
    if (otz == 0)
    {
        ++avsz3_zero_count;
        if (avsz3_zero_count <= 20)
            emu::logf(emu::LogLevel::error, "GTE",
                "AVSZ3 OTZ=0 #%u! zsf3=%d sz=(%u,%u,%u) sum=%lld mac0=%lld mac0>>12=%d",
                avsz3_zero_count, zsf3, sz1, sz2, sz3,
                (long long)sum, (long long)mac0, (int)(int32_t)(mac0 >> 12));
    }
    if (avsz3_log_count < 5)
    {
        ++avsz3_log_count;
        emu::logf(emu::LogLevel::error, "GTE",
            "AVSZ3 #%u: zsf3=%d sz=(%u,%u,%u) sum=%lld mac0=%lld otz=%u",
            avsz3_log_count, zsf3, sz1, sz2, sz3,
            (long long)sum, (long long)mac0, otz);
    }
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
    set_mac_shifted(1, mac1, shift);
    set_mac_shifted(2, mac2, shift);
    set_mac_shifted(3, mac3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
}

void Gte::cmd_gpf(uint32_t cmd)
{
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
    set_mac_shifted(1, mac1, shift);
    set_mac_shifted(2, mac2, shift);
    set_mac_shifted(3, mac3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
}

void Gte::cmd_gpl(uint32_t cmd)
{
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int shift = sf ? 12 : 0;

    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ir3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    // GPL uses MAC << shift as base (DuckStation: s64(s32(m_regs.MAC1)) << sf)
    const int64_t mac1 = ((int64_t)(int32_t)data_[D_MAC1] << shift) + (int64_t)ir1 * ir0;
    const int64_t mac2 = ((int64_t)(int32_t)data_[D_MAC2] << shift) + (int64_t)ir2 * ir0;
    const int64_t mac3 = ((int64_t)(int32_t)data_[D_MAC3] << shift) + (int64_t)ir3 * ir0;

    set_mac_shifted(1, mac1, shift);
    set_mac_shifted(2, mac2, shift);
    set_mac_shifted(3, mac3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
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

    set_mac_shifted(1, mac1, shift);
    set_mac_shifted(2, mac2, shift);
    set_mac_shifted(3, mac3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
}

// InterpolateColor: DuckStation's InterpolateColor pattern.
// in_MAC = input color values (pre-computed, unshifted).
// Step 1: IR = clamp((FC << 12) - in_MAC) >> shift  (lm=false → full range)
// Step 2: MAC = (IR * IR0 + in_MAC) >> shift        (lm from instruction)
void Gte::interpolate_color(int64_t in1, int64_t in2, int64_t in3, int shift, int lm)
{
    const int64_t fc1 = (int32_t)ctrl_[C_RFC];
    const int64_t fc2 = (int32_t)ctrl_[C_GFC];
    const int64_t fc3 = (int32_t)ctrl_[C_BFC];

    // Step 1: (FC << 12) - in_MAC, set MAC and IR (lm=false for this step)
    set_mac_shifted(1, (fc1 << 12) - in1, shift);
    set_mac_shifted(2, (fc2 << 12) - in2, shift);
    set_mac_shifted(3, (fc3 << 12) - in3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], false);
    set_ir(2, (int32_t)data_[D_MAC2], false);
    set_ir(3, (int32_t)data_[D_MAC3], false);

    // Step 2: IR_clamped * IR0 + in_MAC (DuckStation uses clamped IR from step 1)
    const int32_t ir0 = (int32_t)(int16_t)(data_[D_IR0] & 0xFFFFu);
    set_mac_shifted(1, (int64_t)(int32_t)data_[D_IR1] * ir0 + in1, shift);
    set_mac_shifted(2, (int64_t)(int32_t)data_[D_IR2] * ir0 + in2, shift);
    set_mac_shifted(3, (int64_t)(int32_t)data_[D_IR3] * ir0 + in3, shift);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);
}

void Gte::dpcs_internal(const uint8_t color[3], int shift, int lm)
{
    // [MAC1,MAC2,MAC3] = [R,G,B] SHL 16
    set_mac(1, (int64_t)color[0] << 16);
    set_mac(2, (int64_t)color[1] << 16);
    set_mac(3, (int64_t)color[2] << 16);

    // InterpolateColor
    interpolate_color(data_[D_MAC1], data_[D_MAC2], data_[D_MAC3], shift, lm);

    // Push color
    uint8_t code = (uint8_t)((data_[D_RGBC] >> 24) & 0xFFu);
    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
}

void Gte::cmd_dpcs(uint32_t cmd)
{
    const int shift = ((cmd >> 19) & 1) ? 12 : 0;
    const int lm = (cmd >> 10) & 1;
    const uint8_t color[3] = {
        (uint8_t)(data_[D_RGBC] & 0xFFu),
        (uint8_t)((data_[D_RGBC] >> 8) & 0xFFu),
        (uint8_t)((data_[D_RGBC] >> 16) & 0xFFu),
    };
    dpcs_internal(color, shift, lm);
}

void Gte::cmd_intpl(uint32_t cmd)
{
    const int shift = ((cmd >> 19) & 1) ? 12 : 0;
    const int lm = (cmd >> 10) & 1;

    // [MAC1,MAC2,MAC3] = [IR1,IR2,IR3] SHL 12, then InterpolateColor
    interpolate_color(
        (int64_t)(int32_t)data_[D_IR1] << 12,
        (int64_t)(int32_t)data_[D_IR2] << 12,
        (int64_t)(int32_t)data_[D_IR3] << 12,
        shift, lm);

    uint8_t code = (uint8_t)((data_[D_RGBC] >> 24) & 0xFFu);
    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
}

void Gte::cmd_ncs(uint32_t cmd)
{
    const int lm = (cmd >> 10) & 1;
    const int32_t nx = vx(0), ny = vy(0), nz = vz(0);

    // Step 1: L * Normal -> MAC/IR (light intensities)
    const int32_t l11 = s16(ctrl_[C_L11L12]);
    const int32_t l12 = hi16(ctrl_[C_L11L12]);
    const int32_t l13 = s16(ctrl_[C_L13L21]);
    const int32_t l21 = hi16(ctrl_[C_L13L21]);
    const int32_t l22 = s16(ctrl_[C_L22L23]);
    const int32_t l23 = hi16(ctrl_[C_L22L23]);
    const int32_t l31 = s16(ctrl_[C_L31L32]);
    const int32_t l32 = hi16(ctrl_[C_L31L32]);
    const int32_t l33 = s16(ctrl_[C_L33]);

    int64_t mac1 = sign_extend_mac(1, (int64_t)l11 * nx + (int64_t)l12 * ny) + (int64_t)l13 * nz;
    int64_t mac2 = sign_extend_mac(2, (int64_t)l21 * nx + (int64_t)l22 * ny) + (int64_t)l23 * nz;
    int64_t mac3 = sign_extend_mac(3, (int64_t)l31 * nx + (int64_t)l32 * ny) + (int64_t)l33 * nz;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 2: BK + LC * IR -> MAC/IR (lit color)
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

    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    mac1 = sign_extend_mac(1, sign_extend_mac(1, (rbk << 12) + (int64_t)lr1 * i1) + (int64_t)lr2 * i2) + (int64_t)lr3 * i3;
    mac2 = sign_extend_mac(2, sign_extend_mac(2, (gbk << 12) + (int64_t)lg1 * i1) + (int64_t)lg2 * i2) + (int64_t)lg3 * i3;
    mac3 = sign_extend_mac(3, sign_extend_mac(3, (bbk << 12) + (int64_t)lb1 * i1) + (int64_t)lb2 * i2) + (int64_t)lb3 * i3;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 3: push color
    uint8_t code = (uint8_t)((data_[D_RGBC] >> 24) & 0xFFu);
    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
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
    // NCCS = NCS lighting + RGBC color modulation (no depth cueing)
    const int lm = (cmd >> 10) & 1;
    const int32_t nx = vx(0), ny = vy(0), nz = vz(0);

    // Step 1: L * Normal -> MAC/IR
    const int32_t l11 = s16(ctrl_[C_L11L12]);
    const int32_t l12 = hi16(ctrl_[C_L11L12]);
    const int32_t l13 = s16(ctrl_[C_L13L21]);
    const int32_t l21 = hi16(ctrl_[C_L13L21]);
    const int32_t l22 = s16(ctrl_[C_L22L23]);
    const int32_t l23 = hi16(ctrl_[C_L22L23]);
    const int32_t l31 = s16(ctrl_[C_L31L32]);
    const int32_t l32 = hi16(ctrl_[C_L31L32]);
    const int32_t l33 = s16(ctrl_[C_L33]);

    int64_t mac1 = sign_extend_mac(1, (int64_t)l11 * nx + (int64_t)l12 * ny) + (int64_t)l13 * nz;
    int64_t mac2 = sign_extend_mac(2, (int64_t)l21 * nx + (int64_t)l22 * ny) + (int64_t)l23 * nz;
    int64_t mac3 = sign_extend_mac(3, (int64_t)l31 * nx + (int64_t)l32 * ny) + (int64_t)l33 * nz;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 2: BK + LC * IR -> MAC/IR
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

    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    mac1 = sign_extend_mac(1, sign_extend_mac(1, (rbk << 12) + (int64_t)lr1 * i1) + (int64_t)lr2 * i2) + (int64_t)lr3 * i3;
    mac2 = sign_extend_mac(2, sign_extend_mac(2, (gbk << 12) + (int64_t)lg1 * i1) + (int64_t)lg2 * i2) + (int64_t)lg3 * i3;
    mac3 = sign_extend_mac(3, sign_extend_mac(3, (bbk << 12) + (int64_t)lb1 * i1) + (int64_t)lb2 * i2) + (int64_t)lb3 * i3;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 3: RGBC * IR -> color modulation
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t ci1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ci2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ci3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    mac1 = (int64_t)(r << 4) * ci1;
    mac2 = (int64_t)(g << 4) * ci2;
    mac3 = (int64_t)(b << 4) * ci3;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
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

void Gte::cmd_cc(uint32_t cmd)
{
    const int lm = (cmd >> 10) & 1;

    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    // BK + LC * (IR * RGBC)
    // DuckStation: BK + LC * clamp(RGBC * IR >> 8, lm=false)
    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    // RGBC * IR >> 8 -> gives 1.15.0 range light input
    // Actually DuckStation does: ((R << 4) * IR1) then feeds into BK+LC*V
    const int64_t rv = ((int64_t)(r << 4) * i1);
    const int64_t gv = ((int64_t)(g << 4) * i2);
    const int64_t bv = ((int64_t)(b << 4) * i3);
    set_mac_shifted(1, rv, 12);
    set_mac_shifted(2, gv, 12);
    set_mac_shifted(3, bv, 12);
    set_ir(1, (int32_t)data_[D_MAC1], false);
    set_ir(2, (int32_t)data_[D_MAC2], false);
    set_ir(3, (int32_t)data_[D_MAC3], false);

    // BK + LC * IR
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

    const int32_t vi1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t vi2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t vi3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int64_t mac1 = sign_extend_mac(1, sign_extend_mac(1, (rbk << 12) + (int64_t)lr1 * vi1) + (int64_t)lr2 * vi2) + (int64_t)lr3 * vi3;
    const int64_t mac2 = sign_extend_mac(2, sign_extend_mac(2, (gbk << 12) + (int64_t)lg1 * vi1) + (int64_t)lg2 * vi2) + (int64_t)lg3 * vi3;
    const int64_t mac3 = sign_extend_mac(3, sign_extend_mac(3, (bbk << 12) + (int64_t)lb1 * vi1) + (int64_t)lb2 * vi2) + (int64_t)lb3 * vi3;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
}

void Gte::cmd_ncds(uint32_t cmd)
{
    // NCDS = NCS (lighting) + CC (color modulation) + DPCS (depth cue)
    const int lm = (cmd >> 10) & 1;
    const int32_t nx = vx(0), ny = vy(0), nz = vz(0);

    // Step 1: L * Normal -> MAC/IR (light intensities)
    const int32_t l11 = s16(ctrl_[C_L11L12]);
    const int32_t l12 = hi16(ctrl_[C_L11L12]);
    const int32_t l13 = s16(ctrl_[C_L13L21]);
    const int32_t l21 = hi16(ctrl_[C_L13L21]);
    const int32_t l22 = s16(ctrl_[C_L22L23]);
    const int32_t l23 = hi16(ctrl_[C_L22L23]);
    const int32_t l31 = s16(ctrl_[C_L31L32]);
    const int32_t l32 = hi16(ctrl_[C_L31L32]);
    const int32_t l33 = s16(ctrl_[C_L33]);

    int64_t mac1 = sign_extend_mac(1, (int64_t)l11 * nx + (int64_t)l12 * ny) + (int64_t)l13 * nz;
    int64_t mac2 = sign_extend_mac(2, (int64_t)l21 * nx + (int64_t)l22 * ny) + (int64_t)l23 * nz;
    int64_t mac3 = sign_extend_mac(3, (int64_t)l31 * nx + (int64_t)l32 * ny) + (int64_t)l33 * nz;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 2: BK + LC * IR -> MAC/IR (lit color)
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

    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    mac1 = sign_extend_mac(1, sign_extend_mac(1, (rbk << 12) + (int64_t)lr1 * i1) + (int64_t)lr2 * i2) + (int64_t)lr3 * i3;
    mac2 = sign_extend_mac(2, sign_extend_mac(2, (gbk << 12) + (int64_t)lg1 * i1) + (int64_t)lg2 * i2) + (int64_t)lg3 * i3;
    mac3 = sign_extend_mac(3, sign_extend_mac(3, (bbk << 12) + (int64_t)lb1 * i1) + (int64_t)lb2 * i2) + (int64_t)lb3 * i3;
    set_mac_shifted(1, mac1, 12);
    set_mac_shifted(2, mac2, 12);
    set_mac_shifted(3, mac3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 3: [MAC1,MAC2,MAC3] = [R*IR1,G*IR2,B*IR3] SHL 4 (DuckStation: no MAC set, just compute)
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t ci1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ci2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t ci3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    const int32_t in_mac1 = (r * ci1) << 4;
    const int32_t in_mac2 = (g * ci2) << 4;
    const int32_t in_mac3 = (b * ci3) << 4;

    // Step 4: InterpolateColor (FC-MAC)*IR0 + MAC
    interpolate_color(in_mac1, in_mac2, in_mac3, 12, lm);

    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
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
    // CDP = BK + LCM*IR → RGBC*IR<<4 → InterpolateColor
    const int lm = (cmd >> 10) & 1;

    // Step 1: BK + LCM * IR
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

    const int32_t i1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t i2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);
    const int32_t i3 = (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu);

    set_mac_shifted(1, sign_extend_mac(1, sign_extend_mac(1, (rbk << 12) + (int64_t)lr1*i1) + (int64_t)lr2*i2) + (int64_t)lr3*i3, 12);
    set_mac_shifted(2, sign_extend_mac(2, sign_extend_mac(2, (gbk << 12) + (int64_t)lg1*i1) + (int64_t)lg2*i2) + (int64_t)lg3*i3, 12);
    set_mac_shifted(3, sign_extend_mac(3, sign_extend_mac(3, (bbk << 12) + (int64_t)lb1*i1) + (int64_t)lb2*i2) + (int64_t)lb3*i3, 12);
    set_ir(1, (int32_t)data_[D_MAC1], lm);
    set_ir(2, (int32_t)data_[D_MAC2], lm);
    set_ir(3, (int32_t)data_[D_MAC3], lm);

    // Step 2: [R*IR1,G*IR2,B*IR3] SHL 4
    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);
    const int32_t in1 = (r * (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu)) << 4;
    const int32_t in2 = (g * (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu)) << 4;
    const int32_t in3 = (b * (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu)) << 4;

    // Step 3: InterpolateColor
    interpolate_color(in1, in2, in3, 12, lm);
    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
}

void Gte::cmd_dcpl(uint32_t cmd)
{
    // DCPL = [R*IR1,G*IR2,B*IR3] SHL 4 → InterpolateColor
    const int shift = ((cmd >> 19) & 1) ? 12 : 0;
    const int lm = (cmd >> 10) & 1;

    int32_t r, g, b;
    uint8_t code = 0;
    unpack_rgbc(data_[D_RGBC], r, g, b, code);

    const int32_t in1 = (r * (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu)) << 4;
    const int32_t in2 = (g * (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu)) << 4;
    const int32_t in3 = (b * (int32_t)(int16_t)(data_[D_IR3] & 0xFFFFu)) << 4;

    interpolate_color(in1, in2, in3, shift, lm);
    push_color((int32_t)data_[D_MAC1] >> 4, (int32_t)data_[D_MAC2] >> 4, (int32_t)data_[D_MAC3] >> 4, code);
}

void Gte::cmd_dpct(uint32_t cmd)
{
    // DPCT = DPCS × 3, each time reading CURRENT RGB0 (FIFO shifts between iterations)
    const int shift = ((cmd >> 19) & 1) ? 12 : 0;
    const int lm = (cmd >> 10) & 1;
    for (int i = 0; i < 3; ++i)
    {
        // Always read current RGB0 (data_[D_RGB0]) — after each push_color, FIFO shifts
        const uint32_t rgb0 = data_[D_RGB0];
        const uint8_t color[3] = {
            (uint8_t)(rgb0 & 0xFFu),
            (uint8_t)((rgb0 >> 8) & 0xFFu),
            (uint8_t)((rgb0 >> 16) & 0xFFu),
        };
        dpcs_internal(color, shift, lm);
    }
}

int Gte::execute(uint32_t cop2_instruction)
{
    const uint32_t cmd = cop2_instruction & 0x01FF'FFFFu;
    const uint32_t funct = cmd & 0x3Fu;

    // Reset FLAG at start of every GTE command (DuckStation behavior)
    flag_ = 0;

    // INSTRUMENTATION: fire once to PROVE execute() is reached
    static uint32_t exec_count = 0;
    ++exec_count;
    if (exec_count == 1)
        emu::logf(emu::LogLevel::error, "GTE", "execute() FIRST CALL funct=0x%02X", funct);
    if (exec_count == 100)
        emu::logf(emu::LogLevel::error, "GTE", "execute() 100th call funct=0x%02X total=%u", funct, exec_count);
    if (funct == 0x2D)
    {
        static uint32_t avsz3_exec = 0;
        ++avsz3_exec;
        // Read AVSZ3 inputs BEFORE the command runs (SZ values are already set by prior RTPT)
        const uint32_t pre_sz1 = data_[D_SZ1] & 0xFFFFu;
        const uint32_t pre_sz2 = data_[D_SZ2] & 0xFFFFu;
        const uint32_t pre_sz3 = data_[D_SZ3] & 0xFFFFu;
        const int32_t pre_zsf3 = s16(ctrl_[C_ZSF3]);
        if (avsz3_exec <= 10)
            emu::logf(emu::LogLevel::error, "GTE",
                "AVSZ3_PRE #%u zsf3=%d sz=(%u,%u,%u)", avsz3_exec, pre_zsf3, pre_sz1, pre_sz2, pre_sz3);
    }

    // Debug: count GTE command usage (log summary after 1000 calls)
    static uint32_t gte_cmd_counts[64] = {};
    static int light_log_count = 0;
    static bool gte_summary_logged = false;

    gte_cmd_counts[funct]++;
    uint32_t total = 0;
    for (int i = 0; i < 64; i++) total += gte_cmd_counts[i];
    if (total == 1000 && !gte_summary_logged)
    {
        gte_summary_logged = true;
        emu::logf(emu::LogLevel::error, "GTE", "=== GTE command usage after 1000 calls ===");
        for (int i = 0; i < 64; i++)
        {
            if (gte_cmd_counts[i] > 0)
                emu::logf(emu::LogLevel::error, "GTE", "  cmd 0x%02X: %u calls", i, gte_cmd_counts[i]);
        }
    }

    // GTE command cycle counts (PSX-SPX / DuckStation reference).
    // These represent the actual hardware execution time for each command.
    int cycles = 0;
    switch (funct)
    {
        case 0x06: cmd_nclip(cmd); cycles = 8; break;
        case 0x0C: cmd_op(cmd); cycles = 6; break;
        case 0x10: cmd_dpcs(cmd); cycles = 8; break;
        case 0x11: cmd_intpl(cmd); cycles = 8; break;
        case 0x12:
        {
            // Log MVMVA when used with Light or Color matrices (lighting path)
            const int mx = (cmd >> 17) & 3;
            if (light_log_count < 10 && (mx == 1 || mx == 2))
            {
                ++light_log_count;
                const int vv = (cmd >> 15) & 3;
                const int tv = (cmd >> 13) & 3;
                emu::logf(emu::LogLevel::warn, "GTE", "MVMVA #%d mx=%d(%s) vv=%d tv=%d L=(%d,%d,%d/%d,%d,%d/%d,%d,%d) BK=(%d,%d,%d) V=(%d,%d,%d)",
                    light_log_count, mx, mx==1?"LLM":"LCM", vv, tv,
                    s16(ctrl_[C_L11L12]), hi16(ctrl_[C_L11L12]), s16(ctrl_[C_L13L21]),
                    hi16(ctrl_[C_L13L21]), s16(ctrl_[C_L22L23]), hi16(ctrl_[C_L22L23]),
                    s16(ctrl_[C_L31L32]), hi16(ctrl_[C_L31L32]), s16(ctrl_[C_L33]),
                    (int32_t)ctrl_[C_RBK], (int32_t)ctrl_[C_GBK], (int32_t)ctrl_[C_BBK],
                    vx(0), vy(0), vz(0));
            }
            cmd_mvmva(cmd); cycles = 8; break;
        }
        case 0x13: // NCDS (19)
        case 0x14: // CDP  (13)
        case 0x16: // NCDT (44)
        case 0x1B: // NCCS (17)
        case 0x1C: // CC   (11)
        case 0x1E: // NCS  (14)
        case 0x20: // NCT  (30)
        case 0x3F: // NCCT (39)
        {
            if (light_log_count < 10)
            {
                ++light_log_count;
                emu::logf(emu::LogLevel::warn, "GTE", "LIGHT cmd=0x%02X #%d: L=(%d,%d,%d/%d,%d,%d/%d,%d,%d) BK=(%d,%d,%d) RGBC=0x%08X V0=(%d,%d,%d)",
                    funct, light_log_count,
                    s16(ctrl_[C_L11L12]), hi16(ctrl_[C_L11L12]), s16(ctrl_[C_L13L21]),
                    hi16(ctrl_[C_L13L21]), s16(ctrl_[C_L22L23]), hi16(ctrl_[C_L22L23]),
                    s16(ctrl_[C_L31L32]), hi16(ctrl_[C_L31L32]), s16(ctrl_[C_L33]),
                    (int32_t)ctrl_[C_RBK], (int32_t)ctrl_[C_GBK], (int32_t)ctrl_[C_BBK],
                    data_[D_RGBC], vx(0), vy(0), vz(0));
            }
            switch (funct)
            {
                case 0x13: cmd_ncds(cmd); cycles = 19; break;
                case 0x14: cmd_cdp(cmd);  cycles = 13; break;
                case 0x16: cmd_ncdt(cmd); cycles = 44; break;
                case 0x1B: cmd_nccs(cmd); cycles = 17; break;
                case 0x1C: cmd_cc(cmd);   cycles = 11; break;
                case 0x1E: cmd_ncs(cmd);  cycles = 14; break;
                case 0x20: cmd_nct(cmd);  cycles = 30; break;
                case 0x3F: cmd_ncct(cmd); cycles = 39; break;
            }
            break;
        }
        case 0x01: cmd_rtps(cmd);  cycles = 15; break;
        case 0x30: cmd_rtpt(cmd);  cycles = 23; break;
        case 0x2D: cmd_avsz3(cmd); cycles = 5;  break;
        case 0x2E: cmd_avsz4(cmd); cycles = 6;  break;
        case 0x28: cmd_sqr(cmd);   cycles = 5;  break;
        case 0x29: cmd_dcpl(cmd);  cycles = 8;  break;
        case 0x2A: cmd_dpct(cmd);  cycles = 17; break;
        case 0x3D: cmd_gpf(cmd);   cycles = 5;  break;
        case 0x3E: cmd_gpl(cmd);   cycles = 5;  break;
        default: return 0;
    }

    // Finalize FLAG register: bit 31 = OR of error bits (30..23, 18..13)
    if (flag_ & FLAG_ERROR_BITS)
        flag_ |= (1u << 31);
    ctrl_[C_FLAG] = flag_;

    // Targeted debug for TREX-style exploding polys:
    // capture projected vertices that hit screen saturation without tripping
    // the "fatal" FLAG sign bit that PsyQ checks before emitting a packet.
    if (funct == 0x01 || funct == 0x30)
    {
        const auto unpack_sxy = [](uint32_t packed, int32_t& sx, int32_t& sy) {
            sx = (int16_t)(packed & 0xFFFFu);
            sy = (int16_t)((packed >> 16) & 0xFFFFu);
        };
        int32_t sx0, sy0, sx1, sy1, sx2, sy2;
        unpack_sxy(data_[D_SXY0], sx0, sy0);
        unpack_sxy(data_[D_SXY1], sx1, sy1);
        unpack_sxy(data_[D_SXY2], sx2, sy2);
        const bool extreme =
            (std::abs(sx0) >= 900 || std::abs(sy0) >= 900 ||
             std::abs(sx1) >= 900 || std::abs(sy1) >= 900 ||
             std::abs(sx2) >= 900 || std::abs(sy2) >= 900);
        const bool sxy_sat = (flag_ & (FLAG_SX2_SAT | FLAG_SY2_SAT)) != 0;
        const bool fatal = (flag_ & (1u << 31)) != 0;
        if (extreme && (!fatal || sxy_sat))
        {
            static uint32_t extreme_proj_logs = 0;
            if (extreme_proj_logs < 200)
            {
                ++extreme_proj_logs;
                emu::logf(emu::LogLevel::warn, "GTE",
                    "EXTREME_%s #%u flag=0x%08X fatal=%u sxy_sat=%u sxy0=(%d,%d) sxy1=(%d,%d) sxy2=(%d,%d) "
                    "sz=(%u,%u,%u) of=(%d,%d) h=%u",
                    (funct == 0x30) ? "RTPT" : "RTPS",
                    extreme_proj_logs,
                    flag_, fatal ? 1u : 0u, sxy_sat ? 1u : 0u,
                    sx0, sy0, sx1, sy1, sx2, sy2,
                    data_[D_SZ1], data_[D_SZ2], data_[D_SZ3],
                    (int32_t)ctrl_[C_OFX], (int32_t)ctrl_[C_OFY],
                    ctrl_[C_H] & 0xFFFFu);

                for (int i = 0; i < rtps_trace_count_ && i < 3; ++i)
                {
                    const RtpsTraceVertex& trace = rtps_trace_[i];
                    emu::logf(emu::LogLevel::warn, "GTE",
                        "EXTREME_%s_V%d #%u in=(%d,%d,%d) mac=(%lld,%lld,%lld) ir=(%d,%d,%d) "
                        "sz3=%u h=%u q=0x%05X sxy_accum=(%lld,%lld) sxy_preclamp=(%d,%d) "
                        "flag_before_push=0x%08X last=%u",
                        (funct == 0x30) ? "RTPT" : "RTPS",
                        i,
                        extreme_proj_logs,
                        trace.vx, trace.vy, trace.vz,
                        (long long)trace.mac1_raw,
                        (long long)trace.mac2_raw,
                        (long long)trace.mac3_raw,
                        trace.ir1, trace.ir2, trace.ir3_z,
                        trace.sz3, trace.h, trace.quotient,
                        (long long)trace.sx_accum,
                        (long long)trace.sy_accum,
                        trace.sx_preclamp, trace.sy_preclamp,
                        trace.flag_before_push,
                        trace.last ? 1u : 0u);
                }
            }
        }
    }

    // POST-command log for AVSZ3: capture OTZ result + FLAG
    if (funct == 0x2D)
    {
        static uint32_t avsz3_post = 0;
        static uint32_t avsz3_otz0 = 0;
        ++avsz3_post;
        const uint32_t otz = data_[D_OTZ];
        if (otz == 0)
        {
            ++avsz3_otz0;
            if (avsz3_otz0 <= 20)
                emu::logf(emu::LogLevel::error, "GTE",
                    "AVSZ3_POST OTZ=0! #%u/%u flag=0x%08X mac0=%d",
                    avsz3_otz0, avsz3_post, flag_, (int32_t)data_[D_MAC0]);
        }
        else if (avsz3_post <= 10)
        {
            emu::logf(emu::LogLevel::error, "GTE",
                "AVSZ3_POST #%u otz=%u flag=0x%08X mac0=%d",
                avsz3_post, otz, flag_, (int32_t)data_[D_MAC0]);
        }
    }

    return cycles;
}

} // namespace gte
