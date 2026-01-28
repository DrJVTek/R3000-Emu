#include "xa_decoder.h"
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
    // XA sector layout (Mode2/Form2, 2336 bytes user data):
    // Bytes 0-3: Sub-header (file, channel, submode, coding info)
    // Bytes 4-7: Copy of sub-header
    // Bytes 8-2335: 18 sound groups (128 bytes each) = 2304 bytes
    // Bytes 2336+: EDC (if present)

    // Sub-header at offset 0
    // uint8_t file = sector_data[0];
    // uint8_t channel = sector_data[1];
    // uint8_t submode = sector_data[2];
    uint8_t coding = sector_data[3];

    // Coding info:
    // Bit 0: 0=Mono, 1=Stereo
    // Bit 2: 0=37800Hz, 1=18900Hz
    // Bit 4: 0=4-bit ADPCM, 1=8-bit ADPCM (8-bit not commonly used)
    // Bit 6: Emphasis (rarely used)

    is_stereo_ = (coding & 0x01) != 0;
    bool half_rate = (coding & 0x04) != 0;
    sample_rate_ = half_rate ? 18900 : 37800;

    int total_samples = 0;

    // Process 18 sound groups starting at offset 8
    const uint8_t* group_ptr = sector_data + 8;

    for (int g = 0; g < 18; g++)
    {
        int group_samples = 0;
        decode_sound_group(
            group_ptr,
            out_left + total_samples,
            out_right + total_samples,
            group_samples
        );
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
    // XA Sound Group layout (128 bytes):
    // Bytes 0-15: Sound parameters (4 bytes per sound unit header, 4 units * 2 for L/R or 8 for mono)
    // Bytes 16-127: Sound data (8 sound units, 14 bytes each = 112 bytes)

    // For stereo: units 0,2,4,6 are left; 1,3,5,7 are right
    // For mono: all 8 units are same channel

    // Sound parameters layout (bytes 0-15):
    // Each sound unit has a parameter nibble pair at specific positions
    // The layout is a bit complex - simplified version:

    // Actually, XA groups have 8 sound units per group
    // Each sound unit: 14 bytes of ADPCM data (28 nibbles = 28 samples)
    // Parameters are in bytes 0-15 with a specific interleaving

    // Sound unit parameters (shift and filter):
    // Byte 0: Unit 0 param, Byte 1: Unit 1 param, etc. (but interleaved)

    out_count = 0;

    // Extract parameters for all 8 units
    int shifts[8];
    int filters[8];

    for (int u = 0; u < 8; u++)
    {
        // Parameters at bytes 0-3 (unit 0-3) and 4-7 (unit 4-7) repeated at 8-15
        uint8_t param = group[u % 4 + (u / 4) * 4];
        shifts[u] = param & 0x0F;
        filters[u] = (param >> 4) & 0x03;  // Only 4 filters for XA (0-3)
    }

    // Sound data starts at byte 16
    // Each unit is 14 bytes (28 nibbles = 28 samples)
    // Units are interleaved: bytes 16-29 = units 0-7 interleaved

    // Simplified: decode each unit separately
    int16_t unit_samples[8][28];

    for (int u = 0; u < 8; u++)
    {
        // Unit data is at: byte 16 + u*14 (simplified, actual XA has complex interleaving)
        // Real XA interleaving: sound bytes are packed differently

        // Correct XA interleaving:
        // Bytes 16-127 contain 112 bytes of sound data
        // Each "column" of 8 bytes contains one nibble from each of 8 units
        // There are 14 such columns (14 * 8 = 112 bytes, but only 28 nibbles per unit)

        // Let's use the correct interleaving
        int16_t* samples = unit_samples[u];
        int16_t s1 = (u & 1) ? prev_right_[0] : prev_left_[0];
        int16_t s2 = (u & 1) ? prev_right_[1] : prev_left_[1];

        int filter = filters[u];
        int shift = shifts[u];

        for (int n = 0; n < 28; n++)
        {
            // Byte index in sound data (bytes 16-127)
            // XA layout: each byte at position (16 + n*4 + u/2) contains
            // nibble for unit u at low (u even) or high (u odd) position
            int byte_idx = 16 + (n / 2) * 8 + (u / 2) + ((n & 1) ? 4 : 0);
            if (byte_idx >= 128) byte_idx = 127;  // Safety

            uint8_t data_byte = group[byte_idx];
            int nibble;
            if ((u & 1) == 0)
                nibble = data_byte & 0x0F;
            else
                nibble = (data_byte >> 4) & 0x0F;

            // Sign extend
            if (nibble >= 8)
                nibble -= 16;

            // Apply shift and filter
            int32_t sample = nibble << (12 - shift);
            if (filter < 5)
                sample += (s1 * kPosTable[filter] + s2 * kNegTable[filter] + 32) >> 6;

            // Clamp
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;

            s2 = s1;
            s1 = static_cast<int16_t>(sample);
            samples[n] = static_cast<int16_t>(sample);
        }

        // Save state
        if (u & 1)
        {
            prev_right_[0] = s1;
            prev_right_[1] = s2;
        }
        else
        {
            prev_left_[0] = s1;
            prev_left_[1] = s2;
        }
    }

    // Combine units into output
    // For stereo: even units -> left, odd units -> right
    // For mono: all units -> both channels

    if (is_stereo_)
    {
        // 4 units per channel, 28 samples each = 112 samples per channel
        // Units 0,2,4,6 -> left; Units 1,3,5,7 -> right
        for (int u = 0; u < 4; u++)
        {
            for (int s = 0; s < 28; s++)
            {
                out_left[u * 28 + s] = unit_samples[u * 2][s];
                out_right[u * 28 + s] = unit_samples[u * 2 + 1][s];
            }
        }
        out_count = 112;  // 4 units * 28 samples
    }
    else
    {
        // Mono: 8 units, all same channel
        for (int u = 0; u < 8; u++)
        {
            for (int s = 0; s < 28; s++)
            {
                out_left[u * 28 + s] = unit_samples[u][s];
                out_right[u * 28 + s] = unit_samples[u][s];  // Copy to both channels
            }
        }
        out_count = 224;  // 8 units * 28 samples
    }
}

void XaDecoder::decode_sound_unit(
    const uint8_t* unit,
    int filter,
    int shift,
    int16_t* out,
    int16_t& s1,
    int16_t& s2)
{
    // This is used for simple decoding, actual XA uses interleaved layout
    // Kept for reference

    for (int i = 0; i < 28; i++)
    {
        int byte_idx = i / 2;
        int nibble;
        if ((i & 1) == 0)
            nibble = unit[byte_idx] & 0x0F;
        else
            nibble = (unit[byte_idx] >> 4) & 0x0F;

        if (nibble >= 8)
            nibble -= 16;

        int32_t sample = nibble << (12 - shift);
        if (filter < 5)
            sample += (s1 * kPosTable[filter] + s2 * kNegTable[filter] + 32) >> 6;

        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;

        s2 = s1;
        s1 = static_cast<int16_t>(sample);
        out[i] = static_cast<int16_t>(sample);
    }
}

} // namespace audio
