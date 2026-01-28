#include "wav_writer.h"
#include <cstring>

namespace audio
{

WavWriter::~WavWriter()
{
    close();
}

bool WavWriter::open(const char* path, int sample_rate, int channels)
{
    close();  // Close any previously open file

    file_ = std::fopen(path, "wb");
    if (!file_)
        return false;

    sample_rate_ = sample_rate;
    channels_ = channels;
    samples_written_ = 0;

    write_header();
    return true;
}

void WavWriter::write_header()
{
    // WAV header (44 bytes)
    // We'll write placeholder sizes and update them on close()

    // RIFF chunk
    std::fwrite("RIFF", 1, 4, file_);
    uint32_t riff_size = 0;  // Placeholder, will be updated
    std::fwrite(&riff_size, 4, 1, file_);
    std::fwrite("WAVE", 1, 4, file_);

    // fmt sub-chunk
    std::fwrite("fmt ", 1, 4, file_);
    uint32_t fmt_size = 16;  // PCM format
    std::fwrite(&fmt_size, 4, 1, file_);

    uint16_t audio_format = 1;  // PCM
    std::fwrite(&audio_format, 2, 1, file_);

    uint16_t num_channels = static_cast<uint16_t>(channels_);
    std::fwrite(&num_channels, 2, 1, file_);

    uint32_t sample_rate = static_cast<uint32_t>(sample_rate_);
    std::fwrite(&sample_rate, 4, 1, file_);

    uint32_t byte_rate = sample_rate * channels_ * 2;  // 16-bit = 2 bytes
    std::fwrite(&byte_rate, 4, 1, file_);

    uint16_t block_align = static_cast<uint16_t>(channels_ * 2);
    std::fwrite(&block_align, 2, 1, file_);

    uint16_t bits_per_sample = 16;
    std::fwrite(&bits_per_sample, 2, 1, file_);

    // data sub-chunk
    std::fwrite("data", 1, 4, file_);
    uint32_t data_size = 0;  // Placeholder
    std::fwrite(&data_size, 4, 1, file_);

    data_start_pos_ = 44;  // Data starts after header
}

void WavWriter::write_samples(const int16_t* samples, int sample_count)
{
    if (!file_)
        return;

    // Write samples (already in correct format: 16-bit little-endian)
    std::fwrite(samples, sizeof(int16_t), sample_count * channels_, file_);
    samples_written_ += sample_count;
}

void WavWriter::write_sample(int16_t left, int16_t right)
{
    if (!file_)
        return;

    int16_t samples[2] = {left, right};
    std::fwrite(samples, sizeof(int16_t), 2, file_);
    samples_written_++;
}

void WavWriter::close()
{
    if (!file_)
        return;

    finalize_header();
    std::fclose(file_);
    file_ = nullptr;
}

void WavWriter::finalize_header()
{
    if (!file_)
        return;

    // Calculate final sizes
    uint32_t data_size = samples_written_ * channels_ * 2;  // 16-bit = 2 bytes
    uint32_t riff_size = data_size + 36;  // data + header - 8 (RIFF + size)

    // Update RIFF size (at offset 4)
    std::fseek(file_, 4, SEEK_SET);
    std::fwrite(&riff_size, 4, 1, file_);

    // Update data size (at offset 40)
    std::fseek(file_, 40, SEEK_SET);
    std::fwrite(&data_size, 4, 1, file_);

    // Seek back to end
    std::fseek(file_, 0, SEEK_END);
}

} // namespace audio
