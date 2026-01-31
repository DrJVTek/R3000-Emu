#include "spu.h"
#include "wav_writer.h"
#include <algorithm>
#include <cstdio>
#include <cstring>

namespace audio
{

Spu::Spu()
{
    std::memset(ram_, 0, kRamSize);
}

Spu::~Spu() = default;

void Spu::write_reg(uint32_t offset, uint16_t val)
{
    // Voice registers: 0x000-0x17F (voices 0-23, 0x10 bytes each)
    if (offset < 0x180)
    {
        int voice = offset / 0x10;
        int reg_off = offset & 0x0F;
        write_voice_reg(voice, reg_off, val);
        return;
    }

    // Global registers: 0x180+
    switch (offset)
    {
        case 0x180: main_vol_l_ = static_cast<int16_t>(val); break;
        case 0x182: main_vol_r_ = static_cast<int16_t>(val); break;
        case 0x184: reverb_vol_l_ = static_cast<int16_t>(val); break;
        case 0x186: reverb_vol_r_ = static_cast<int16_t>(val); break;

        // Key On (KON) - low 16 bits
        case 0x188:
            kon_ = (kon_ & 0xFFFF0000) | val;
            break;
        // Key On - high 8 bits
        case 0x18A:
            kon_ = (kon_ & 0x0000FFFF) | (static_cast<uint32_t>(val & 0xFF) << 16);
            // Process key on
            for (int i = 0; i < kNumVoices; i++)
            {
                if (kon_ & (1u << i))
                {
                    voices_[i].key_on();
                    endx_ &= ~(1u << i);  // Clear ENDX flag
                }
            }
            kon_shadow_ = kon_;
            kon_ = 0;
            break;

        // Key Off (KOFF) - low 16 bits
        case 0x18C:
            koff_ = (koff_ & 0xFFFF0000) | val;
            break;
        // Key Off - high 8 bits
        case 0x18E:
            koff_ = (koff_ & 0x0000FFFF) | (static_cast<uint32_t>(val & 0xFF) << 16);
            // Process key off
            for (int i = 0; i < kNumVoices; i++)
            {
                if (koff_ & (1u << i))
                {
                    voices_[i].key_off();
                }
            }
            koff_ = 0;
            break;

        // PMON - Pitch modulation enable
        case 0x190: pmon_ = (pmon_ & 0xFFFF0000) | val; break;
        case 0x192: pmon_ = (pmon_ & 0x0000FFFF) | (static_cast<uint32_t>(val & 0xFF) << 16); break;

        // NON - Noise mode enable
        case 0x194: non_ = (non_ & 0xFFFF0000) | val; break;
        case 0x196: non_ = (non_ & 0x0000FFFF) | (static_cast<uint32_t>(val & 0xFF) << 16); break;

        // EON - Reverb enable
        case 0x198: eon_ = (eon_ & 0xFFFF0000) | val; break;
        case 0x19A: eon_ = (eon_ & 0x0000FFFF) | (static_cast<uint32_t>(val & 0xFF) << 16); break;

        // ENDX - clear on write
        case 0x19C:
        case 0x19E:
            endx_ = 0;
            break;

        // Reverb work area start
        case 0x1A2: reverb_base_ = val; break;

        // IRQ address
        case 0x1A4: /* irq_addr */ break;

        // Transfer address
        case 0x1A6:
            xfer_addr_reg_ = val;
            xfer_addr_cur_ = static_cast<uint32_t>(val) << 3;  // 8-byte units -> bytes
            break;

        // Transfer FIFO (manual write to SPU RAM)
        case 0x1A8:
            if (xfer_addr_cur_ < kRamSize - 1)
            {
                ram_[xfer_addr_cur_] = val & 0xFF;
                ram_[xfer_addr_cur_ + 1] = (val >> 8) & 0xFF;
                xfer_addr_cur_ += 2;
                xfer_addr_cur_ &= (kRamSize - 1);
            }
            break;

        // SPUCNT - Control register
        case 0x1AA:
        {
            ctrl_ = val;
            // Bit 15: SPU enable
            // Bit 14: Mute
            // Bit 13: Noise frequency shift
            // Bit 12-8: Noise frequency step
            // Bit 5: Reverb master enable
            // Bit 4: IRQ enable
            // Bit 3-1: Transfer mode
            // Bit 0: CD audio enable
            break;
        }

        // Transfer control
        case 0x1AC:
            xfer_ctrl_ = val;
            break;

        // CD volume
        case 0x1B0: cd_vol_l_ = static_cast<int16_t>(val); break;
        case 0x1B2: cd_vol_r_ = static_cast<int16_t>(val); break;

        // External audio volume
        case 0x1B4: ext_vol_l_ = static_cast<int16_t>(val); break;
        case 0x1B6: ext_vol_r_ = static_cast<int16_t>(val); break;

        // Current main volume (read-only in theory, some games write)
        case 0x1B8:
        case 0x1BA:
            break;

        // Reverb registers (0x1C0-0x1FF)
        default:
            if (offset >= 0x1C0 && offset < 0x200)
            {
                int rev_idx = (offset - 0x1C0) / 2;
                if (rev_idx < 32)
                    reverb_regs_[rev_idx] = val;
            }
            break;
    }
}

uint16_t Spu::read_reg(uint32_t offset) const
{
    // Voice registers
    if (offset < 0x180)
    {
        int voice = offset / 0x10;
        int reg_off = offset & 0x0F;
        return read_voice_reg(voice, reg_off);
    }

    // Global registers
    switch (offset)
    {
        case 0x180: return static_cast<uint16_t>(main_vol_l_);
        case 0x182: return static_cast<uint16_t>(main_vol_r_);
        case 0x184: return static_cast<uint16_t>(reverb_vol_l_);
        case 0x186: return static_cast<uint16_t>(reverb_vol_r_);

        case 0x188: return static_cast<uint16_t>(kon_shadow_);
        case 0x18A: return static_cast<uint16_t>(kon_shadow_ >> 16);
        case 0x18C: return 0;  // KOFF not readable
        case 0x18E: return 0;

        case 0x190: return static_cast<uint16_t>(pmon_);
        case 0x192: return static_cast<uint16_t>(pmon_ >> 16);
        case 0x194: return static_cast<uint16_t>(non_);
        case 0x196: return static_cast<uint16_t>(non_ >> 16);
        case 0x198: return static_cast<uint16_t>(eon_);
        case 0x19A: return static_cast<uint16_t>(eon_ >> 16);

        // ENDX - voices that reached end
        case 0x19C: return static_cast<uint16_t>(endx_);
        case 0x19E: return static_cast<uint16_t>(endx_ >> 16);

        case 0x1A2: return reverb_base_;
        case 0x1A6: return xfer_addr_reg_;
        case 0x1A8: return 0;  // FIFO read (not commonly used)
        case 0x1AA: return ctrl_;
        case 0x1AC: return xfer_ctrl_;
        case 0x1AE: return stat();

        case 0x1B0: return static_cast<uint16_t>(cd_vol_l_);
        case 0x1B2: return static_cast<uint16_t>(cd_vol_r_);
        case 0x1B4: return static_cast<uint16_t>(ext_vol_l_);
        case 0x1B6: return static_cast<uint16_t>(ext_vol_r_);

        // Current main volume
        case 0x1B8: return static_cast<uint16_t>(main_vol_l_);
        case 0x1BA: return static_cast<uint16_t>(main_vol_r_);

        default:
            if (offset >= 0x1C0 && offset < 0x200)
            {
                int rev_idx = (offset - 0x1C0) / 2;
                if (rev_idx < 32)
                    return reverb_regs_[rev_idx];
            }
            return 0;
    }
}

void Spu::write_voice_reg(int voice, uint32_t offset, uint16_t val)
{
    if (voice < kNumVoices)
        voices_[voice].write_reg(offset, val);
}

uint16_t Spu::read_voice_reg(int voice, uint32_t offset) const
{
    if (voice < kNumVoices)
        return voices_[voice].read_reg(offset);
    return 0;
}

uint16_t Spu::stat() const
{
    // SPUSTAT:
    // Bit 15-12: Current mode (from SPUCNT bits 5-0)
    // Bit 11: Write to second half of capture buffers
    // Bit 10: Data transfer busy
    // Bit 9: DMA read request
    // Bit 8: DMA write request
    // Bit 7: DMA ready (r/w)
    // Bit 6: IRQ flag
    // Bit 5-0: Current mode (mirror of SPUCNT bits 5-0)

    uint16_t s = ctrl_ & 0x3F;  // Current mode from ctrl
    s |= (ctrl_ & 0x3F) << 10;  // Upper bits

    // DMA ready flag (bit 7)
    s |= 0x80;

    return s;
}

void Spu::write_ram(uint32_t addr, uint16_t val)
{
    addr &= (kRamSize - 1);
    if (addr < kRamSize - 1)
    {
        ram_[addr] = val & 0xFF;
        ram_[addr + 1] = (val >> 8) & 0xFF;
    }
}

uint16_t Spu::read_ram(uint32_t addr) const
{
    addr &= (kRamSize - 1);
    if (addr < kRamSize - 1)
        return ram_[addr] | (ram_[addr + 1] << 8);
    return 0;
}

void Spu::dma_write(const uint16_t* data, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        write_ram(xfer_addr_cur_, data[i]);
        xfer_addr_cur_ += 2;
        xfer_addr_cur_ &= (kRamSize - 1);
    }
}

