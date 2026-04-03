#include "xa_decoder.h"
#include <algorithm>
#include <cstring>

namespace audio
{

void XaDecoder::reset()
{
    prev_left_[0] = prev_left_[1] = 0;
    prev_right_[0] = prev_right_[1] = 0;
    sample_rate_ = 37800;
    is_stereo_ = true;
}

int XaDecoder::decode_sector(const uint8_t* sector_data, int16_t* out_left, int16_t* out_right)
{
    // XA sector layout (after subheader at sector_data[0]):
    // Bytes 0-3: Sub-header (file, channel, submode, coding)
    // Bytes 4-7: Copy of sub-header
    // Bytes 8-2311: 18 sound groups (128 bytes each) = 2304 bytes

    uint8_t coding = sector_data[3];

    // Coding info:
    // Bit 0: 0=Mono, 1=Stereo
    // Bit 2: 0=37800Hz, 1=18900Hz
    // Bit 4: 0=4-bit ADPCM, 1=8-bit ADPCM
    is_stereo_ = (coding & 0x01) != 0;
    bool half_rate = (coding & 0x04) != 0;
    sample_rate_ = half_rate ? 18900 : 37800;

    int total_samples = 0;
    const uint8_t* group_ptr = sector_data + 8;

    for (int g = 0; g < 18; g++)
    {
        int group_samples = 0;
        decode_sound_group(group_ptr, out_left + total_samples, out_right + total_samples, group_samples);
        total_samples += group_samples;
        group_ptr += 128;
    }

    return total_samples;
}

void XaDecoder::decode_sound_group(
    const uint8_t* group,
    int16_t* out_left,
    int16_t* out_right,
    int& out_count)
{
    // 128-byte sound group layout (DuckStation reference):
    //   Bytes 0-3:   Sync/reserved
    //   Bytes 4-11:  Block headers (8 bytes, one per block: shift|filter)
    //   Bytes 12-15: Reserved
    //   Bytes 16-127: 28 data words (4 bytes each = 112 bytes)
    //
    // Each 4-byte data word contains 8 nibbles (one per block):
    //   word >> (block * 4) & 0x0F
    //
    // Stereo: blocks 0,2,4,6 = left; blocks 1,3,5,7 = right
    // Mono: all 8 blocks sequential

    const int num_blocks = 8;

    // Extract parameters from headers at bytes 4-11
    uint8_t shifts[8];
    uint8_t filters[8];
    for (int b = 0; b < num_blocks; b++)
    {
        uint8_t hdr = group[4 + b];
        uint8_t s = hdr & 0x0F;
        shifts[b] = (s > 12) ? 9 : s;  // DuckStation: clamp >12 to 9
        filters[b] = (hdr >> 4) & 0x0F;
    }

    // Decode 28 samples per block from the 28 data words at bytes 16-127
    int16_t block_samples[8][28];
    const uint8_t* words_ptr = group + 16;

    for (int b = 0; b < num_blocks; b++)
    {
        const int filter = filters[b];
        const int shift = shifts[b];
        const int32_t fpos = (filter < 5) ? kPosTable[filter] : 0;
        const int32_t fneg = (filter < 5) ? kNegTable[filter] : 0;

        // Pick previous samples for this channel
        // DuckStation: prev samples are s32, indexed by channel (stereo: block&1)
        int32_t* prev = is_stereo_ ? ((b & 1) ? prev_right_ : prev_left_) : prev_left_;

        for (int w = 0; w < 28; w++)
        {
            // Each data word is 4 bytes (little-endian), nibble for block b
            uint32_t word;
            std::memcpy(&word, &words_ptr[w * 4], sizeof(word));
            const uint32_t nibble = (word >> (b * 4)) & 0x0F;

            // DuckStation formula: cast to int16 THEN arithmetic shift
            const int16_t sample = static_cast<int16_t>(static_cast<uint16_t>(nibble << 12)) >> shift;

            // Mix with previous samples (filter)
            const int32_t interp_sample = std::clamp<int32_t>(
                static_cast<int32_t>(sample) + ((prev[0] * fpos) >> 6) + ((prev[1] * fneg) >> 6),
                -32768, 32767);

            prev[1] = prev[0];
            prev[0] = interp_sample;
            block_samples[b][w] = static_cast<int16_t>(interp_sample);
        }
        // State is already saved via prev pointer (points to prev_left_ or prev_right_)
    }

    // Output: interleave blocks into L/R channels
    if (is_stereo_)
    {
        // 4 block pairs: (0,1), (2,3), (4,5), (6,7) → L,R
        for (int pair = 0; pair < 4; pair++)
        {
            for (int s = 0; s < 28; s++)
            {
                out_left[pair * 28 + s]  = block_samples[pair * 2][s];
                out_right[pair * 28 + s] = block_samples[pair * 2 + 1][s];
            }
        }
        out_count = 112;  // 4 pairs × 28 samples
    }
    else
    {
        for (int b = 0; b < 8; b++)
        {
            for (int s = 0; s < 28; s++)
            {
                out_left[b * 28 + s]  = block_samples[b][s];
                out_right[b * 28 + s] = block_samples[b][s];
            }
        }
        out_count = 224;  // 8 blocks × 28 samples
    }
}

void XaDecoder::decode_sound_unit(
    const uint8_t* /*unit*/,
    int /*filter*/,
    int /*shift*/,
    int16_t* /*out*/,
    int16_t& /*s1*/,
    int16_t& /*s2*/)
{
    // Unused — kept for API compatibility
}

} // namespace audio
