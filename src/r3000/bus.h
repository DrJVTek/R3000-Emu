#pragma once

#include <cstddef>
#include <cstdint>

#include "../log/filelog.h"
#include "../log/logger.h"

namespace cdrom
{
class Cdrom;
}

namespace gpu
{
class Gpu;
}

namespace audio
{
class Spu;
class WavWriter;
}

namespace r3000
{

// Bus minimal: RAM plate + une MMIO fictive pour démo live.
// - Little-endian (PS1).
// - Exceptions d'alignement pour LH/LW/SH/SW.
class Bus
{
  public:
    struct MemFault
    {
        enum class Kind
        {
            out_of_range,
            unaligned,
        };
        Kind kind{};
        uint32_t addr{};
    };

    Bus(
        uint8_t* ram,
        uint32_t ram_size,
        const uint8_t* bios,
        uint32_t bios_size,
        cdrom::Cdrom* cdrom,
        gpu::Gpu* gpu,
        rlog::Logger* logger = nullptr
    );
    ~Bus();

    uint32_t ram_size() const;
    uint8_t* ram_ptr() { return ram_; }
    bool has_bios() const { return bios_ != nullptr && bios_size_ != 0; }

    // Lecture/écriture RAM/MMIO. Retourne false en cas de fault (et remplit fault).
    bool read_u8(uint32_t addr, uint8_t& out, MemFault& fault);
    bool read_u16(uint32_t addr, uint16_t& out, MemFault& fault);
    bool read_u32(uint32_t addr, uint32_t& out, MemFault& fault);

    bool write_u8(uint32_t addr, uint8_t v, MemFault& fault);
    bool write_u16(uint32_t addr, uint16_t v, MemFault& fault);
    bool write_u32(uint32_t addr, uint32_t v, MemFault& fault);

    // MMIO "print" (demo): écrire un u32 à cette adresse => affiche la valeur (décimal + hex).
    static constexpr uint32_t kMmioPrintU32 = 0x1F00'0000u;

    // Tick "hardware" minimal (timers/IRQ controller).
    // Modèle très simplifié: on avance d'un certain nombre de cycles (souvent 1/cpu step).
    void tick(uint32_t cycles);

    // Check CDROM IRQ edge and latch into I_STAT bit 2.
    // Called after every CDROM register access to ensure I_STAT is updated
    // before the CPU can poll the CDROM again.
    void check_cdrom_irq_edge();

    // Debug: trace ciblé des écritures RAM "sensibles" (vecteurs low RAM, watch address).
    // Objectif: voir si le BIOS installe/écrase réellement les handlers en RAM, ou si tout reste à 0.
    void set_trace_vectors(int enabled)
    {
        trace_vectors_ = enabled ? 1 : 0;
    }

    void set_trace_vector_sink(const flog::Sink& s, const flog::Clock& c)
    {
        trace_sink_ = s;
        trace_clock_ = c;
        trace_has_clock_ = 1;
    }

    // CPU PC tracking for debug (set by CPU before store instructions)
    void set_cpu_pc(uint32_t pc) { cpu_pc_ = pc; }
    uint32_t cpu_pc() const { return cpu_pc_; }

    // Debug: watch d'une adresse RAM (physique) - log les writes byte qui la touchent.
    // Exemple: pour watcher 0x8009A204 (KSEG0), donner phys=0x0009A204.
    void set_watch_ram_u32(uint32_t phys_addr, int enabled)
    {
        watch_ram_u32_phys_ = phys_addr;
        watch_ram_u32_enabled_ = enabled ? 1 : 0;
        watch_ram_u32_last_valid_ = 0;
        watch_ram_u32_read_seen_ = 0;
    }

    // PS1 interrupt controller (I_STAT/I_MASK): bits pending & mask.
    // Retourne (I_STAT & I_MASK).
    uint32_t irq_pending_masked() const;

    // Debug: raw access to I_STAT and I_MASK for diagnostic logging.
    uint32_t irq_stat_raw() const { return i_stat_; }
    uint32_t irq_mask_raw() const { return i_mask_; }

    // Set a specific I_STAT bit (used by HLE VBlank delivery).
    void set_i_stat_bit(uint32_t bit) { i_stat_ |= (1u << bit); }

    // Accès device (pour HLE BIOS côté CPU).
    cdrom::Cdrom* cdrom() const { return cdrom_; }

    // SPU access
    audio::Spu* spu() const { return spu_; }

    // Enable WAV output for audio debugging
    void enable_wav_output(const char* path);

  private:
    void dma_finish(int ch);
    bool is_in_ram(uint32_t addr, uint32_t size) const;
    bool is_in_range(uint32_t addr, uint32_t base, uint32_t size, uint32_t access_size) const;
    void log_mem(const char* op, uint32_t addr, uint32_t v) const;
    void sio0_write_data(uint8_t v);
    uint16_t sio0_read_data();
    uint16_t sio0_stat_value() const;

    uint8_t* ram_{nullptr};
    uint32_t ram_size_{0};
    const uint8_t* bios_{nullptr};
    uint32_t bios_size_{0};
    cdrom::Cdrom* cdrom_{nullptr};
    gpu::Gpu* gpu_{nullptr};
    rlog::Logger* logger_{nullptr};

    // SPU (full implementation)
    audio::Spu* spu_{nullptr};
    audio::WavWriter* wav_writer_{nullptr};
    bool spu_owned_{false};  // true if we created the SPU

    // PS1 memory-mapped regions with backing stores.
    // These emulate real hardware behavior for unmapped/unconnected regions.
    static constexpr uint32_t kScratchBase = 0x1F80'0000u;
    static constexpr uint32_t kScratchSize = 1024u;

