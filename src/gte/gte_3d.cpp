#include "gte_3d.h"
#include "gte_internal.h"
#include <algorithm>
#include "../log/emu_log.h"

// Use shared helpers from gte_internal.h
using gte::internal::unr_table;
using gte::internal::count_leading_zeros;
// NOTE: We do NOT use gte_divide — see gte_divide_raw below
using gte::internal::u8_clamp;
using gte::internal::unpack_rgbc;
using gte::internal::pack_rgbc;
using gte::internal::clamp_sxy;
using gte::internal::shift_rgb_pipeline;

namespace gte
{

// Unclamped perspective divide for 3D reconstruction.
// Same Newton-Raphson math as hardware but:
//   - NO overflow early-return (sz3*2 <= h doesn't saturate to 0x1FFFF)
//   - NO final cap at 0x1FFFF
// This preserves raw projection for off-screen / extreme-depth vertices.
static inline uint32_t gte_divide_raw(uint32_t h, uint32_t sz3)
{
    if (sz3 == 0) return 0x1FFFF;  // Avoid div-by-zero, vertex at camera

    const uint32_t shift = (uint32_t)count_leading_zeros((uint16_t)sz3) - 16;
    uint32_t lhs = h << shift;
    uint32_t rhs = sz3 << shift;

    const uint32_t divisor = rhs | 0x8000;
    const int32_t x = (int32_t)(0x101 + (uint32_t)unr_table[((divisor & 0x7FFF) + 0x40) >> 7]);
    const int32_t d = ((int32_t)(divisor) * -x + 0x80) >> 8;
    const uint32_t recip = (uint32_t)((x * (0x20000 + d) + 0x80) >> 8);

    return (uint32_t)(((uint64_t)lhs * (uint64_t)recip + 0x8000ULL) >> 16);
}

Gte3D::Gte3D()
{
    emu::logf(emu::LogLevel::warn, "GTE", "GTE3D v5 (tags + SXY table)");
    write_cache_.reserve(4096);
    read_cache_.reserve(4096);
    write_face_cache_.reserve(2048);
    read_face_cache_.reserve(2048);
    write_sxy_table_.reserve(4096);
    read_sxy_table_.reserve(4096);
    reset();
}

void Gte3D::reset()
{
    for (int i = 0; i < 32; ++i)
    {
        data_[i] = 0;
        ctrl_[i] = 0;
    }
    vertex_index_ = 1; // Start at 1: index 0 encodes as 0x0000 (ambiguous with screen coord 0)
    face_index_ = 1;   // Same for faces
    last_normal_[0] = last_normal_[1] = last_normal_[2] = 0;
    write_cache_.clear();
    read_cache_.clear();
    write_face_cache_.clear();
    read_face_cache_.clear();
    write_sxy_table_.clear();
    read_sxy_table_.clear();
}

uint32_t Gte3D::read_data(uint32_t idx) const
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

void Gte3D::write_data(uint32_t idx, uint32_t v)
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

uint32_t Gte3D::read_ctrl(uint32_t idx) const
{
    return ctrl_[idx & 31u];
}

void Gte3D::write_ctrl(uint32_t idx, uint32_t v)
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

void Gte3D::lwc2(uint32_t gte_reg, uint32_t word)
{
    write_data(gte_reg, word);
}

uint32_t Gte3D::swc2(uint32_t gte_reg) const
{
    return read_data(gte_reg);
}


int32_t Gte3D::s16(uint32_t v)
{
    return (int32_t)(int16_t)(v & 0xFFFFu);
}

int32_t Gte3D::hi16(uint32_t v)
{
    return (int32_t)(int16_t)((v >> 16) & 0xFFFFu);
}

uint32_t Gte3D::pack16(int32_t lo, int32_t hi)
{
    return (uint32_t)((uint16_t)lo) | ((uint32_t)((uint16_t)hi) << 16);
}

int32_t Gte3D::clamp_s16(int32_t v)
{
    if (v < -32768)
        return -32768;
    if (v > 32767)
        return 32767;
    return v;
}

uint32_t Gte3D::clamp_u16(int32_t v)
{
    if (v < 0)
        return 0;
    if (v > 0xFFFF)
        return 0xFFFF;
    return (uint32_t)v;
}

int32_t Gte3D::clamp_s32(int64_t v)
{
    if (v < (int64_t)INT32_MIN)
        return INT32_MIN;
    if (v > (int64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)v;
}

int32_t Gte3D::vx(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VXY0 : (n == 1) ? D_VXY1 : D_VXY2;
    return s16(data_[idx]);
}

int32_t Gte3D::vy(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VXY0 : (n == 1) ? D_VXY1 : D_VXY2;
    return hi16(data_[idx]);
}

int32_t Gte3D::vz(uint32_t n) const
{
    const uint32_t idx = (n == 0) ? D_VZ0 : (n == 1) ? D_VZ1 : D_VZ2;
    return s16(data_[idx]);
}

void Gte3D::push_sxy(int32_t sx, int32_t sy)
{
    // 3D shadow: NO clamping to [-1024,1023]. Keep raw projected coords.
    // UE5 handles its own frustum culling — we want ALL geometry.
    // pack16 truncates to int16 range [-32768,32767] which is sufficient.
    const uint32_t val = pack16(sx, sy);
    data_[D_SXY0] = data_[D_SXY1];
    data_[D_SXY1] = data_[D_SXY2];
    data_[D_SXY2] = val;
    data_[D_SXYP] = val;
}

void Gte3D::push_sz(int32_t sz)
{
    // 3D shadow: NO clamping to [0, 0xFFFF]. Keep raw signed Z depth.
    data_[D_SZ0] = data_[D_SZ1];
    data_[D_SZ1] = data_[D_SZ2];
    data_[D_SZ2] = data_[D_SZ3];
    data_[D_SZ3] = (uint32_t)sz;
}

void Gte3D::push_color(int32_t r, int32_t g, int32_t b, uint8_t code)
{
    // 3D shadow: NO clamping to [0,255]. Truncate to uint8 directly.
    // UE5 handles its own color pipeline — preserve raw lighting output.
    const uint8_t cr = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
    const uint8_t cg = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
    const uint8_t cb = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));