void Spu::dma_read(uint16_t* data, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++)
    {
        data[i] = read_ram(xfer_addr_cur_);
        xfer_addr_cur_ += 2;
        xfer_addr_cur_ &= (kRamSize - 1);
    }
}

void Spu::tick(int16_t* out_l, int16_t* out_r)
{
    total_samples_++;
    int32_t mix_l = 0;
    int32_t mix_r = 0;

    // Check if SPU is enabled (SPUCNT bit 15)
    bool spu_enabled = (ctrl_ & 0x8000) != 0;

    if (spu_enabled)
    {
        // Mix all active voices
        for (int i = 0; i < kNumVoices; i++)
        {
            int16_t sample = voices_[i].tick(ram_, kRamSize - 1);

            // Check for loop end
            if (voices_[i].hit_loop_end())
            {
                endx_ |= (1u << i);
                voices_[i].clear_loop_end();
            }

            // Apply voice volume (signed 15-bit)
            int16_t vol_l = static_cast<int16_t>(voices_[i].read_reg(0x00));
            int16_t vol_r = static_cast<int16_t>(voices_[i].read_reg(0x02));
            int32_t sample_l = (sample * vol_l) >> 15;
            int32_t sample_r = (sample * vol_r) >> 15;

            mix_l += sample_l;
            mix_r += sample_r;
        }

        // Mix XA audio if available
        if (xa_samples_available_ > 0 && (ctrl_ & 0x01))  // CD audio enable
        {
            int16_t xa_l = xa_buffer_l_[xa_read_pos_];
            int16_t xa_r = xa_buffer_r_[xa_read_pos_];

            // Apply CD volume
            int32_t cd_l = (xa_l * cd_vol_l_) >> 15;
            int32_t cd_r = (xa_r * cd_vol_r_) >> 15;

            mix_l += cd_l;
            mix_r += cd_r;

            xa_read_pos_ = (xa_read_pos_ + 1) % kXaBufferSize;
            xa_samples_available_--;
        }

        // Apply main volume
        mix_l = (mix_l * main_vol_l_) >> 15;
        mix_r = (mix_r * main_vol_r_) >> 15;
    }

    // Clamp to 16-bit
    if (mix_l > 32767) mix_l = 32767;
    if (mix_l < -32768) mix_l = -32768;
    if (mix_r > 32767) mix_r = 32767;
    if (mix_r < -32768) mix_r = -32768;

    *out_l = static_cast<int16_t>(mix_l);
    *out_r = static_cast<int16_t>(mix_r);

    // Write to WAV if enabled
    if (wav_writer_)
    {
        int16_t stereo[2] = {*out_l, *out_r};
        // Will be written via external call
    }

    // Buffer for callback
    if (audio_callback_)
    {
        output_buffer_[output_buffer_pos_++] = *out_l;
        output_buffer_[output_buffer_pos_++] = *out_r;

        // Flush buffer when full
        if (output_buffer_pos_ >= 2048)
        {
            audio_callback_(output_buffer_, output_buffer_pos_ / 2);
            output_buffer_pos_ = 0;
        }
    }
}

void Spu::tick_cycles(uint32_t cycles)
{
    cycle_accum_ += cycles;

    while (cycle_accum_ >= kCyclesPerSample)
    {
        cycle_accum_ -= kCyclesPerSample;

        int16_t l, r;
        tick(&l, &r);

        // Write to WAV writer if present
        if (wav_writer_)
        {
            wav_writer_->write_sample(l, r);
        }
    }
}

void Spu::push_xa_samples(const int16_t* left, const int16_t* right, int count)
{
    for (int i = 0; i < count; i++)
    {
        if (xa_samples_available_ < kXaBufferSize)
        {
            xa_buffer_l_[xa_write_pos_] = left[i];
            xa_buffer_r_[xa_write_pos_] = right[i];
            xa_write_pos_ = (xa_write_pos_ + 1) % kXaBufferSize;
            xa_samples_available_++;
        }
    }
}

} // namespace audio
