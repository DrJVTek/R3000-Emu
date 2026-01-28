#include "spu_voice.h"
#include <algorithm>
#include <cstdio>

namespace audio
{

void SpuVoice::write_reg(uint32_t offset, uint16_t val)
{
    switch (offset)
    {
        case 0x00: vol_l_ = val; break;
        case 0x02: vol_r_ = val; break;
        case 0x04: pitch_ = val; break;
        case 0x06: start_addr_ = val; break;
        case 0x08: adsr1_ = val; break;
        case 0x0A: adsr2_ = val; break;
        case 0x0C: /* adsr_vol_ read-only */ break;
        case 0x0E: repeat_addr_ = val; break;
        default: break;
    }
}

uint16_t SpuVoice::read_reg(uint32_t offset) const
{
    switch (offset)
    {
        case 0x00: return vol_l_;
        case 0x02: return vol_r_;
        case 0x04: return pitch_;
        case 0x06: return start_addr_;
        case 0x08: return adsr1_;
        case 0x0A: return adsr2_;
        case 0x0C: return static_cast<uint16_t>(env_level_);
        case 0x0E: return repeat_addr_;
        default: return 0;
    }
}

void SpuVoice::key_on()
{
    // Reset to start of sample
    current_addr_ = static_cast<uint32_t>(start_addr_) << 3;  // 8-byte units -> bytes
    counter_ = 0;
    decode_idx_ = 28;  // Force decode on first tick
    prev_samples_[0] = 0;
    prev_samples_[1] = 0;
    hit_loop_end_ = false;

    // Start attack phase
    env_phase_ = ENV_ATTACK;
    env_level_ = 0;

    // Parse ADSR1 for attack parameters
    // ADSR1: [15:10]=Sustain Level, [9:8]=Decay Shift, [7]=Attack Mode, [6:2]=Attack Shift, [1:0]=Attack Step
    int attack_shift = (adsr1_ >> 2) & 0x1F;
    int attack_step = adsr1_ & 0x03;
    int attack_mode = (adsr1_ >> 7) & 1;

    if (attack_mode == 0)
    {
        // Linear attack
        env_step_ = (7 - attack_step) << (11 - attack_shift);
        if (env_step_ <= 0) env_step_ = 1;
    }
    else
    {
        // Exponential attack (simplified as linear for now)
        env_step_ = (7 - attack_step) << (11 - attack_shift);
        if (env_step_ <= 0) env_step_ = 1;
    }
    env_target_ = 0x7FFF;
}

void SpuVoice::key_off()
{
    if (env_phase_ != ENV_OFF)
    {
        env_phase_ = ENV_RELEASE;

        // Parse ADSR2 for release parameters
        // ADSR2: [15]=Release Mode, [14:9]=Release Shift, [8:6]=Sustain Step, [5]=Sustain Mode, [4:0]=Sustain Shift
        int release_shift = (adsr2_ >> 9) & 0x1F;
        int release_mode = (adsr2_ >> 15) & 1;

        if (release_mode == 0)
        {
            // Linear release
            env_step_ = -((env_level_ >> release_shift) + 1);
            if (env_step_ >= 0) env_step_ = -1;
        }
        else
        {
            // Exponential release
            env_step_ = -((env_level_ >> (release_shift + 2)) + 1);
            if (env_step_ >= 0) env_step_ = -1;
        }
        env_target_ = 0;
    }
}

int16_t SpuVoice::tick(const uint8_t* spu_ram, uint32_t ram_mask)
{
    if (env_phase_ == ENV_OFF)
        return 0;

    // Advance pitch counter
    counter_ += pitch_;

    // Check if we need to advance sample position
    while (counter_ >= 0x1000)
    {
        counter_ -= 0x1000;
        decode_idx_++;

        // Need new ADPCM block?
        if (decode_idx_ >= 28)
        {
            decode_idx_ = 0;

            // Read and decode ADPCM block (16 bytes -> 28 samples)
            const uint8_t* block = spu_ram + (current_addr_ & ram_mask);
            decode_block(block);

            // Check flags byte (byte 1)
            uint8_t flags = block[1];
            bool loop_end = (flags & 0x01) != 0;
            bool loop_repeat = (flags & 0x02) != 0;

            // Advance to next block
            current_addr_ += 16;
            current_addr_ &= ram_mask;

            if (loop_end)
            {
                hit_loop_end_ = true;
                if (loop_repeat)
                {
                    // Jump to loop address
                    current_addr_ = static_cast<uint32_t>(repeat_addr_) << 3;
                }
                else
                {
                    // Stop voice
                    env_phase_ = ENV_OFF;
                    env_level_ = 0;
                    return 0;
                }
            }
        }
    }

    // Get current sample (nearest neighbor - could upgrade to Gaussian)
    int32_t sample = decoded_[decode_idx_];

    // Apply envelope
    tick_envelope();
    sample = (sample * env_level_) >> 15;

    // Clamp
    if (sample > 32767) sample = 32767;
    if (sample < -32768) sample = -32768;

    adsr_vol_ = static_cast<uint16_t>(env_level_);
    return static_cast<int16_t>(sample);
}

void SpuVoice::decode_block(const uint8_t* block)
{
    // PS1 ADPCM: 16 bytes -> 28 samples
    // Byte 0: shift (bits 0-3) | filter (bits 4-6)
    // Byte 1: flags (bit 0=loop end, bit 1=loop repeat, bit 2=loop start)
    // Bytes 2-15: 4-bit samples (2 per byte, low nibble first)

    int shift = block[0] & 0x0F;
    int filter = (block[0] >> 4) & 0x07;
    if (filter > 4) filter = 4;  // Only 5 filters defined

    int16_t s1 = prev_samples_[0];
    int16_t s2 = prev_samples_[1];

    for (int i = 0; i < 28; i++)
    {
        // Get 4-bit nibble
        int byte_idx = 2 + (i / 2);
        int nibble;
        if ((i & 1) == 0)
            nibble = block[byte_idx] & 0x0F;
        else
            nibble = (block[byte_idx] >> 4) & 0x0F;

        // Sign extend from 4 bits
        if (nibble >= 8)
            nibble -= 16;

        // Apply shift and filter
        int32_t sample = nibble << (12 - shift);
        sample += (s1 * kPosTable[filter] + s2 * kNegTable[filter] + 32) >> 6;

        // Clamp to 16-bit
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        // Update history
        s2 = s1;
        s1 = static_cast<int16_t>(sample);

        decoded_[i] = static_cast<int16_t>(sample);
    }

    prev_samples_[0] = s1;
    prev_samples_[1] = s2;
}

void SpuVoice::tick_envelope()
{
    switch (env_phase_)
    {
        case ENV_ATTACK:
            env_level_ += env_step_;
            if (env_level_ >= 0x7FFF)
            {
                env_level_ = 0x7FFF;
                env_phase_ = ENV_DECAY;

                // Setup decay
                int decay_shift = (adsr1_ >> 8) & 0x0F;
                env_step_ = -((env_level_ >> decay_shift) + 1);
                if (env_step_ >= 0) env_step_ = -1;

                // Sustain level target
                int sustain_level = (adsr1_ >> 10) & 0x3F;
                env_target_ = (sustain_level + 1) << 9;  // 0..0x7FFF
            }
            break;

        case ENV_DECAY:
            env_level_ += env_step_;
            if (env_level_ <= env_target_)
            {
                env_level_ = env_target_;
                env_phase_ = ENV_SUSTAIN;

                // Setup sustain
                int sustain_shift = adsr2_ & 0x1F;
                int sustain_step = (adsr2_ >> 6) & 0x03;
                int sustain_mode = (adsr2_ >> 5) & 1;
                int sustain_dir = (adsr2_ >> 14) & 1;

                if (sustain_dir == 0)
                {
                    // Increase
                    env_step_ = (7 - sustain_step) << (11 - sustain_shift);
                    if (env_step_ <= 0) env_step_ = 1;
                    env_target_ = 0x7FFF;
                }
                else
                {
                    // Decrease
                    if (sustain_mode == 0)
                        env_step_ = -((7 - sustain_step) << (11 - sustain_shift));
                    else
                        env_step_ = -((env_level_ >> sustain_shift) + 1);
                    if (env_step_ >= 0) env_step_ = -1;
                    env_target_ = 0;
                }
            }
            break;

        case ENV_SUSTAIN:
            env_level_ += env_step_;
            if (env_step_ > 0 && env_level_ >= env_target_)
                env_level_ = env_target_;
            else if (env_step_ < 0 && env_level_ <= env_target_)
                env_level_ = env_target_;
            break;

        case ENV_RELEASE:
            env_level_ += env_step_;
            if (env_level_ <= 0)
            {
                env_level_ = 0;
                env_phase_ = ENV_OFF;
            }
            break;

        case ENV_OFF:
        default:
            break;
    }

    // Clamp envelope
    if (env_level_ < 0) env_level_ = 0;
    if (env_level_ > 0x7FFF) env_level_ = 0x7FFF;
}

} // namespace audio
