#pragma once

#include <cstdint>
#include <functional>
#include "spu_voice.h"

namespace cdrom { class Cdrom; }

namespace audio
{

// Forward declaration
class WavWriter;

// Audio output callback type: receives interleaved stereo samples
using AudioCallback = std::function<void(const int16_t* samples, int count)>;

// PS1 Sound Processing Unit
// - 24 hardware voices with ADPCM decoding
// - 512 KB SPU RAM
// - CD-XA audio mixing
// - Reverb (stub for now)
class Spu
{
  public:
    static constexpr uint32_t kRamSize = 512 * 1024;  // 512 KB
    static constexpr int kNumVoices = 24;
    static constexpr int kSampleRate = 44100;

    Spu();
    ~Spu();

    // Register access (offset from 0x1F801C00)
    void write_reg(uint32_t offset, uint16_t val);
    uint16_t read_reg(uint32_t offset) const;

    // SPU RAM access (for DMA transfers)
    void write_ram(uint32_t addr, uint16_t val);
    uint16_t read_ram(uint32_t addr) const;
    uint8_t* ram() { return ram_; }
    const uint8_t* ram() const { return ram_; }

    // DMA transfer support
    void dma_write(const uint16_t* data, uint32_t count);
    void dma_read(uint16_t* data, uint32_t count);
    uint32_t transfer_addr() const { return xfer_addr_cur_; }
    void set_transfer_addr(uint32_t addr) { xfer_addr_cur_ = addr & (kRamSize - 1); }

    // Generate audio samples (called at 44100 Hz rate)
    // out_l, out_r receive one sample each
    void tick(int16_t* out_l, int16_t* out_r);

    // Mix XA-ADPCM samples from CDROM
    void push_xa_samples(const int16_t* left, const int16_t* right, int count);

    // Set WAV writer for debug output (optional)
    void set_wav_writer(WavWriter* writer) { wav_writer_ = writer; }

    // Set audio callback for streaming (e.g., to UE5)
    void set_audio_callback(AudioCallback cb) { audio_callback_ = std::move(cb); }

    // Set CDROM for CDDA audio integration
    void set_cdrom(cdrom::Cdrom* cd) { cdrom_ = cd; }

    // SPUSTAT register value
    uint16_t stat() const;

    // IRQ callback (called when SPU triggers interrupt)
    using IrqCallback = std::function<void()>;
    void set_irq_callback(IrqCallback cb) { irq_callback_ = std::move(cb); }

    // Check and clear IRQ flag (for reading SPUSTAT)
    bool irq_pending() const { return irq_flag_; }
    void clear_irq_flag() { irq_flag_ = false; }

    // Tick called from bus (cycle-based)
    void tick_cycles(uint32_t cycles);

    // Flush any buffered audio samples to the callback.
    // Call at end of each frame to avoid latency at high framerates.
    void flush_audio();

  private:
    // Voice registers: 0x1F801C00 + voice*0x10 (voices 0-23)
    // Global registers: 0x1F801D80+
    void write_voice_reg(int voice, uint32_t offset, uint16_t val);
    uint16_t read_voice_reg(int voice, uint32_t offset) const;

    // SPU RAM
    uint8_t ram_[kRamSize]{};

    // 24 voices
    SpuVoice voices_[kNumVoices];

    // Global volume registers
    int16_t main_vol_l_{0};
    int16_t main_vol_r_{0};
    int16_t reverb_vol_l_{0};
    int16_t reverb_vol_r_{0};

    // CD audio volume
    int16_t cd_vol_l_{0x7FFF};
    int16_t cd_vol_r_{0x7FFF};

    // External audio volume
    int16_t ext_vol_l_{0};
    int16_t ext_vol_r_{0};

    // Key on/off latches (bits 0-23 = voices 0-23)
    uint32_t kon_{0};
    uint32_t koff_{0};

    // Voice status
    uint32_t endx_{0};  // ENDX: voices that reached end

    // Channel enable flags
    uint32_t pmon_{0};   // Pitch modulation
    uint32_t non_{0};    // Noise mode
    uint32_t eon_{0};    // Reverb enable
    uint32_t kon_shadow_{0};  // Key on shadow for reading

    // Control register (SPUCNT)
    uint16_t ctrl_{0};

    // Transfer registers
    uint16_t xfer_addr_reg_{0};  // In 8-byte units
    uint32_t xfer_addr_cur_{0};  // Current byte address
    uint16_t xfer_ctrl_{0};

    // IRQ registers
    uint16_t irq_addr_{0};       // IRQ trigger address (8-byte units)
    bool irq_flag_{false};       // IRQ pending flag (SPUSTAT bit 6)
    IrqCallback irq_callback_{}; // Called when IRQ triggers

    // FIFO for manual transfers
    uint16_t fifo_[32]{};
    int fifo_count_{0};

    // Reverb registers (stub)
    uint16_t reverb_base_{0};
    uint16_t reverb_regs_[32]{};

    // XA audio ring buffer
    static constexpr int kXaBufferSize = 8192;
    int16_t xa_buffer_l_[kXaBufferSize]{};
    int16_t xa_buffer_r_[kXaBufferSize]{};
    int xa_read_pos_{0};
    int xa_write_pos_{0};
    int xa_samples_available_{0};

    // Cycle accumulator for sample generation
    uint32_t cycle_accum_{0};
    static constexpr uint32_t kCyclesPerSample = 768;  // ~33.8 MHz / 44100 Hz

    // Audio output
    WavWriter* wav_writer_{nullptr};
    AudioCallback audio_callback_{};

    // Output buffer for callback
    int16_t output_buffer_[2048]{};
    int output_buffer_pos_{0};

    // Debug: total samples generated
    uint64_t total_samples_{0};
    uint64_t callback_invocations_{0};

    // CDROM for CDDA audio
    cdrom::Cdrom* cdrom_{nullptr};
};

} // namespace audio