    shift_rgb_pipeline(data_);
    data_[D_RGB2] = pack_rgbc(cr, cg, cb, code);
}

// check_mac_overflow: check 44-bit overflow on the FULL (pre-shift) value
void Gte3D::check_mac_overflow(int idx, int64_t raw)
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

// 3D shadow: NO 44-bit truncation. Keep full 64-bit precision for dot products.
// Primary Gte truncates to 44 bits (hardware-accurate), but for 3D reconstruction
// we want maximum precision to avoid accumulation errors.
int64_t Gte3D::sign_extend_mac(int /*idx*/, int64_t v)
{
    return v;  // Pass through — full 64-bit precision
}

// set_mac: DuckStation behavior - check overflow on raw value, store shifted value
void Gte3D::set_mac(int idx, int64_t v)
{
    check_mac_overflow(idx, v);
    data_[D_MAC0 + (uint32_t)idx] = (uint32_t)(int32_t)v;  // Truncate to 32-bit
}

// set_mac_shifted: check overflow on raw (pre-shift), store raw >> shift
// This matches DuckStation's TruncateAndSetMAC which checks BEFORE shifting
void Gte3D::set_mac_shifted(int idx, int64_t raw, int shift)
{
    check_mac_overflow(idx, raw);
    data_[D_MAC0 + (uint32_t)idx] = (uint32_t)(int32_t)(raw >> shift);
}

void Gte3D::set_ir(int idx, int32_t v, int /*lm*/)
{
    // 3D shadow: NO saturation on IR registers.
    // Store raw value — preserves full precision for 3D reconstruction.
    // Primary Gte still saturates faithfully for 2D rendering.
    data_[D_IR0 + (uint32_t)idx] = (uint32_t)v;
}

void Gte3D::cmd_nclip(uint32_t)
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

void Gte3D::cmd_mvmva(uint32_t cmd)
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
void Gte3D::rtps_internal(const int32_t V[3], int sf, int lm, bool last)
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
    const uint32_t quotient = gte_divide_raw(h, sz3);

    const int64_t ofx = (int32_t)ctrl_[C_OFX];
    const int64_t ofy = (int32_t)ctrl_[C_OFY];
    const int32_t ir1 = (int32_t)(int16_t)(data_[D_IR1] & 0xFFFFu);
    const int32_t ir2 = (int32_t)(int16_t)(data_[D_IR2] & 0xFFFFu);

    const int64_t Sx = (int64_t)quotient * (int64_t)ir1 + ofx;
    const int64_t Sy = (int64_t)quotient * (int64_t)ir2 + ofy;

    // 3D shadow: skip MAC0 overflow checks (no game reads our flags)
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
void Gte3D::capture_snapshot(const int32_t (*verts)[3], int count)
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