    // I/O register fallback region - stores values for registers without dedicated emulation.
    // Real PS1: unmapped I/O returns last written value or open-bus.
    static constexpr uint32_t kIoBase = 0x1F80'1000u;
    static constexpr uint32_t kIoSize = 0x2000u; // 0x1F801000..0x1F802FFF

    // EXP1 region (Expansion Port 1). BIOS probes this on boot for connected hardware.
    // Real PS1: unconnected expansion returns open-bus (0xFF) or latched values.
    static constexpr uint32_t kExp1Base = 0x1F00'0000u;
    static constexpr uint32_t kExp1Size = 0x1'0000u;

    static constexpr uint32_t kCdromBase = 0x1F80'1800u;
    static constexpr uint32_t kCdromSize = 4u;

    static constexpr uint32_t kGpuBase = 0x1F80'1810u;
    static constexpr uint32_t kGpuSize = 8u; // 0x1810..0x1817 (GP0/GP1)

    // SIO0 (Serial/Controller)
    static constexpr uint32_t kSio0Base = 0x1F80'1040u;
    static constexpr uint32_t kSio0Size = 0x10u; // 0x1040..0x104F

    static constexpr uint32_t kBiosBase = 0x1FC0'0000u;
    static constexpr uint32_t kBiosMaxSize = 512u * 1024u;

    // Cache control (R3000A): BIOS écrit typiquement ici au boot.
    // Sur PS1: 0xFFFE0130.
    static constexpr uint32_t kCacheCtrlAddr = 0xFFFE'0130u;

    // IRQ controller (PS1)
    static constexpr uint32_t kIrqStatAddr = 0x1F80'1070u; // I_STAT
    static constexpr uint32_t kIrqMaskAddr = 0x1F80'1074u; // I_MASK

    // Timers (PS1) - 3 canaux: counter/mode/target.
    static constexpr uint32_t kTimerBase = 0x1F80'1100u;
    static constexpr uint32_t kTimerSpan = 0x30u; // 0x1100..0x112F
    static constexpr uint32_t kTimerBlock = 0x10u;

    struct Timer
    {
        uint16_t count;
        uint16_t mode;
        uint16_t target;
        uint16_t _pad;
    };

    uint8_t scratch_[kScratchSize]{};
    uint8_t io_[kIoSize]{};
    uint8_t exp1_[kExp1Size]{};
    uint32_t cache_ctrl_{0};

    // Minimal HW state (bring-up; non cycle-accurate, but explicit/observable)
    uint32_t i_stat_{0};
    uint32_t i_mask_{0};
    Timer timers_[3]{};
    uint32_t timer_prescale_accum_[3]{}; // prescaler accumulator (TMR0=dotclock, TMR1=hblank, TMR2=sysclock/8)

    // SIO0 minimal state (enough for BIOS polling loops)
    uint16_t sio0_data_{0};
    uint16_t sio0_stat_{0x0005u}; // TXRDY|TXEMPTY by default
    uint16_t sio0_mode_{0};
    uint16_t sio0_ctrl_{0};
    uint16_t sio0_baud_{0};
    uint8_t sio0_rx_data_{0xFF};
    uint8_t sio0_rx_ready_{0};
    uint8_t sio0_tx_phase_{0};

    // DMA controller (PS1) - minimal (suffisant pour BIOS init).
    // On implémente surtout DMA2 (GPU) linked-list pour déverrouiller les boucles BIOS qui attendent CHCR.
    struct DmaChannel
    {
        uint32_t madr;
        uint32_t bcr;
        uint32_t chcr;
    };

    DmaChannel dma_[7]{};
    uint32_t dpcr_{0};
    uint32_t dicr_{0};
    uint8_t dma_irq_prev_{0};
    uint8_t cdrom_irq_prev_{0};
    uint32_t vblank_no_mask_count_{0}; // VBlank frames with I_MASK=0 (for auto-enable workaround)
    uint32_t no_mask_cycles_{0}; // Cycles accumulated with I_MASK=0 (for auto-enable workaround)

    // ----------------------------
    // SPU (minimal, spec-aligned)
    // ----------------------------
    // Pieces required for BIOS bring-up:
    // - SPUCNT (1F801DAA) applies with delay (reflected in SPUSTAT bits0-5)
    // - SPUSTAT (1F801DAE) exposes transfer busy and current mode
    // - Manual write FIFO (1F801DA8) + transfer addr/control (1F801DA6/1F801DAC)
    uint16_t spu_cnt_reg_{0};
    uint16_t spu_cnt_applied_{0};
    uint32_t spu_apply_delay_{0};

    uint16_t spu_xfer_addr_reg_{0}; // in 8-byte units
    uint32_t spu_xfer_addr_cur_{0}; // byte address (increments during transfer)
    uint16_t spu_xfer_ctrl_{0};

    uint16_t spu_fifo_[32]{};
    uint8_t spu_fifo_count_{0};
    uint8_t spu_busy_{0}; // SPUSTAT.bit10
    uint32_t spu_busy_delay_{0};
    uint32_t spu_xfer_pending_bytes_{0};
    static constexpr uint32_t kSpuRamSize = 512u * 1024u; // 512 KiB
    uint8_t spu_ram_[kSpuRamSize]{};

    void spu_tick_one();
    uint16_t spu_read_stat() const;

    // Debug: vector audit / watchpoints (vers io.log).
    int trace_vectors_{0};
    flog::Sink trace_sink_{};
    flog::Clock trace_clock_{};
    int trace_has_clock_{0};
    uint32_t cpu_pc_{0};
    uint32_t watch_ram_u32_phys_{0};
    int watch_ram_u32_enabled_{0};
    uint32_t watch_ram_u32_last_{0};
    int watch_ram_u32_last_valid_{0};
    uint8_t watch_ram_u32_read_seen_{0};
};

} // namespace r3000
