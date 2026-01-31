#include "spu_voice.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

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

// Convert a 7-bit ADSR rate to step and counter_increment.
// Based on DuckStation's VolumeEnvelope::Reset().
//
// ADSR1 layout: [15] AttackExp | [14:8] AttackRate(7b) | [7:4] DecayRate>>2(4b) | [3:0] SustainLevel(4b)
// ADSR2 layout: [15] SustainExp | [14] SustainDir | [12:6] SustainRate(7b) | [5] ReleaseExp | [4:0] ReleaseRate>>2(5b)
//
static void setup_envelope(int rate, bool decreasing, bool exponential,
                           int32_t& out_step, uint16_t& out_counter_inc)
{
    int base_step = 7 - (rate & 3);

    // Step sign: negative for decrease, positive for increase
    // DuckStation: step = decreasing ? ~base_step : base_step
    // ~base_step = -(base_step + 1) = -(8 - (rate & 3))
    if (decreasing)
        out_step = -(base_step + 1);  // e.g. base_step=7 -> step=-8
    else
        out_step = base_step;

    out_counter_inc = 0x8000;

    if (rate < 44)
    {
        // Shift step up for faster rates
        out_step <<= (11 - (rate >> 2));
    }
    else if (rate >= 48)
    {
        // Shift counter down for slower rates
        int shift = (rate >> 2) - 11;
        if (shift < 16)
            out_counter_inc >>= shift;
        else
            out_counter_inc = 0;

        // DuckStation: only clamp to 1 if rate doesn't exactly fill the mask
        // For rate 127 with 7-bit mask (0x7F): 127 & 0x7F = 0x7F, no clamp -> stays 0
        // This means rate=127 never ticks (hold forever)
    }
    // rate 44-47: step stays as-is (small), counter_inc stays 0x8000
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
    env_counter_ = 0;

    // Parse ADSR1 for attack parameters
    // ADSR1: [15]=AttackExp [14:8]=AttackRate(7b) [7:4]=DecayRate>>2 [3:0]=SustainLevel
    int attack_rate = (adsr1_ >> 8) & 0x7F;
    bool attack_exp = (adsr1_ >> 15) & 1;

    setup_envelope(attack_rate, false, attack_exp, env_step_, env_counter_inc_);
    env_exponential_ = attack_exp;
    env_decreasing_ = false;
    env_target_ = 0x7FFF;
    env_rate_ = static_cast<uint8_t>(attack_rate);
}

void SpuVoice::key_off()
{
    if (env_phase_ != ENV_OFF)
    {
        env_phase_ = ENV_RELEASE;
        env_counter_ = 0;

        // Parse ADSR2 for release parameters
        // ADSR2: [5]=ReleaseExp [4:0]=ReleaseRate>>2
        int release_rate = (adsr2_ & 0x1F) << 2;  // 5 bits, actual rate = val * 4
        bool release_exp = (adsr2_ >> 5) & 1;

        setup_envelope(release_rate, true, release_exp, env_step_, env_counter_inc_);
        env_exponential_ = release_exp;
        env_decreasing_ = true;
        env_target_ = 0;
        env_rate_ = static_cast<uint8_t>(release_rate);
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
            bool loop_start = (flags & 0x04) != 0;

            // If loop_start flag, update repeat address
            if (loop_start)
            {
                repeat_addr_ = static_cast<uint16_t>(current_addr_ >> 3);
            }

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

    // Linear interpolation between current and next sample
    int16_t s0 = decoded_[decode_idx_];
    int16_t s1 = (decode_idx_ < 27) ? decoded_[decode_idx_ + 1] : decoded_[decode_idx_];
    int32_t frac = counter_ & 0xFFF;  // 12-bit fraction (0..4095)
    int32_t sample = s0 + ((s1 - s0) * frac >> 12);

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
    // Per psx-spx: shift values 13-15 act as shift=9
    if (shift > 12) shift = 9;

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
    if (env_phase_ == ENV_OFF)
        return;

    // Counter-based timing (DuckStation approach):
    // Compute adjusted step/increment first, THEN check counter overflow.
    int32_t this_step = env_step_;
    uint32_t this_increment = env_counter_inc_;

    // Exponential mode adjustments (per DuckStation VolumeEnvelope::Tick)
    if (env_exponential_)
    {
        if (env_decreasing_)
        {
            // Exponential decrease: step is proportional to current level
            // step is already negative, multiply by level/32768
            this_step = (this_step * env_level_) >> 15;
        }
        else
        {
            // Exponential increase: slow down above 0x6000
            if (env_level_ >= 0x6000)
            {
                if (env_rate_ < 40)
                {
                    this_step >>= 2;
                }
                else if (env_rate_ >= 44)
                {
                    this_increment >>= 2;
                }
                else
                {
                    this_step >>= 1;
                    this_increment >>= 1;
                }
            }
        }
    }

    // Advance counter
    env_counter_ += static_cast<uint16_t>(this_increment);
    if (!(env_counter_ & 0x8000))
        return;  // Not time to apply step yet

    env_counter_ = 0;  // Reset counter on overflow

    // Apply step
    int32_t new_level = env_level_ + this_step;

    switch (env_phase_)
    {
        case ENV_ATTACK:
            env_level_ = std::clamp(new_level, (int32_t)0, (int32_t)0x7FFF);
            if (env_level_ >= 0x7FFF)
            {
                env_level_ = 0x7FFF;
                env_phase_ = ENV_DECAY;
                env_counter_ = 0;

                // Setup decay: always exponential decrease
                // ADSR1: [7:4] = DecayRate >> 2 (4 bits), actual rate = val * 4
                int decay_rate = ((adsr1_ >> 4) & 0x0F) << 2;
                setup_envelope(decay_rate, true, true, env_step_, env_counter_inc_);
                env_exponential_ = true;
                env_decreasing_ = true;
                env_rate_ = static_cast<uint8_t>(decay_rate);

                // Sustain level target: ADSR1 [3:0] = 4 bits
                // Per DuckStation: (sustain_level + 1) * 0x800
                int sustain_level = adsr1_ & 0x0F;
                env_target_ = std::min((sustain_level + 1) * 0x800, 0x7FFF);
            }
            break;

        case ENV_DECAY:
            env_level_ = std::max(new_level, (int32_t)0);
            if (env_level_ <= env_target_)
            {
                env_level_ = env_target_;
                env_phase_ = ENV_SUSTAIN;
                env_counter_ = 0;

                // Setup sustain from ADSR2
                // ADSR2: [15]=SustainExp [14]=SustainDir [12:6]=SustainRate(7b)
                int sustain_rate = (adsr2_ >> 6) & 0x7F;
                bool sustain_dir_decrease = (adsr2_ >> 14) & 1;
                bool sustain_exp = (adsr2_ >> 15) & 1;

                setup_envelope(sustain_rate, sustain_dir_decrease, sustain_exp,
                               env_step_, env_counter_inc_);
                env_exponential_ = sustain_exp;
                env_decreasing_ = sustain_dir_decrease;
                env_rate_ = static_cast<uint8_t>(sustain_rate);
                env_target_ = sustain_dir_decrease ? 0 : 0x7FFF;
            }
            break;

        case ENV_SUSTAIN:
            // Sustain continues indefinitely (no phase transition)
            if (env_decreasing_)
                env_level_ = std::max(new_level, (int32_t)0);
            else
                env_level_ = std::min(new_level, (int32_t)0x7FFF);
            break;

        case ENV_RELEASE:
            env_level_ = std::max(new_level, (int32_t)0);
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
}

} // namespace audio