void Gte3D::cmd_rtps(uint32_t cmd)
{
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;
    const int32_t V[3] = { vx(0), vy(0), vz(0) };
    rtps_internal(V, sf, lm, true);

    // 3D reconstruction: capture snapshot after RTPS (single vertex)
    capture_snapshot(&V, 1);

    // Tagging: store vertex in cache, encode index in SXY2
    after_rtps();
}

void Gte3D::cmd_rtpt(uint32_t cmd)
{
    // RTPT: RTPS on V0, V1, V2. DQA/DQB only on last vertex.
    // Uses push_sxy shift register naturally (3 pushes → SXY0=V0, SXY1=V1, SXY2=V2).
    const int sf = (cmd >> 19) & 1;
    const int lm = (cmd >> 10) & 1;

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

    // Tagging: store 3 vertices in cache, encode indices in SXY0/1/2
    after_rtpt();
}

void Gte3D::cmd_avsz3(uint32_t)
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
    // 3D shadow: NO clamping OTZ to [0, 0xFFFF]. Store raw value.
    data_[D_OTZ] = (uint32_t)(int32_t)(mac0 >> 12);
}

void Gte3D::cmd_avsz4(uint32_t)
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
    // 3D shadow: NO clamping OTZ to [0, 0xFFFF]. Store raw value.
    data_[D_OTZ] = (uint32_t)(int32_t)(mac0 >> 12);
}

void Gte3D::cmd_sqr(uint32_t cmd)
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

void Gte3D::cmd_gpf(uint32_t cmd)
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

void Gte3D::cmd_gpl(uint32_t cmd)
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

void Gte3D::cmd_op(uint32_t cmd)
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
void Gte3D::interpolate_color(int64_t in1, int64_t in2, int64_t in3, int shift, int lm)
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

void Gte3D::dpcs_internal(const uint8_t color[3], int shift, int lm)
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

void Gte3D::cmd_dpcs(uint32_t cmd)
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

void Gte3D::cmd_intpl(uint32_t cmd)
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

