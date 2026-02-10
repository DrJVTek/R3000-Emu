#pragma once

#include <cstdint>

namespace audio
{

// PS1 SPU Voice - handles ADPCM decoding, pitch, and ADSR envelope
class SpuVoice
{
  public:
    SpuVoice() = default;

    // Register writes (offset within voice: 0x00-0x0E)
    void write_reg(uint32_t offset, uint16_t val);
    uint16_t read_reg(uint32_t offset) const;

    // Key on/off triggers
    void key_on();
    void key_off();

    // Force immediate silence (used when SPU is disabled)
    // Unlike key_off() which starts release phase, this immediately stops the voice.
    void force_off();

    // Generate one sample at 44100 Hz
    // Returns sample with envelope applied (-32768..32767)
    int16_t tick(const uint8_t* spu_ram, uint32_t ram_mask);

    // Check if voice is active (producing sound)
    bool is_active() const { return env_phase_ != ENV_OFF; }

    // Get current envelope level (for ENDX detection)
    int32_t envelope_level() const { return env_level_; }

    // Check if voice hit loop end (for KON/ENDX flags)
    bool hit_loop_end() const { return hit_loop_end_; }
    void clear_loop_end() { hit_loop_end_ = false; }

    // Debug: get envelope phase
    int env_phase() const { return static_cast<int>(env_phase_); }

    // Get current SPU RAM address (for IRQ checking)
    uint32_t current_addr() const { return current_addr_; }

  private:
    // ADPCM block decoding
    void decode_block(const uint8_t* block);

    // ADSR envelope tick
    void tick_envelope();

    // Voice registers (directly mapped to 0x1F801C00 + voice*0x10)
    uint16_t vol_l_{0};        // 0x00: Volume Left
    uint16_t vol_r_{0};        // 0x02: Volume Right
    uint16_t pitch_{0};        // 0x04: Pitch (4.12 fixed point)
    uint16_t start_addr_{0};   // 0x06: Start address (8-byte units)
    uint16_t adsr1_{0};        // 0x08: ADSR Attack/Decay/Sustain
    uint16_t adsr2_{0};        // 0x0A: ADSR Sustain/Release
    uint16_t adsr_vol_{0};     // 0x0C: Current ADSR volume (read-only)
    uint16_t repeat_addr_{0};  // 0x0E: Loop/Repeat address (8-byte units)

    // Internal state
    uint32_t current_addr_{0};  // Current SPU RAM address (byte)
    uint32_t counter_{0};       // Pitch counter (16.16 fixed point)

    // ADPCM decode state
    int16_t prev_samples_[2]{0, 0};  // s1, s2 for filter prediction
    int16_t decoded_[28]{};          // Current decoded block (28 samples)
    int decode_idx_{28};             // Index in decoded block (28 = need new block)

    // ADSR envelope state
    enum EnvPhase { ENV_OFF, ENV_ATTACK, ENV_DECAY, ENV_SUSTAIN, ENV_RELEASE };
    EnvPhase env_phase_{ENV_OFF};
    int32_t env_level_{0};           // Current envelope level (0..0x7FFF)
    int32_t env_step_{0};            // Current envelope step (applied when counter overflows)
    int32_t env_target_{0};          // Current envelope target
    uint16_t env_counter_{0};        // Envelope timing counter
    uint16_t env_counter_inc_{0};    // Counter increment per tick
    bool env_exponential_{false};    // Exponential mode flag
    bool env_decreasing_{false};     // Direction flag
    uint8_t env_rate_{0};            // Current rate (for exponential adjustments)

    // Flags
    bool hit_loop_end_{false};

    // ADPCM filter tables (PS1 specific)
    static constexpr int16_t kPosTable[5] = {0, 60, 115, 98, 122};
    static constexpr int16_t kNegTable[5] = {0, 0, -52, -55, -60};
};

} // namespace audio
