#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace audio
{

// Simple WAV file writer for debugging audio output
// Creates 16-bit stereo PCM WAV files
class WavWriter
{
  public:
    WavWriter() = default;
    ~WavWriter();

    // Open WAV file for writing
    // Returns true on success
    bool open(const char* path, int sample_rate = 44100, int channels = 2);

    // Write interleaved stereo samples (left, right, left, right, ...)
    void write_samples(const int16_t* samples, int sample_count);

    // Write a single stereo sample pair
    void write_sample(int16_t left, int16_t right);

    // Close file and finalize header
    void close();

    // Check if file is open
    bool is_open() const { return file_ != nullptr; }

    // Get total samples written
    uint32_t samples_written() const { return samples_written_; }

  private:
    void write_header();
    void finalize_header();

    FILE* file_{nullptr};
    int sample_rate_{44100};
    int channels_{2};
    uint32_t samples_written_{0};
    uint32_t data_start_pos_{0};
};

} // namespace audio