void Gte3D::cmd_ncs(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_nct(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_nccs(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_ncct(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_cc(uint32_t cmd)
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

void Gte3D::cmd_ncds(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_ncdt(uint32_t cmd)
{
    capture_normal();
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

void Gte3D::cmd_cdp(uint32_t cmd)
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

void Gte3D::cmd_dcpl(uint32_t cmd)
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

void Gte3D::cmd_dpct(uint32_t cmd)
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

int Gte3D::execute(uint32_t cop2_instruction)
{
    const uint32_t cmd = cop2_instruction & 0x01FF'FFFFu;
    const uint32_t funct = cmd & 0x3Fu;

    // Reset FLAG at start of every GTE command (DuckStation behavior)
    flag_ = 0;

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
        emu::logf(emu::LogLevel::warn, "GTE", "=== GTE command usage after 1000 calls ===");
        for (int i = 0; i < 64; i++)
        {
            if (gte_cmd_counts[i] > 0)
                emu::logf(emu::LogLevel::warn, "GTE", "  cmd 0x%02X: %u calls", i, gte_cmd_counts[i]);
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

    return cycles;
}

// ── Normal capture ──────────────────────────────────────────────────
void Gte3D::capture_normal()
{
    last_normal_[0] = (int16_t)(data_[D_VXY0] & 0xFFFFu);
    last_normal_[1] = (int16_t)(data_[D_VXY0] >> 16);
    last_normal_[2] = (int16_t)(data_[D_VZ0] & 0xFFFFu);
}

// ── Tagging: differential lattice encoding ───────────────────────────
//
// Differential encoding: face_idx encoded in DIFFERENCE between carrier and
// reference vertices. Immune to uniform add/sub (offset cancels).
//
// RTPT:  V0 = carrier, V1 = reference, V2 = carrier (redundant)
// Quad:  V3 = carrier (same face_idx, tagged by after_rtps)

uint16_t Gte3D::encode_coord(uint32_t index_8bits)
{
    // Backward compat for standalone RTPS tagging
    return (uint16_t)(REF_BASE + (index_8bits & 0xFFu) * SPACING);
}

void Gte3D::tag_sxy(uint32_t sxy_reg, uint32_t vertex_idx)
{
    // Backward compat for standalone RTPS
    const uint16_t x_encoded = encode_coord((vertex_idx >> 8) & 0xFFu);
    const uint16_t y_encoded = encode_coord(vertex_idx & 0xFFu);
    data_[sxy_reg] = ((uint32_t)y_encoded << 16) | (uint32_t)x_encoded;
}

void Gte3D::tag_face_differential(uint32_t face_idx)
{
    const uint16_t lo = (uint16_t)(face_idx & 0xFFu);
    const uint16_t hi = (uint16_t)((face_idx >> 8) & 0xFFu);
    const uint16_t carrier_x = (uint16_t)(REF_BASE + lo * SPACING);
    const uint16_t carrier_y = (uint16_t)(REF_BASE + hi * SPACING);
    const uint16_t ref_x = REF_BASE;
    const uint16_t ref_y = REF_BASE;

    // V0 = carrier, V1 = reference, V2 = carrier (redundant for validation)
    data_[D_SXY0] = ((uint32_t)carrier_y << 16) | (uint32_t)carrier_x;
    data_[D_SXY1] = ((uint32_t)ref_y << 16) | (uint32_t)ref_x;
    data_[D_SXY2] = ((uint32_t)carrier_y << 16) | (uint32_t)carrier_x;

    // Save for quad detection (after_rtps will use these)
    last_carrier_x_ = carrier_x;
    last_carrier_y_ = carrier_y;
    last_face_idx_ = face_idx;
    last_was_rtpt_ = true;
}

uint32_t Gte3D::store_vertex(const GteSnapshot& snap, int vert_idx)
{
    const uint32_t idx = vertex_index_;
    if (idx > MAX_FACE_INDEX)
        return idx;

    if (idx >= write_cache_.size())
        write_cache_.resize(idx + 1);

    GteCacheVertex& entry = write_cache_[idx];
    entry.vx = snap.vertices[vert_idx].vx;
    entry.vy = snap.vertices[vert_idx].vy;
    entry.vz = snap.vertices[vert_idx].vz;
    entry.nx = last_normal_[0];
    entry.ny = last_normal_[1];
    entry.nz = last_normal_[2];
    entry.transform = snap.transform;
    entry.sx = snap.sx[vert_idx];
    entry.sy = snap.sy[vert_idx];
    entry.sz = snap.sz[vert_idx];

    ++vertex_index_;
    return idx;
}

uint32_t Gte3D::store_face(const GteSnapshot& snap)
{
    const uint32_t fi = face_index_;
    if (fi > MAX_FACE_INDEX)
        return fi;

    if (fi >= write_face_cache_.size())
        write_face_cache_.resize(fi + 1);

    GteCacheFace& face = write_face_cache_[fi];
    for (int i = 0; i < 3; ++i)
    {
        face.vx[i] = snap.vertices[i].vx;
        face.vy[i] = snap.vertices[i].vy;
        face.vz[i] = snap.vertices[i].vz;
        face.nx[i] = last_normal_[0];
        face.ny[i] = last_normal_[1];
        face.nz[i] = last_normal_[2];
        face.sx[i] = snap.sx[i];
        face.sy[i] = snap.sy[i];
        face.sz[i] = snap.sz[i];
    }
    face.transform = snap.transform;

    ++face_index_;
    return fi;
}

void Gte3D::store_quad(const GteSnapshot& rtps_snap, uint32_t face_idx)
{
    if (face_idx >= write_quad_cache_.size())
        write_quad_cache_.resize(face_idx + 1);

    GteCacheQuad& quad = write_quad_cache_[face_idx];

    // First 3 vertices from saved RTPT snapshot
    for (int i = 0; i < 3; ++i)
    {
        quad.vx[i] = last_rtpt_snapshot_.vertices[i].vx;
        quad.vy[i] = last_rtpt_snapshot_.vertices[i].vy;
        quad.vz[i] = last_rtpt_snapshot_.vertices[i].vz;
        quad.nx[i] = last_normal_[0];
        quad.ny[i] = last_normal_[1];
        quad.nz[i] = last_normal_[2];
        quad.sx[i] = last_rtpt_snapshot_.sx[i];
        quad.sy[i] = last_rtpt_snapshot_.sy[i];
        quad.sz[i] = last_rtpt_snapshot_.sz[i];
    }

    // 4th vertex from RTPS snapshot (vertex index 2 = SXY2 output)
    quad.vx[3] = rtps_snap.vertices[2].vx;
    quad.vy[3] = rtps_snap.vertices[2].vy;
    quad.vz[3] = rtps_snap.vertices[2].vz;
    quad.nx[3] = last_normal_[0];
    quad.ny[3] = last_normal_[1];
    quad.nz[3] = last_normal_[2];
    quad.sx[3] = rtps_snap.sx[2];
    quad.sy[3] = rtps_snap.sy[2];
    quad.sz[3] = rtps_snap.sz[2];

    quad.transform = last_rtpt_snapshot_.transform;
    ++quad_count_;
}

void Gte3D::after_rtps()
{
    if (!last_snapshot_.valid || vertex_index_ > MAX_FACE_INDEX)
        return;

    // Store real SXY in lookup table BEFORE tagging overwrites it
    const uint32_t sxy = data_[D_SXY2];
    const uint32_t idx = store_vertex(last_snapshot_, 2);
    write_sxy_table_[sxy] = idx;

    // Quad detection: if previous command was RTPT, this is the 4th vertex
    if (last_was_rtpt_ && last_face_idx_ != 0xFFFFFFFFu &&
        last_face_idx_ < write_face_cache_.size())
    {
        // Create quad face entry (3 from RTPT + 1 from this RTPS)
        store_quad(last_snapshot_, last_face_idx_);

        // Tag V3 (SXY2) as carrier with same face_idx as the RTPT
        data_[D_SXY2] = ((uint32_t)last_carrier_y_ << 16) | (uint32_t)last_carrier_x_;
    }
    else
    {
        // Standalone RTPS: tag with vertex index (backward compat)
        tag_sxy(D_SXY2, idx);
    }

    last_was_rtpt_ = false;
}

void Gte3D::after_rtpt()
{
    if (!last_snapshot_.valid || vertex_index_ + 2 > MAX_FACE_INDEX)
        return;

    // Store real SXY values in lookup table BEFORE tagging
    const uint32_t sxy0 = data_[D_SXY0];
    const uint32_t sxy1 = data_[D_SXY1];
    const uint32_t sxy2 = data_[D_SXY2];

    // Per-vertex cache (backward compat for RTPS-based lookups)
    const uint32_t idx0 = store_vertex(last_snapshot_, 0);
    const uint32_t idx1 = store_vertex(last_snapshot_, 1);
    const uint32_t idx2 = store_vertex(last_snapshot_, 2);

    write_sxy_table_[sxy0] = idx0;
    write_sxy_table_[sxy1] = idx1;
    write_sxy_table_[sxy2] = idx2;

    // Face cache: store all 3 vertices as one face
    const uint32_t fi = store_face(last_snapshot_);

    // Save previous face_idx BEFORE tag_face_differential overwrites it
    // (tag_face_differential sets last_face_idx_ = fi for RTPS quad path)
    const uint32_t prev_face_idx = last_face_idx_;
    const bool prev_was_rtpt = last_was_rtpt_;

    // Differential tagging: V0=carrier, V1=reference, V2=carrier
    // NOTE: this sets last_face_idx_ = fi and last_was_rtpt_ = true
    tag_face_differential(fi);

    // ── Quad detection from consecutive RTPTs sharing 2 vertices ──
    // Games (e.g. Ridge Racer) use 2×RTPT for quads instead of RTPT+RTPS.
    // Pattern: RTPT #1 → (A, B, C), RTPT #2 → (B, C, D) with 2 shared verts.
    // We create a quad_cache entry for the PREVIOUS face_idx containing all 4
    // unique vertices so the GPU decoder can split the quad correctly.

    if (prev_was_rtpt && prev_face_idx != 0xFFFFFFFFu &&
        prev_face_idx < write_face_cache_.size())
    {
        // Find the unique vertex in current RTPT (not present in previous RTPT).
        // Compare 3D positions (vx, vy, vz) since screen coords are tagged.
        int unique_curr = -1;
        int shared_count = 0;
        for (int j = 0; j < 3; ++j)
        {
            bool found = false;
            for (int i = 0; i < 3; ++i)
            {
                if (last_snapshot_.vertices[j].vx == last_rtpt_snapshot_.vertices[i].vx &&
                    last_snapshot_.vertices[j].vy == last_rtpt_snapshot_.vertices[i].vy &&
                    last_snapshot_.vertices[j].vz == last_rtpt_snapshot_.vertices[i].vz)
                {
                    found = true;
                    ++shared_count;
                    break;
                }
            }
            if (!found && unique_curr < 0)
                unique_curr = j;
        }

        // Exactly 2 shared + 1 unique → these two RTPTs form a quad
        if (shared_count == 2 && unique_curr >= 0)
        {
            // Build quad: V0-V2 from previous RTPT, V3 = unique vertex from current RTPT
            if (prev_face_idx >= write_quad_cache_.size())
                write_quad_cache_.resize(prev_face_idx + 1);

            GteCacheQuad& quad = write_quad_cache_[prev_face_idx];
            for (int i = 0; i < 3; ++i)
            {
                quad.vx[i] = last_rtpt_snapshot_.vertices[i].vx;
                quad.vy[i] = last_rtpt_snapshot_.vertices[i].vy;
                quad.vz[i] = last_rtpt_snapshot_.vertices[i].vz;
                quad.nx[i] = last_normal_[0];
                quad.ny[i] = last_normal_[1];
                quad.nz[i] = last_normal_[2];
                quad.sx[i] = last_rtpt_snapshot_.sx[i];
                quad.sy[i] = last_rtpt_snapshot_.sy[i];
                quad.sz[i] = last_rtpt_snapshot_.sz[i];
            }
            const int u = unique_curr;
            quad.vx[3] = last_snapshot_.vertices[u].vx;
            quad.vy[3] = last_snapshot_.vertices[u].vy;
            quad.vz[3] = last_snapshot_.vertices[u].vz;
            quad.nx[3] = last_normal_[0];
            quad.ny[3] = last_normal_[1];
            quad.nz[3] = last_normal_[2];
            quad.sx[3] = last_snapshot_.sx[u];
            quad.sy[3] = last_snapshot_.sy[u];
            quad.sz[3] = last_snapshot_.sz[u];
            quad.transform = last_rtpt_snapshot_.transform;
            ++quad_count_;
        }
    }

    // Save state for next RTPT's quad detection and for RTPS quad path
    last_rtpt_snapshot_ = last_snapshot_;
    last_was_rtpt_ = true;
    last_face_idx_ = fi;
}

uint32_t Gte3D::lookup_by_sxy(uint32_t sxy_packed) const
{
    auto it = read_sxy_table_.find(sxy_packed);
    if (it != read_sxy_table_.end())
        return it->second;
    it = write_sxy_table_.find(sxy_packed);
    if (it != write_sxy_table_.end())
        return it->second;
    return 0xFFFFFFFFu;
}

const GteCacheVertex* Gte3D::vertex_by_index(uint32_t index) const
{
    if (index < read_cache_.size())
        return &read_cache_[index];
    if (index < write_cache_.size())
        return &write_cache_[index];
    return nullptr;
}

const GteCacheFace* Gte3D::face_by_index(uint32_t index) const
{
    if (index < read_face_cache_.size())
        return &read_face_cache_[index];
    if (index < write_face_cache_.size())
        return &write_face_cache_[index];
    return nullptr;
}

const GteCacheQuad* Gte3D::quad_by_index(uint32_t index) const
{
    if (index < read_quad_cache_.size())
        return &read_quad_cache_[index];
    if (index < write_quad_cache_.size())
        return &write_quad_cache_[index];
    return nullptr;
}

void Gte3D::copy_ready_cache(std::vector<GteCacheVertex>& out) const
{
    std::lock_guard<std::mutex> lock(cache_mutex_);
    out = read_cache_;
}

void Gte3D::copy_ready_face_cache(std::vector<GteCacheFace>& out) const
{
    std::lock_guard<std::mutex> lock(cache_mutex_);
    out = read_face_cache_;
}

void Gte3D::copy_ready_quad_cache(std::vector<GteCacheQuad>& out) const
{
    std::lock_guard<std::mutex> lock(cache_mutex_);
    out = read_quad_cache_;
}

void Gte3D::swap_frame()
{
    {
        std::lock_guard<std::mutex> lock(cache_mutex_);
        read_cache_ = std::move(write_cache_);
        read_face_cache_ = std::move(write_face_cache_);
        read_quad_cache_ = std::move(write_quad_cache_);
    }

    write_cache_.clear();
    write_cache_.reserve(4096);
    write_face_cache_.clear();
    write_face_cache_.reserve(2048);
    write_quad_cache_.clear();
    write_quad_cache_.reserve(1024);

    read_sxy_table_ = std::move(write_sxy_table_);
    write_sxy_table_.clear();
    write_sxy_table_.reserve(4096);

    vertex_index_ = 1;
    face_index_ = 1;
    quad_count_ = 0;
    last_was_rtpt_ = false;
    last_face_idx_ = 0xFFFFFFFFu;
}

} // namespace gte
