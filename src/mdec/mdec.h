#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace mdec
{

class Mdec
{
public:
    Mdec();

    void reset();

    // MMIO register access (0x1F801820, 0x1F801824)
    uint32_t read_reg(uint32_t addr);
    void write_reg(uint32_t addr, uint32_t val);

    // DMA0 (MDEC IN): push compressed data words
    void dma_write(const uint32_t* words, uint32_t count);

    // DMA1 (MDEC OUT): pull decoded pixel words
    void dma_read(uint32_t* words, uint32_t count);

    // Query state for DMA handshake
    bool data_out_request() const;
    bool data_in_request() const;

private:
    // Commands
    enum class State : uint8_t
    {
        idle,
        decoding,
        set_quant,
        set_scale,
    };

    void execute();
    void decode_macroblock();
    bool decode_rle(int16_t* blk, const uint8_t* qt);
    void idct(int16_t* blk);
    void yuv_to_rgb(uint32_t xx, uint32_t yy,
                    const int16_t* cr, const int16_t* cb, const int16_t* y);
    void yuv_to_mono(const int16_t* y);
    void copy_out_block();

    // Zigzag reorder table (column-major, matching PS1 hardware)
    static const uint8_t s_zagzig[64];

    // State
    State state_{State::idle};
    uint8_t output_depth_{0};   // 0=4bit, 1=8bit, 2=24bit, 3=15bit
    bool output_signed_{false};
    uint8_t output_bit15_{0};
    bool enable_dma_in_{false};
    bool enable_dma_out_{false};

    uint32_t remaining_halfwords_{0};
    uint32_t current_block_{0};     // 0-5 (Cr, Cb, Y1-Y4)
    uint32_t current_coeff_{64};    // k in block (64 = not started)
    uint16_t current_q_scale_{0};

    // FIFO
    std::vector<uint16_t> fifo_in_;
    uint32_t fifo_in_pos_{0};
    std::vector<uint32_t> fifo_out_;
    uint32_t fifo_out_pos_{0};

    // Quant tables (64 bytes each)
    std::array<uint8_t, 64> iq_y_{};
    std::array<uint8_t, 64> iq_uv_{};

    // Scale (IDCT) matrix: 8x8 signed 16-bit
    std::array<int16_t, 64> scale_table_{};

    // 6 decoded blocks: [0]=Cr, [1]=Cb, [2-5]=Y1-Y4
    std::array<std::array<int16_t, 64>, 6> blocks_{};

    // RGB output buffer (16x16 pixels = 256 entries)
    std::array<uint32_t, 256> block_rgb_{};

    // FIFO helpers
    bool fifo_in_empty() const { return fifo_in_pos_ >= fifo_in_.size(); }
    uint16_t fifo_in_pop();
    void fifo_out_push(uint32_t v);
};

} // namespace mdec
