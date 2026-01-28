#pragma once

#include <cstdint>

namespace audio
{

// XA-ADPCM Decoder for CD-ROM audio streaming
// Decodes Mode2/Form2 XA sectors into PCM samples
class XaDecoder
{
  public:
    XaDecoder() = default;

    // Reset decoder state
    void reset();

    // Set filter parameters (from CDROM SetFilter command)
    void set_filter(uint8_t file, uint8_t channel)
    {
        filter_file_ = file;
        filter_channel_ = channel;
    }

    // Check if sector matches filter
    bool matches_filter(uint8_t file, uint8_t channel) const
    {
        return (filter_file_ == file) && (filter_channel_ == channel);
    }

    // Decode XA sector (2336 bytes of Mode2/Form2 user data after sync+header)
    // Returns number of samples decoded per channel (usually 4032 for stereo, 8064 for mono)
    // out_left and out_right must have space for at least 4032 samples each
    int decode_sector(const uint8_t* sector_data, int16_t* out_left, int16_t* out_right);

    // Get last decoded sample rate (37800 Hz or 18900 Hz)
    int sample_rate() const { return sample_rate_; }

    // Get if last sector was stereo
    bool is_stereo() const { return is_stereo_; }

  private:
    // Decode a single XA sound group (128 bytes -> 224 samples per channel)
    void decode_sound_group(
        const uint8_t* group,
        int16_t* out_left,
        int16_t* out_right,
        int& out_count
    );

    // Decode 28 nibbles from a sound unit
    void decode_sound_unit(
        const uint8_t* unit,
        int filter,
        int shift,
        int16_t* out,
        int16_t& s1,
        int16_t& s2
    );

    // Filter parameters
    uint8_t filter_file_{0};
    uint8_t filter_channel_{0};

    // ADPCM decode state (separate for left/right)
    int16_t prev_left_[2]{0, 0};
    int16_t prev_right_[2]{0, 0};

    // Last sector info
    int sample_rate_{37800};
    bool is_stereo_{true};

    // XA ADPCM filter tables (same as SPU)
    static constexpr int16_t kPosTable[5] = {0, 60, 115, 98, 122};
    static constexpr int16_t kNegTable[5] = {0, 0, -52, -55, -60};
};

} // namespace audio
