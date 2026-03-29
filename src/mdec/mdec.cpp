#include "mdec.h"
#include "../log/emu_log.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <cstring>

namespace mdec
{

// Zigzag scan order (PSX-SPX: "MDEC Compressed Data Format")
const uint8_t Mdec::s_zagzig[64] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

// Sign-extend N-bit value to int32 (truncate to N bits first, then sign-extend)
template <int N>
static int32_t sign_extend(int32_t v)
{
    const int32_t mask = 1 << (N - 1);
    v &= (1 << N) - 1;     // mask to N bits (unsigned)
    return (v ^ mask) - mask; // sign-extend
}

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

Mdec::Mdec()
{
    reset();
}

void Mdec::reset()
{
    state_ = State::idle;
    output_depth_ = 0;
    output_signed_ = false;
    output_bit15_ = 0;
    enable_dma_in_ = false;
    enable_dma_out_ = false;
    remaining_halfwords_ = 0;
    current_block_ = 0;
    current_coeff_ = 64;
    current_q_scale_ = 0;
    fifo_in_.clear();
    fifo_in_pos_ = 0;
    fifo_out_.clear();
    fifo_out_pos_ = 0;
    iq_y_.fill(0);
    iq_uv_.fill(0);
    scale_table_.fill(0);
    for (auto& b : blocks_) b.fill(0);
    block_rgb_.fill(0);
}

uint16_t Mdec::fifo_in_pop()
{
    if (fifo_in_pos_ < fifo_in_.size())
        return fifo_in_[fifo_in_pos_++];
    return 0;
}

void Mdec::fifo_out_push(uint32_t v)
{
    fifo_out_.push_back(v);
}

// ---------------------------------------------------------------------------
// Register read (0x1F801820 = data, 0x1F801824 = status)
// ---------------------------------------------------------------------------
uint32_t Mdec::read_reg(uint32_t addr)
{
    if (addr == 0x1F80'1824u)
    {
        // Status register (PSX-SPX)
        uint32_t st = remaining_halfwords_ & 0xFFFFu;
        st |= ((uint32_t)current_block_ & 7u) << 16;
        st |= ((uint32_t)output_bit15_) << 23;
        st |= ((uint32_t)output_signed_ ? 1u : 0u) << 24;
        st |= ((uint32_t)output_depth_ & 3u) << 25;
        if (enable_dma_out_ && fifo_out_pos_ < fifo_out_.size())
            st |= (1u << 27); // data-out request
        if (enable_dma_in_)
            st |= (1u << 28); // data-in request
        if (state_ == State::decoding)
            st |= (1u << 29); // command busy
        if (fifo_in_.size() >= 0xFFF)
            st |= (1u << 30); // data-in FIFO full (approximate)
        if (fifo_out_pos_ >= fifo_out_.size())
            st |= (1u << 31); // data-out FIFO empty
        return st;
    }

    // 0x1F801820: data output (read decoded pixels)
    if (fifo_out_pos_ < fifo_out_.size())
        return fifo_out_[fifo_out_pos_++];
    return 0;
}

// ---------------------------------------------------------------------------
// Register write (0x1F801820 = command/data, 0x1F801824 = control)
// ---------------------------------------------------------------------------
void Mdec::write_reg(uint32_t addr, uint32_t val)
{
    if (addr == 0x1F80'1824u)
    {
        // Control register
        static uint32_t ctrl_log = 0;
        if (ctrl_log < 20u)
            emu::logf(emu::LogLevel::warn, "MDEC",
                "CTRL write 0x%08X reset=%d dma_in=%d dma_out=%d fifo_out=%zu/%zu (#%u)",
                val, (val >> 31) & 1, (val >> 30) & 1, (val >> 29) & 1,
                fifo_out_pos_, fifo_out_.size(), ++ctrl_log);
        if (val & 0x8000'0000u)
            reset();
        enable_dma_in_ = (val & (1u << 30)) != 0;
        enable_dma_out_ = (val & (1u << 29)) != 0;
        return;
    }

    // 0x1F801820: command/data input
    fifo_in_.push_back((uint16_t)(val & 0xFFFF));
    fifo_in_.push_back((uint16_t)(val >> 16));
    execute();
}

// ---------------------------------------------------------------------------
// DMA0 write (MDEC IN): push compressed words
// ---------------------------------------------------------------------------
void Mdec::dma_write(const uint32_t* words, uint32_t count)
{
    static uint32_t dma_in_log = 0;
    if (dma_in_log < 10)
    {
        ++dma_in_log;
        emu::logf(emu::LogLevel::warn, "MDEC",
            "DMA_IN: %u words, state=%d, fifo_in=%zu, fifo_out=%zu (#%u)",
            count, (int)state_, fifo_in_.size(), fifo_out_.size(), dma_in_log);
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        fifo_in_.push_back((uint16_t)(words[i] & 0xFFFF));
        fifo_in_.push_back((uint16_t)(words[i] >> 16));
    }
    execute();
    if (dma_in_log <= 10)
    {
        emu::logf(emu::LogLevel::warn, "MDEC",
            "DMA_IN done: state=%d, fifo_in=%zu/%zu, fifo_out=%zu",
            (int)state_, fifo_in_pos_, fifo_in_.size(), fifo_out_.size());
    }
}

// ---------------------------------------------------------------------------
// DMA1 read (MDEC OUT): pull decoded pixel words
// ---------------------------------------------------------------------------
void Mdec::dma_read(uint32_t* words, uint32_t count)
{
    static uint32_t dma_out_log = 0;
    if (dma_out_log < 10)
    {
        ++dma_out_log;
        emu::logf(emu::LogLevel::warn, "MDEC",
            "DMA_OUT: %u words requested, fifo_out=%zu/%zu (#%u)",
            count, fifo_out_pos_, fifo_out_.size(), dma_out_log);
    }
    for (uint32_t i = 0; i < count; ++i)
    {
        if (fifo_out_pos_ < fifo_out_.size())
            words[i] = fifo_out_[fifo_out_pos_++];
        else
            words[i] = 0;
    }
}

bool Mdec::data_out_request() const
{
    return enable_dma_out_ && (fifo_out_pos_ < fifo_out_.size());
}

bool Mdec::data_in_request() const
{
    return enable_dma_in_;
}

// ---------------------------------------------------------------------------
// Command execution state machine
// ---------------------------------------------------------------------------
void Mdec::execute()
{
    for (;;)
    {
        switch (state_)
        {
        case State::idle:
        {
            if (fifo_in_.size() - fifo_in_pos_ < 2)
                return;

            // Read command word
            uint32_t cw = (uint32_t)fifo_in_[fifo_in_pos_] |
                          ((uint32_t)fifo_in_[fifo_in_pos_ + 1] << 16);
            fifo_in_pos_ += 2;

            uint8_t cmd = (cw >> 29) & 7u;
            output_depth_ = (cw >> 27) & 3u;
            output_signed_ = (cw & (1u << 26)) != 0;
            output_bit15_ = (cw >> 25) & 1u;

            fifo_out_.clear();
            fifo_out_pos_ = 0;

            {
                static uint32_t cmd_log = 0;
                if (cmd_log < 20)
                {
                    ++cmd_log;
                    emu::logf(emu::LogLevel::warn, "MDEC",
                        "CMD %u (cw=0x%08X) depth=%u signed=%d words=%u (#%u)",
                        cmd, cw, output_depth_, output_signed_ ? 1 : 0,
                        cw & 0xFFFF, cmd_log);
                }
            }

            switch (cmd)
            {
            case 1: // DecodeMacroblock
                remaining_halfwords_ = (cw & 0xFFFF) * 2;
                state_ = State::decoding;
                current_block_ = 0;
                current_coeff_ = 64;
                break;
            case 2: // SetQuantTable
                remaining_halfwords_ = 32 + ((cw & 1) ? 32 : 0);
                state_ = State::set_quant;
                break;
            case 3: // SetScaleTable
                remaining_halfwords_ = 64;
                state_ = State::set_scale;
                break;
            default:
                remaining_halfwords_ = (cw & 0xFFFF) * 2;
                // Skip unknown command data
                {
                    uint32_t skip = std::min(remaining_halfwords_,
                        (uint32_t)(fifo_in_.size() - fifo_in_pos_));
                    fifo_in_pos_ += skip;
                    remaining_halfwords_ -= skip;
                }
                if (remaining_halfwords_ == 0)
                    state_ = State::idle;
                return;
            }
            continue;
        }

        case State::decoding:
        {
            static uint32_t mb_count = 0;
            if (!decode_macroblock())
            {
                // decode_rle can exhaust remaining_halfwords_ mid-block
                // (padding skip loop exits when remaining hits 0).
                if (remaining_halfwords_ == 0)
                {
                    emu::logf(emu::LogLevel::warn, "MDEC",
                        "Frame decode END: %u MBs, fifo_out=%zu",
                        mb_count, fifo_out_.size());
                    mb_count = 0;
                    state_ = State::idle;
                    dump_frame_ppm();
                }
                return;   // need more data or frame complete
            }
            mb_count++;
            continue;     // decoded one MB, try next
        }

        case State::set_quant:
        {
            uint32_t avail = (uint32_t)(fifo_in_.size() - fifo_in_pos_);
            if (avail < remaining_halfwords_)
                return;

            // First 64 bytes = Y quant table
            for (int i = 0; i < 32; ++i)
            {
                uint16_t hw = fifo_in_[fifo_in_pos_++];
                iq_y_[i * 2]     = (uint8_t)(hw & 0xFF);
                iq_y_[i * 2 + 1] = (uint8_t)(hw >> 8);
            }
            remaining_halfwords_ -= 32;

            // Optional second 64 bytes = UV quant table
            if (remaining_halfwords_ >= 32)
            {
                for (int i = 0; i < 32; ++i)
                {
                    uint16_t hw = fifo_in_[fifo_in_pos_++];
                    iq_uv_[i * 2]     = (uint8_t)(hw & 0xFF);
                    iq_uv_[i * 2 + 1] = (uint8_t)(hw >> 8);
                }
                remaining_halfwords_ -= 32;
            }

            state_ = State::idle;
            continue;
        }

        case State::set_scale:
        {
            uint32_t avail = (uint32_t)(fifo_in_.size() - fifo_in_pos_);
            if (avail < 64)
                return;

            // Read 64 halfwords and transpose into scale_table_
            // Input is column-major, we store row-major for IDCT
            int16_t raw[64];
            for (int i = 0; i < 64; ++i)
                raw[i] = (int16_t)fifo_in_[fifo_in_pos_++];
            remaining_halfwords_ -= 64;

            for (int y = 0; y < 8; ++y)
                for (int x = 0; x < 8; ++x)
                    scale_table_[y * 8 + x] = raw[x * 8 + y];

            state_ = State::idle;
            continue;
        }
        }
    }
}

// ---------------------------------------------------------------------------
// Macroblock decoding: 6 blocks (Cr, Cb, Y1-Y4)
// ---------------------------------------------------------------------------
bool Mdec::decode_macroblock()
{
    for (; current_block_ < 6; ++current_block_)
    {
        const uint8_t* qt = (current_block_ >= 2) ? iq_y_.data() : iq_uv_.data();
        if (!decode_rle(blocks_[current_block_].data(), qt))
            return false; // need more data

        idct(blocks_[current_block_].data());
    }

    // All 6 blocks decoded — convert YUV→RGB into block_rgb_
    if (output_depth_ == 1) // 8-bit mono
    {
        // Mono uses only Y1 block
        yuv_to_mono(blocks_[2].data());
    }
    else
    {
        // Color: 4 Y blocks at (0,0), (8,0), (0,8), (8,8)
        yuv_to_rgb(0, 0, blocks_[0].data(), blocks_[1].data(), blocks_[2].data());
        yuv_to_rgb(8, 0, blocks_[0].data(), blocks_[1].data(), blocks_[3].data());
        yuv_to_rgb(0, 8, blocks_[0].data(), blocks_[1].data(), blocks_[4].data());
        yuv_to_rgb(8, 8, blocks_[0].data(), blocks_[1].data(), blocks_[5].data());
    }

    // Diagnostic: dump first 3 decoded macroblocks (pixel corner samples)
    {
        static uint32_t mb_log = 0;
        if (mb_log < 3)
        {
            ++mb_log;
            // Log Cr/Cb DC values and first pixel RGB
            int16_t cr_dc = blocks_[0][0];
            int16_t cb_dc = blocks_[1][0];
            int16_t y_dc  = blocks_[2][0];
            uint32_t px0  = block_rgb_[0];
            uint32_t px1  = block_rgb_[1];
            uint8_t  r0   = (uint8_t)(px0 & 0xFF);
            uint8_t  g0   = (uint8_t)((px0 >> 8) & 0xFF);
            uint8_t  b0   = (uint8_t)((px0 >> 16) & 0xFF);
            emu::logf(emu::LogLevel::warn, "MDEC",
                "MB#%u depth=%u Cr_dc=%d Cb_dc=%d Y_dc=%d px[0]=(%u,%u,%u) px[1]=0x%06X",
                mb_log, output_depth_, cr_dc, cb_dc, y_dc, r0, g0, b0, px1 & 0xFFFFFF);
        }
    }

    copy_out_block();

    // Reset for next macroblock
    current_block_ = 0;
    current_coeff_ = 64;

    if (remaining_halfwords_ == 0)
    {
        state_ = State::idle;
        dump_frame_ppm();
    }
    return true;
}

// ---------------------------------------------------------------------------
// RLE decode (PSX-SPX: "MDEC Decompression")
// ---------------------------------------------------------------------------
bool Mdec::decode_rle(int16_t* blk, const uint8_t* qt)
{
    if (current_coeff_ == 64)
    {
        // Start new block: clear coefficients
        std::memset(blk, 0, 64 * sizeof(int16_t));

        // Skip padding (0xFE00)
        uint16_t n;
        for (;;)
        {
            if (fifo_in_empty() || remaining_halfwords_ == 0)
                return false;
            n = fifo_in_pop();
            remaining_halfwords_--;
            if (n != 0xFE00)
                break;
        }

        current_coeff_ = 0;
        current_q_scale_ = (n >> 10) & 0x3F;

        int32_t val = sign_extend<10>((int32_t)(n & 0x3FF));
        if (current_q_scale_ == 0)
            val = val * 2;
        else
            val = val * (int32_t)qt[0];

        val = clamp_i32(val, -0x400, 0x3FF);
        if (current_q_scale_ > 0)
            blk[s_zagzig[0]] = (int16_t)val;
        else
            blk[0] = (int16_t)val;
    }

    while (!fifo_in_empty() && remaining_halfwords_ > 0)
    {
        uint16_t n = fifo_in_pop();
        remaining_halfwords_--;

        current_coeff_ += ((n >> 10) & 0x3F) + 1;
        if (current_coeff_ < 64)
        {
            int32_t val = sign_extend<10>((int32_t)(n & 0x3FF));
            if (current_q_scale_ == 0)
                val = val * 2;
            else
                val = (val * (int32_t)qt[current_coeff_] * (int32_t)current_q_scale_ + 4) / 8;

            val = clamp_i32(val, -0x400, 0x3FF);
            if (current_q_scale_ > 0)
                blk[s_zagzig[current_coeff_]] = (int16_t)val;
            else
                blk[current_coeff_] = (int16_t)val;
        }

        if (current_coeff_ >= 63)
        {
            current_coeff_ = 64;
            return true;
        }
    }

    return false;
}

// ---------------------------------------------------------------------------
// IDCT 8×8 (2-pass separable, using scale_table_ as cosine matrix)
// ---------------------------------------------------------------------------
void Mdec::idct(int16_t* blk)
{
    int64_t temp[64];

    // Pass 1: columns (DuckStation IDCT_Old)
    for (int x = 0; x < 8; ++x)
    {
        for (int y = 0; y < 8; ++y)
        {
            int64_t sum = 0;
            for (int u = 0; u < 8; ++u)
                sum += (int32_t)blk[u * 8 + x] * (int32_t)scale_table_[y * 8 + u];
            temp[x + y * 8] = sum;
        }
    }

    // Pass 2: rows, with final rounding and clamping to [-128, 127]
    for (int x = 0; x < 8; ++x)
    {
        for (int y = 0; y < 8; ++y)
        {
            int64_t sum = 0;
            for (int u = 0; u < 8; ++u)
                sum += temp[u + y * 8] * (int32_t)scale_table_[x * 8 + u];

            // Round and convert: result has 32 fractional bits
            int32_t r = (int32_t)((sum >> 32) + ((sum >> 31) & 1));
            // Sign-extend 9 bits then clamp to [-128, 127]
            r = sign_extend<9>(r);
            blk[x + y * 8] = (int16_t)clamp_i32(r, -128, 127);
        }
    }
}

// ---------------------------------------------------------------------------
// YUV → RGB conversion (BT.601, matching PS1 hardware)
// ---------------------------------------------------------------------------
void Mdec::yuv_to_rgb(uint32_t xx, uint32_t yy,
                      const int16_t* cr_blk, const int16_t* cb_blk,
                      const int16_t* y_blk)
{
    const int16_t addval = output_signed_ ? 0 : 0x80;

    for (uint32_t y = 0; y < 8; ++y)
    {
        for (uint32_t x = 0; x < 8; ++x)
        {
            int16_t Cr = cr_blk[((x + xx) / 2) + ((y + yy) / 2) * 8];
            int16_t Cb = cb_blk[((x + xx) / 2) + ((y + yy) / 2) * 8];

            // BT.601 conversion (float, matches PS1 MDEC output)
            int16_t R_delta = (int16_t)(1.402f * (float)Cr);
            int16_t B_delta = (int16_t)(1.772f * (float)Cb);
            int16_t G_delta = (int16_t)((-0.3437f * (float)Cb) + (-0.7143f * (float)Cr));

            int16_t Y = y_blk[x + y * 8];
            int16_t R = (int16_t)clamp_i32((int32_t)Y + R_delta, -128, 127) + addval;
            int16_t G = (int16_t)clamp_i32((int32_t)Y + G_delta, -128, 127) + addval;
            int16_t B = (int16_t)clamp_i32((int32_t)Y + B_delta, -128, 127) + addval;

            block_rgb_[(x + xx) + (y + yy) * 16] =
                ((uint32_t)(uint8_t)R) |
                ((uint32_t)(uint8_t)G << 8) |
                ((uint32_t)(uint8_t)B << 16);
        }
    }
}

void Mdec::yuv_to_mono(const int16_t* y_blk)
{
    const int32_t addval = output_signed_ ? 0 : 0x80;
    for (int i = 0; i < 64; ++i)
    {
        int32_t v = clamp_i32(sign_extend<9>((int32_t)y_blk[i]), -128, 127) + addval;
        block_rgb_[i] = (uint32_t)(uint8_t)v;
    }
}

// ---------------------------------------------------------------------------
// Copy decoded block_rgb_ to output FIFO (format depends on output_depth_)
// ---------------------------------------------------------------------------
void Mdec::copy_out_block()
{
    switch (output_depth_)
    {
    case 0: // 4-bit (rarely used)
    {
        const uint32_t* in = block_rgb_.data();
        for (int i = 0; i < 64 / 8; ++i)
        {
            uint32_t v = (in[0] >> 4);
            v |= (in[1] >> 4) << 4;
            v |= (in[2] >> 4) << 8;
            v |= (in[3] >> 4) << 12;
            v |= (in[4] >> 4) << 16;
            v |= (in[5] >> 4) << 20;
            v |= (in[6] >> 4) << 24;
            v |= (in[7] >> 4) << 28;
            fifo_out_push(v);
            in += 8;
        }
        break;
    }
    case 1: // 8-bit
    {
        const uint32_t* in = block_rgb_.data();
        for (int i = 0; i < 64 / 4; ++i)
        {
            uint32_t v = in[0] & 0xFF;
            v |= (in[1] & 0xFF) << 8;
            v |= (in[2] & 0xFF) << 16;
            v |= (in[3] & 0xFF) << 24;
            fifo_out_push(v);
            in += 4;
        }
        break;
    }
    case 2: // 24-bit RGB
    {
        // Pack RGB24 tightly: 3 bytes per pixel, 4 pixels = 12 bytes = 3 words
        uint32_t idx = 0;
        uint32_t pack_state = 0;
        uint32_t rgb = 0;
        while (idx < 256)
        {
            switch (pack_state)
            {
            case 0:
                rgb = block_rgb_[idx++] & 0x00FFFFFFu; // RGB-
                pack_state = 1;
                break;
            case 1:
                rgb |= (block_rgb_[idx] & 0xFF) << 24; // RGBR
                fifo_out_push(rgb);
                rgb = block_rgb_[idx] >> 8; // GB--
                idx++;
                pack_state = 2;
                break;
            case 2:
                rgb |= (block_rgb_[idx] & 0xFFFF) << 16; // GBRG
                fifo_out_push(rgb);
                rgb = block_rgb_[idx] >> 16; // B---
                idx++;
                pack_state = 3;
                break;
            case 3:
                rgb |= block_rgb_[idx] << 8; // BRGB
                fifo_out_push(rgb);
                idx++;
                pack_state = 0;
                break;
            }
        }
        break;
    }
    case 3: // 15-bit
    {
        const uint16_t a = (uint16_t)output_bit15_ << 15;
        for (uint32_t i = 0; i < 256; i += 2)
        {
            uint32_t c0 = block_rgb_[i];
            uint16_t r0 = (uint16_t)((c0 >> 3) & 0x1F);
            uint16_t g0 = (uint16_t)((c0 >> 11) & 0x1F);
            uint16_t b0 = (uint16_t)((c0 >> 19) & 0x1F);
            uint16_t p0 = r0 | (g0 << 5) | (b0 << 10) | a;

            uint32_t c1 = block_rgb_[i + 1];
            uint16_t r1 = (uint16_t)((c1 >> 3) & 0x1F);
            uint16_t g1 = (uint16_t)((c1 >> 11) & 0x1F);
            uint16_t b1 = (uint16_t)((c1 >> 19) & 0x1F);
            uint16_t p1 = r1 | (g1 << 5) | (b1 << 10) | a;

            fifo_out_push((uint32_t)p0 | ((uint32_t)p1 << 16));
        }
        break;
    }
    }
}

// ---------------------------------------------------------------------------
// enable_frame_dump / dump_frame_ppm
// ---------------------------------------------------------------------------
void Mdec::enable_frame_dump(const char* path_prefix, int frame_w, int frame_h, int max_frames)
{
    dump_prefix_ = path_prefix ? path_prefix : "";
    dump_w_ = frame_w;
    dump_h_ = frame_h;
    dump_max_ = max_frames;
    dump_count_ = 0;
    emu::logf(emu::LogLevel::info, "MDEC",
        "frame dump enabled: prefix=%s w=%d h=%d max=%d",
        dump_prefix_.c_str(), dump_w_, dump_h_, dump_max_);
}

void Mdec::dump_frame_ppm()
{
    if (dump_count_ >= dump_max_ || dump_prefix_.empty())
        return;
    if (output_depth_ != 2 && output_depth_ != 3)
        return;

    // Auto-detect frame dimensions from fifo size
    int W = dump_w_, H = dump_h_;
    if (W == 0 || H == 0)
    {
        const int wpb = (output_depth_ == 2) ? 192 : 128;
        const int total_mbs = (int)(fifo_out_.size() / wpb);
        struct { int w, h; } sizes[] = {
            {320,240},{320,256},{320,160},{640,480},{256,240},{512,240},
        };
        for (auto& s : sizes)
            if ((s.w/16)*(s.h/16) == total_mbs) { W=s.w; H=s.h; break; }
        if (W == 0) { W = total_mbs*16; H = 16; }
    }

    char path[512];
    std::snprintf(path, sizeof(path), "%s_%04d.ppm", dump_prefix_.c_str(), dump_count_);

    std::ofstream f(path, std::ios::binary);
    if (!f) return;

    f << "P6\n" << W << " " << H << "\n255\n";
    const int mb_cols = W / 16;

    if (output_depth_ == 2) // 24-bit
    {
        const uint8_t* src = reinterpret_cast<const uint8_t*>(fifo_out_.data());
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int off = ((y/16)*mb_cols+(x/16))*768 + ((y%16)*16+(x%16))*3;
                f.put((char)src[off]); f.put((char)src[off+1]); f.put((char)src[off+2]);
            }
    }
    else // 15-bit
    {
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++)
            {
                int mb = (y/16)*mb_cols+(x/16);
                int p = (y%16)*16+(x%16);
                uint32_t w = fifo_out_[mb*128+p/2];
                uint16_t px = (p&1) ? (uint16_t)(w>>16) : (uint16_t)(w&0xFFFF);
                f.put((char)(uint8_t)(((px)&0x1F)*255/31));
                f.put((char)(uint8_t)(((px>>5)&0x1F)*255/31));
                f.put((char)(uint8_t)(((px>>10)&0x1F)*255/31));
            }
    }

    // Also dump raw fifo_out_ binary for analysis
    {
        char raw_path[512];
        std::snprintf(raw_path, sizeof(raw_path), "%s_%04d.raw", dump_prefix_.c_str(), dump_count_);
        std::ofstream rf(raw_path, std::ios::binary);
        if (rf) rf.write(reinterpret_cast<const char*>(fifo_out_.data()), fifo_out_.size() * 4);
    }
    emu::logf(emu::LogLevel::warn, "MDEC",
        "frame dump #%d -> %s (%dx%d depth=%u fifo_out=%zu words)", dump_count_, path, W, H, output_depth_, fifo_out_.size());
    dump_count_++;
}

} // namespace mdec
