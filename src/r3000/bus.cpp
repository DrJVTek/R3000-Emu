#include "bus.h"

#include <cstdio>
#include <cstring>

#include "../audio/spu.h"
#include "../audio/wav_writer.h"
#include "../cdrom/cdrom.h"
#include "../gpu/gpu.h"
#include "../log/emu_log.h"

namespace r3000
{

Bus::Bus(
    uint8_t* ram,
    uint32_t ram_size,
    const uint8_t* bios,
    uint32_t bios_size,
    cdrom::Cdrom* cdrom,
    gpu::Gpu* gpu,
    rlog::Logger* logger
)
    : ram_(ram)
    , ram_size_(ram_size)
    , bios_(bios)
    , bios_size_(bios_size)
    , cdrom_(cdrom)
    , gpu_(gpu)
    , logger_(logger)
{
    // Initialize EXP1 region to 0xFF (open bus)
    std::memset(exp1_, 0xFF, sizeof(exp1_));

    // Create SPU
    spu_ = new audio::Spu();
    spu_owned_ = true;

    // Connect SPU to CDROM for CDDA audio
    if (spu_ && cdrom_)
    {
        spu_->set_cdrom(cdrom_);
    }

    // Set up SPU IRQ callback: when SPU triggers IRQ, set I_STAT bit 9
    if (spu_)
    {
        spu_->set_irq_callback([this]() {
            i_stat_ |= (1u << 9);  // SPU IRQ = bit 9
        });
    }
}

Bus::~Bus()
{
    if (wav_writer_)
    {
        delete wav_writer_;
        wav_writer_ = nullptr;
    }
    if (spu_owned_ && spu_)
    {
        delete spu_;
        spu_ = nullptr;
    }
}

uint32_t Bus::ram_size() const
{
    return ram_size_;
}

bool Bus::is_in_ram(uint32_t addr, uint32_t size) const
{
    if (addr > ram_size_)
        return false;
    return (ram_size_ - addr) >= size;
}

bool Bus::is_in_range(uint32_t addr, uint32_t base, uint32_t size, uint32_t access_size) const
{
    if (addr < base)
        return false;
    if (addr >= base + size)
        return false;
    return (base + size - addr) >= access_size;
}

void Bus::log_mem(const char* op, uint32_t addr, uint32_t v) const
{
    if (!logger_)
        return;
    rlog::logger_logf(
        logger_, rlog::Level::trace, rlog::Category::mem, "%s addr=0x%08X v=0x%08X", op, addr, v
    );
}

void Bus::enable_wav_output(const char* path)
{
    if (wav_writer_)
    {
        delete wav_writer_;
        wav_writer_ = nullptr;
    }
    wav_writer_ = new audio::WavWriter();
    wav_writer_->open(path, audio::Spu::kSampleRate);
    if (spu_)
    {
        spu_->set_wav_writer(wav_writer_);
    }
}

// Mask physical address from MIPS segments
static uint32_t phys_addr(uint32_t virt)
{
    // KSEG0 (0x80000000), KSEG1 (0xA0000000) -> strip high bits
    if (virt >= 0x80000000u && virt < 0xC0000000u)
        return virt & 0x1FFFFFFFu;
    return virt;
}

// PS1 RAM occupies an 8MB window (0x00000000-0x007FFFFF) with the 2MB
// physical RAM mirrored 4 times.  All accesses in this window must be
// masked to the actual RAM size.
static constexpr uint32_t kRamWindow = 0x00800000u; // 8 MB

// ------------------ SIO0 (minimal controller) ------------------
uint16_t Bus::sio0_stat_value() const
{
    uint16_t stat = (uint16_t)(sio0_stat_ | 0x00C5u); // TXRDY|TXEMPTY|DSR|CTS
    if (sio0_rx_ready_)
    {
        stat |= 0x0002u; // RXRDY
        stat |= 0x0100u; // IRQ
    }
    else
    {
        stat &= static_cast<uint16_t>(~0x0002u & 0xFFFFu);
        stat &= static_cast<uint16_t>(~0x0100u & 0xFFFFu);
    }
    return stat;
}

uint16_t Bus::sio0_read_data()
{
    uint16_t v = sio0_rx_ready_ ? (uint16_t)sio0_rx_data_ : 0x00FFu;
    sio0_rx_ready_ = 0;
    return v;
}

void Bus::sio0_write_data(uint8_t v)
{
    uint8_t resp = 0xFFu;
    switch (sio0_tx_phase_)
    {
        case 0:
            // Expect 0x01 (start)
            resp = 0xFFu;
            sio0_tx_phase_ = (v == 0x01u) ? 1u : 0u;
            break;
        case 1:
            // Command byte (poll/config/etc). Reply with digital pad ID (0x41).
            (void)v;
            resp = 0x41u;
            sio0_tx_phase_ = 2u;
            break;
        case 2:
            // Ack byte
            resp = 0x5Au;
            sio0_tx_phase_ = 3u;
            break;
        case 3:
            // Buttons low
            resp = 0xFFu;
            sio0_tx_phase_ = 4u;
            break;
        case 4:
            // Buttons high
            resp = 0xFFu;
            sio0_tx_phase_ = 0u;
            break;
        default:
            resp = 0xFFu;
            sio0_tx_phase_ = 0u;
            break;
    }
    sio0_rx_data_ = resp;
    sio0_rx_ready_ = 1;
    // Raise SIO IRQ (bit 7) when a byte is ready.
    i_stat_ |= (1u << 7);
}

// ================== READ FUNCTIONS ==================

bool Bus::read_u8(uint32_t addr, uint8_t& out, MemFault& fault)
{
    const uint32_t phys = phys_addr(addr);

    // RAM (2 MB mirrored across 8 MB window)
    if (phys < kRamWindow)
    {
        out = ram_[phys & (ram_size_ - 1)];
        return true;
    }

    // BIOS ROM (0x1FC00000)
    if (phys >= kBiosBase && phys < kBiosBase + bios_size_)
    {
        out = bios_[phys - kBiosBase];
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys < kScratchBase + kScratchSize)
    {
        out = scratch_[phys - kScratchBase];
        return true;
    }

    // CDROM (byte-access, 0x1F801800..0x1F801803)
    if (phys >= kCdromBase && phys < kCdromBase + kCdromSize)
    {
        out = cdrom_ ? cdrom_->mmio_read8(phys) : 0;
        // Check if CDROM IRQ edge occurred (e.g., after reading status that clears IRQ)
        check_cdrom_irq_edge();
        return true;
    }

    // IRQ Controller (byte read)
    if (phys >= kIrqStatAddr && phys < kIrqStatAddr + 4)
    {
        const uint32_t byte_off = phys - kIrqStatAddr;
        out = (uint8_t)((i_stat_ >> (byte_off * 8)) & 0xFF);
        return true;
    }
    if (phys >= kIrqMaskAddr && phys < kIrqMaskAddr + 4)
    {
        const uint32_t byte_off = phys - kIrqMaskAddr;
        out = (uint8_t)((i_mask_ >> (byte_off * 8)) & 0xFF);
        return true;
    }

    // SIO0 (Serial/Controller)
    if (phys >= kSio0Base && phys < kSio0Base + kSio0Size)
    {
        const uint32_t off = phys - kSio0Base;
        switch (off)
        {
            case 0x0: out = (uint8_t)(sio0_read_data() & 0xFFu); break;       // DATA
            case 0x4: out = (uint8_t)(sio0_stat_value() & 0xFFu); break;       // STAT low
            case 0x5: out = (uint8_t)((sio0_stat_value() >> 8) & 0xFFu); break; // STAT high
            case 0x8: out = (uint8_t)(sio0_mode_ & 0xFFu); break;             // MODE low
            case 0x9: out = (uint8_t)((sio0_mode_ >> 8) & 0xFFu); break;       // MODE high
            case 0xA: out = (uint8_t)(sio0_ctrl_ & 0xFFu); break;             // CTRL low
            case 0xB: out = (uint8_t)((sio0_ctrl_ >> 8) & 0xFFu); break;       // CTRL high
            case 0xE: out = (uint8_t)(sio0_baud_ & 0xFFu); break;             // BAUD low
            case 0xF: out = (uint8_t)((sio0_baud_ >> 8) & 0xFFu); break;       // BAUD high
            default: out = 0; break;
        }
        return true;
    }

    // EXP1 (open bus 0xFF)
    if (phys >= kExp1Base && phys < kExp1Base + kExp1Size)
    {
        out = exp1_[phys - kExp1Base];
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys < kIoBase + kIoSize)
    {
        out = io_[phys - kIoBase];
        return true;
    }

    // Unknown address - return 0
    out = 0;
    return true;
}

bool Bus::read_u16(uint32_t addr, uint16_t& out, MemFault& fault)
{
    if ((addr & 1u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }

    const uint32_t phys = phys_addr(addr);

    // RAM (mirrored)
    if (phys < kRamWindow)
    {
        // Note: RAM is mirrored via masking; multi-byte accesses must wrap too.
        const uint32_t rm = ram_size_ - 1;
        const uint32_t mp0 = phys & rm;
        const uint32_t mp1 = (phys + 1u) & rm;
        out = (uint16_t)ram_[mp0] | ((uint16_t)ram_[mp1] << 8);
        return true;
    }

    // BIOS
    if (phys >= kBiosBase && phys + 2 <= kBiosBase + bios_size_)
    {
        const uint32_t off = phys - kBiosBase;
        out = (uint16_t)bios_[off] | ((uint16_t)bios_[off + 1] << 8);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
        out = (uint16_t)(i_stat_ & 0xFFFF);
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        out = (uint16_t)(i_mask_ & 0xFFFF);
        return true;
    }

    // SIO0 (Serial/Controller)
    if (phys >= kSio0Base && phys < kSio0Base + kSio0Size)
    {
        const uint32_t off = phys - kSio0Base;
        switch (off)
        {
            case 0x0: out = sio0_read_data(); break;    // DATA
            case 0x4: out = sio0_stat_value(); break;   // STAT
            case 0x8: out = sio0_mode_; break;          // MODE
            case 0xA: out = sio0_ctrl_; break;          // CTRL
            case 0xE: out = sio0_baud_; break;          // BAUD
            default: out = 0; break;
        }
        return true;
    }

    // SPU registers (0x1F801C00 - 0x1F801DFF)
    if (phys >= 0x1F801C00u && phys < 0x1F801E00u)
    {
        if (spu_)
            out = spu_->read_reg(phys - 0x1F801C00u);
        else
            out = 0;
        return true;
    }

    // SPUSTAT legacy path
    if (phys == 0x1F801DAEu)
    {
        out = spu_read_stat();
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys + 2 <= kScratchBase + kScratchSize)
    {
        const uint32_t off = phys - kScratchBase;
        out = (uint16_t)scratch_[off] | ((uint16_t)scratch_[off + 1] << 8);
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys + 2 <= kIoBase + kIoSize)
    {
        const uint32_t off = phys - kIoBase;
        out = (uint16_t)io_[off] | ((uint16_t)io_[off + 1] << 8);
        return true;
    }

    out = 0;
    return true;
}

bool Bus::read_u32(uint32_t addr, uint32_t& out, MemFault& fault)
{
    if ((addr & 3u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }

    const uint32_t phys = phys_addr(addr);

    // RAM (mirrored)
    if (phys < kRamWindow)
    {
        // Note: RAM is mirrored via masking; multi-byte accesses must wrap too.
        const uint32_t rm = ram_size_ - 1;
        const uint32_t mp0 = phys & rm;
        const uint32_t mp1 = (phys + 1u) & rm;
        const uint32_t mp2 = (phys + 2u) & rm;
        const uint32_t mp3 = (phys + 3u) & rm;
        out = (uint32_t)ram_[mp0] | ((uint32_t)ram_[mp1] << 8) |
              ((uint32_t)ram_[mp2] << 16) | ((uint32_t)ram_[mp3] << 24);
        return true;
    }

    // BIOS
    if (phys >= kBiosBase && phys + 4 <= kBiosBase + bios_size_)
    {
        const uint32_t off = phys - kBiosBase;
        out = (uint32_t)bios_[off] | ((uint32_t)bios_[off + 1] << 8) |
              ((uint32_t)bios_[off + 2] << 16) | ((uint32_t)bios_[off + 3] << 24);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
        out = i_stat_;
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        out = i_mask_;
        return true;
    }

    // DMA registers
    if (phys >= 0x1F801080u && phys < 0x1F801100u)
    {
        const uint32_t off = phys - 0x1F801080u;
        const int ch = off / 0x10;
        const int reg = (off % 0x10) / 4;
        if (ch < 7)
        {
            switch (reg)
            {
            case 0: out = dma_[ch].madr; break;
            case 1: out = dma_[ch].bcr; break;
            case 2: out = dma_[ch].chcr; break;
            default: out = 0; break;
            }
            return true;
        }
    }

    // DMA control
    if (phys == 0x1F8010F0u)
    {
        out = dpcr_;
        return true;
    }
    if (phys == 0x1F8010F4u)
    {
        out = dicr_;
        return true;
    }

    // GPU
    if (phys == kGpuBase || phys == kGpuBase + 4)
    {
        out = gpu_ ? gpu_->mmio_read32(phys) : 0x14802000u;
        return true;
    }

    // CDROM (byte-access only, but handle 32-bit for completion)
    if (phys >= kCdromBase && phys < kCdromBase + kCdromSize)
    {
        if (cdrom_)
        {
            uint8_t b0 = cdrom_->mmio_read8(phys);
            out = b0;
        }
        else
        {
            out = 0;
        }
        check_cdrom_irq_edge();
        return true;
    }

    // Timers
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t off = phys - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            switch (reg)
            {
            case 0: out = timers_[ch].count; break;
            case 1: out = timers_[ch].mode; break;
            case 2: out = timers_[ch].target; break;
            default: out = 0; break;
            }
            return true;
        }
    }

    // Cache control
    if (phys == kCacheCtrlAddr || addr == kCacheCtrlAddr)
    {
        out = cache_ctrl_;
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys + 4 <= kScratchBase + kScratchSize)
    {
        const uint32_t off = phys - kScratchBase;
        out = (uint32_t)scratch_[off] | ((uint32_t)scratch_[off + 1] << 8) |
              ((uint32_t)scratch_[off + 2] << 16) | ((uint32_t)scratch_[off + 3] << 24);
        return true;
    }

    // EXP1
    if (phys >= kExp1Base && phys + 4 <= kExp1Base + kExp1Size)
    {
        out = 0xFFFFFFFFu; // Open bus
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys + 4 <= kIoBase + kIoSize)
    {
        const uint32_t off = phys - kIoBase;
        out = (uint32_t)io_[off] | ((uint32_t)io_[off + 1] << 8) |
              ((uint32_t)io_[off + 2] << 16) | ((uint32_t)io_[off + 3] << 24);
        return true;
    }

    out = 0;
    return true;
}

// ================== WRITE FUNCTIONS ==================

bool Bus::write_u8(uint32_t addr, uint8_t v, MemFault& fault)
{
    const uint32_t phys = phys_addr(addr);

    // RAM (mirrored)
    if (phys < kRamWindow)
    {
        const uint32_t mp = phys & (ram_size_ - 1);
        ram_[mp] = v;
        return true;
    }

    // CDROM
    if (phys >= kCdromBase && phys < kCdromBase + kCdromSize)
    {
        if (cdrom_)
        {
            cdrom_->mmio_write8(phys, v);
        }
        // Check for IRQ edge after write (command execution, IRQ ack, etc.)
        check_cdrom_irq_edge();
        return true;
    }

    // IRQ Controller (byte access)
    // Some BIOS handlers use SB to acknowledge individual I_STAT bytes.
    if (phys >= kIrqStatAddr && phys < kIrqStatAddr + 4)
    {
        const uint32_t byte_off = phys - kIrqStatAddr;
        // I_STAT: writing 0 clears the bit. (Writing 1 keeps it set.)
        const uint32_t shift = byte_off * 8;
        const uint32_t mask = 0xFFu << shift;
        const uint32_t old = i_stat_;
        const uint32_t new_byte = ((old >> shift) & 0xFFu) & v;
        i_stat_ = (old & ~mask) | (new_byte << shift);
        return true;
    }
    if (phys >= kIrqMaskAddr && phys < kIrqMaskAddr + 4)
    {
        const uint32_t byte_off = phys - kIrqMaskAddr;
        uint32_t shift = byte_off * 8;
        i_mask_ = (i_mask_ & ~((uint32_t)0xFF << shift)) | ((uint32_t)v << shift);
        return true;
    }

    // SIO0 (Serial/Controller)
    if (phys >= kSio0Base && phys < kSio0Base + kSio0Size)
    {
        const uint32_t off = phys - kSio0Base;
        switch (off)
        {
            case 0x0:
                sio0_data_ = v;
                sio0_write_data(v);
                break;
            case 0x4:
                sio0_stat_ = (uint16_t)((sio0_stat_ & 0xFF00u) | v);
                break;
            case 0x5:
                sio0_stat_ = (uint16_t)((sio0_stat_ & 0x00FFu) | ((uint16_t)v << 8));
                break;
            case 0x8:
                sio0_mode_ = (uint16_t)((sio0_mode_ & 0xFF00u) | v);
                break;
            case 0x9:
                sio0_mode_ = (uint16_t)((sio0_mode_ & 0x00FFu) | ((uint16_t)v << 8));
                break;
            case 0xA:
                sio0_ctrl_ = (uint16_t)((sio0_ctrl_ & 0xFF00u) | v);
                break;
            case 0xB:
                sio0_ctrl_ = (uint16_t)((sio0_ctrl_ & 0x00FFu) | ((uint16_t)v << 8));
                break;
            case 0xE:
                sio0_baud_ = (uint16_t)((sio0_baud_ & 0xFF00u) | v);
                break;
            case 0xF:
                sio0_baud_ = (uint16_t)((sio0_baud_ & 0x00FFu) | ((uint16_t)v << 8));
                break;
            default:
                break;
        }
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys < kScratchBase + kScratchSize)
    {
        scratch_[phys - kScratchBase] = v;
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys < kIoBase + kIoSize)
    {
        io_[phys - kIoBase] = v;
        return true;
    }

    return true;
}

bool Bus::write_u16(uint32_t addr, uint16_t v, MemFault& fault)
{
    if ((addr & 1u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }

    const uint32_t phys = phys_addr(addr);

    // RAM (mirrored)
    if (phys < kRamWindow)
    {
        // Note: RAM is mirrored via masking; multi-byte accesses must wrap too.
        const uint32_t rm = ram_size_ - 1;
        const uint32_t mp0 = phys & rm;
        const uint32_t mp1 = (phys + 1u) & rm;
        ram_[mp0] = (uint8_t)(v & 0xFF);
        ram_[mp1] = (uint8_t)((v >> 8) & 0xFF);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
        // I_STAT: writing 0 clears the bit. (Writing 1 keeps it set.)
        const uint32_t old = i_stat_;
        const uint32_t new_low = (old & 0xFFFFu) & (uint32_t)v;
        i_stat_ = (old & 0xFFFF0000u) | new_low;
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        i_mask_ = v;
        return true;
    }

    // SIO0 (Serial/Controller)
    if (phys >= kSio0Base && phys < kSio0Base + kSio0Size)
    {
        const uint32_t off = phys - kSio0Base;
        switch (off)
        {
            case 0x0:
                sio0_data_ = v;
                sio0_write_data((uint8_t)(v & 0xFFu));
                break;
            case 0x8: sio0_mode_ = v; break;  // MODE
            case 0xA: sio0_ctrl_ = v; break;  // CTRL
            case 0xE: sio0_baud_ = v; break;  // BAUD
            case 0x4: sio0_stat_ = v; break;  // STAT (rarely written)
            default: break;
        }
        return true;
    }

    // SPU registers (0x1F801C00 - 0x1F801DFF)
    if (phys >= 0x1F801C00u && phys < 0x1F801E00u)
    {
        const uint32_t offset = phys - 0x1F801C00u;

        // Track transfer address for DMA4 (which uses bus's position)
        if (offset == 0x1A6) // SPU transfer address
        {
            spu_xfer_addr_reg_ = v;
            spu_xfer_addr_cur_ = (uint32_t)v * 8;
        }
        else if (offset == 0x1AA) // SPUCNT
        {
            spu_cnt_reg_ = v;
            spu_apply_delay_ = 3;
        }
        else if (offset == 0x1AC) // SPU transfer control
        {
            spu_xfer_ctrl_ = v;
        }
        // NOTE: FIFO writes (offset 0x1A8) are handled entirely by SPU now
        // No bus-side buffering needed - SPU writes directly to its RAM

        // Forward all SPU register writes to SPU object
        if (spu_)
            spu_->write_reg(offset, v);

        return true;
    }

    // Timers
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t off = phys - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            switch (reg)
            {
            case 0: timers_[ch].count = v; break;
            case 1:
                timers_[ch].mode = v;
                timers_[ch].count = 0;
                break;
            case 2: timers_[ch].target = v; break;
            }
        }
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys + 2 <= kScratchBase + kScratchSize)
    {
        const uint32_t off = phys - kScratchBase;
        scratch_[off] = (uint8_t)(v & 0xFF);
        scratch_[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys + 2 <= kIoBase + kIoSize)
    {
        const uint32_t off = phys - kIoBase;
        io_[off] = (uint8_t)(v & 0xFF);
        io_[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        return true;
    }

    return true;
}

bool Bus::write_u32(uint32_t addr, uint32_t v, MemFault& fault)
{
    // Demo MMIO print
    if (addr == kMmioPrintU32)
    {
        emu::logf(emu::LogLevel::info, "GUEST", "MMIO print: %u (0x%08X)", v, v);
        return true;
    }

    if ((addr & 3u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }

    const uint32_t phys = phys_addr(addr);

    // RAM (mirrored)
    if (phys < kRamWindow)
    {
        // Note: RAM is mirrored via masking; multi-byte accesses must wrap too.
        const uint32_t rm = ram_size_ - 1;
        const uint32_t mp0 = phys & rm;
        const uint32_t mp1 = (phys + 1u) & rm;
        const uint32_t mp2 = (phys + 2u) & rm;
        const uint32_t mp3 = (phys + 3u) & rm;
        ram_[mp0] = (uint8_t)(v & 0xFF);
        ram_[mp1] = (uint8_t)((v >> 8) & 0xFF);
        ram_[mp2] = (uint8_t)((v >> 16) & 0xFF);
        ram_[mp3] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
        // I_STAT: writing 0 clears the bit. (Writing 1 keeps it set.)
        i_stat_ &= v;
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        i_mask_ = v;
        return true;
    }

    // DMA registers
    if (phys >= 0x1F801080u && phys < 0x1F801100u)
    {
        const uint32_t off = phys - 0x1F801080u;
        const int ch = off / 0x10;
        const int reg = (off % 0x10) / 4;
        if (ch < 7)
        {
            switch (reg)
            {
            case 0: dma_[ch].madr = v; break;
            case 1: dma_[ch].bcr = v; break;
            case 2:
                dma_[ch].chcr = v;
                // Handle DMA start
                if (v & 0x01000000u)
                {
                    // DMA2 (GPU)
                    if (ch == 2 && gpu_)
                    {
                        const int dir = (v >> 0) & 1;
                        const int mode = (v >> 9) & 3;

                        if (dir == 1) // To GPU
                        {
                            if (mode == 0 || mode == 1) // Burst or Block
                            {
                                const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                                const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                                const uint32_t words = (mode == 0) ? bs : bs * bc;
                                uint32_t ma = dma_[ch].madr & 0x1FFFFF;
                                for (uint32_t i = 0; i < words; ++i)
                                {
                                    uint32_t w = (uint32_t)ram_[ma] |
                                                 ((uint32_t)ram_[ma + 1] << 8) |
                                                 ((uint32_t)ram_[ma + 2] << 16) |
                                                 ((uint32_t)ram_[ma + 3] << 24);
                                    gpu_->mmio_write32(kGpuBase, w);
                                    ma = (ma + 4) & 0x1FFFFF;
                                }
                            }
                            else if (mode == 2) // Linked list
                            {
                                uint32_t node = dma_[ch].madr & 0x1FFFFF;
                                for (int safety = 0; safety < 0x100000; ++safety)
                                {
                                    uint32_t header = (uint32_t)ram_[node] |
                                                      ((uint32_t)ram_[node + 1] << 8) |
                                                      ((uint32_t)ram_[node + 2] << 16) |
                                                      ((uint32_t)ram_[node + 3] << 24);
                                    uint32_t words = header >> 24;
                                    for (uint32_t i = 0; i < words; ++i)
                                    {
                                        uint32_t off2 = (node + 4 + i * 4) & 0x1FFFFF;
                                        uint32_t w = (uint32_t)ram_[off2] |
                                                     ((uint32_t)ram_[off2 + 1] << 8) |
                                                     ((uint32_t)ram_[off2 + 2] << 16) |
                                                     ((uint32_t)ram_[off2 + 3] << 24);
                                        gpu_->mmio_write32(kGpuBase, w);
                                    }
                                    if ((header & 0x00FFFFFF) == 0x00FFFFFF)
                                        break;
                                    node = header & 0x1FFFFF;
                                }
                            }
                        }
                        dma_finish(ch);
                    }

                    // DMA4 (SPU)
                    if (ch == 4)
                    {
                        const int dir = (v >> 0) & 1;
                        const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                        const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                        const uint32_t words = bs * (bc ? bc : 1);
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

                        emu::logf(emu::LogLevel::info, "BUS", "DMA4 SPU %s madr=0x%08X bcr=0x%08X words=%u spu_addr=0x%05X",
                            dir ? "RAM→SPU" : "SPU→RAM", dma_[ch].madr, dma_[ch].bcr, words, spu_xfer_addr_cur_);

                        if (dir == 0) // From SPU (read)
                        {
                            for (uint32_t i = 0; i < words; ++i)
                            {
                                uint16_t lo = (spu_xfer_addr_cur_ + 1 < kSpuRamSize)
                                    ? ((uint16_t)spu_ram_[spu_xfer_addr_cur_] |
                                       ((uint16_t)spu_ram_[spu_xfer_addr_cur_ + 1] << 8))
                                    : 0;
                                spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);
                                uint16_t hi = (spu_xfer_addr_cur_ + 1 < kSpuRamSize)
                                    ? ((uint16_t)spu_ram_[spu_xfer_addr_cur_] |
                                       ((uint16_t)spu_ram_[spu_xfer_addr_cur_ + 1] << 8))
                                    : 0;
                                spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);

                                uint32_t w = (uint32_t)lo | ((uint32_t)hi << 16);
                                ram_[ma] = (uint8_t)(w & 0xFF);
                                ram_[ma + 1] = (uint8_t)((w >> 8) & 0xFF);
                                ram_[ma + 2] = (uint8_t)((w >> 16) & 0xFF);
                                ram_[ma + 3] = (uint8_t)((w >> 24) & 0xFF);
                                ma = (ma + 4) & 0x1FFFFF;
                            }
                        }
                        else // To SPU (write)
                        {
                            for (uint32_t i = 0; i < words; ++i)
                            {
                                uint32_t w = (uint32_t)ram_[ma] |
                                             ((uint32_t)ram_[ma + 1] << 8) |
                                             ((uint32_t)ram_[ma + 2] << 16) |
                                             ((uint32_t)ram_[ma + 3] << 24);

                                uint16_t lo = (uint16_t)(w & 0xFFFF);
                                uint16_t hi = (uint16_t)((w >> 16) & 0xFFFF);

                                // Write directly to SPU's RAM (not bus's copy)
                                // This preserves any FIFO-written data
                                if (spu_)
                                {
                                    spu_->write_ram(spu_xfer_addr_cur_, lo);
                                    spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);
                                    spu_->write_ram(spu_xfer_addr_cur_, hi);
                                    spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);
                                }
                                else
                                {
                                    // Fallback to bus's copy if no SPU (shouldn't happen)
                                    if (spu_xfer_addr_cur_ + 1 < kSpuRamSize)
                                    {
                                        spu_ram_[spu_xfer_addr_cur_] = (uint8_t)(lo & 0xFF);
                                        spu_ram_[spu_xfer_addr_cur_ + 1] = (uint8_t)((lo >> 8) & 0xFF);
                                    }
                                    spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);

                                    if (spu_xfer_addr_cur_ + 1 < kSpuRamSize)
                                    {
                                        spu_ram_[spu_xfer_addr_cur_] = (uint8_t)(hi & 0xFF);
                                        spu_ram_[spu_xfer_addr_cur_ + 1] = (uint8_t)((hi >> 8) & 0xFF);
                                    }
                                    spu_xfer_addr_cur_ = (spu_xfer_addr_cur_ + 2) & (kSpuRamSize - 1);
                                }

                                ma = (ma + 4) & 0x1FFFFF;
                            }
                            // NOTE: No memcpy - DMA writes go directly to SPU's RAM now
                        }

                        dma_finish(ch);
                    }

                    // DMA3 (CDROM → RAM)
                    if (ch == 3 && cdrom_)
                    {
                        emu::logf(emu::LogLevel::info, "BUS", "DMA3 CD→RAM madr=0x%08X bcr=0x%08X words=%u",
                            dma_[ch].madr, dma_[ch].bcr,
                            (dma_[ch].bcr & 0xFFFF) * (((dma_[ch].bcr >> 16) & 0xFFFF) ? ((dma_[ch].bcr >> 16) & 0xFFFF) : 1));
                        const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                        const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                        const uint32_t words = bs * (bc ? bc : 1);
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

                        for (uint32_t i = 0; i < words; ++i)
                        {
                            // Read 4 bytes from CDROM data FIFO
                            const uint8_t b0 = cdrom_->mmio_read8(0x1F801802u);
                            const uint8_t b1 = cdrom_->mmio_read8(0x1F801802u);
                            const uint8_t b2 = cdrom_->mmio_read8(0x1F801802u);
                            const uint8_t b3 = cdrom_->mmio_read8(0x1F801802u);
                            const uint32_t w = (uint32_t)b0 | ((uint32_t)b1 << 8) |
                                               ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
                            ram_[ma]     = (uint8_t)(w & 0xFF);
                            ram_[ma + 1] = (uint8_t)((w >> 8) & 0xFF);
                            ram_[ma + 2] = (uint8_t)((w >> 16) & 0xFF);
                            ram_[ma + 3] = (uint8_t)((w >> 24) & 0xFF);
                            ma = (ma + 4) & 0x1FFFFF;
                        }

                        dma_finish(ch);
                    }

                    // DMA6 (OTC - Ordering Table Clear)
                    if (ch == 6)
                    {
                        uint32_t words = dma_[ch].bcr & 0xFFFF;
                        if (words == 0) words = 0x10000;
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

                        for (uint32_t i = 0; i < words; ++i)
                        {
                            uint32_t w = (i == words - 1) ? 0x00FFFFFFu : ((ma - 4) & 0x1FFFFF);
                            ram_[ma] = (uint8_t)(w & 0xFF);
                            ram_[ma + 1] = (uint8_t)((w >> 8) & 0xFF);
                            ram_[ma + 2] = (uint8_t)((w >> 16) & 0xFF);
                            ram_[ma + 3] = (uint8_t)((w >> 24) & 0xFF);
                            ma = (ma - 4) & 0x1FFFFF;
                        }
                        dma_finish(ch);
                    }
                }
                break;
            }
        }
        return true;
    }

    // DMA control
    if (phys == 0x1F8010F0u)
    {
        dpcr_ = v;
        return true;
    }
    if (phys == 0x1F8010F4u)
    {
        // DICR write: bits 0-14 force enable, bit 15 force IRQ, bits 16-22 channel enable,
        // bit 23 master enable, bits 24-30 write-1-to-acknowledge (clear flag), bit 31 read-only.
        const uint32_t ack_mask = v & 0x7F000000u; // bits 24-30: writing 1 clears flag
        const uint32_t wr_mask  = 0x00FF803Fu;     // bits 0-5, 15-23: writable directly
        dicr_ = (dicr_ & ~wr_mask) | (v & wr_mask);
        dicr_ &= ~ack_mask; // acknowledge flags

        // Recompute master flag (bit 31): set if any (flag & enable) channel is active and master enable is set
        const uint32_t flags   = (dicr_ >> 24) & 0x7Fu;
        const uint32_t enables = (dicr_ >> 16) & 0x7Fu;
        const int force = (dicr_ >> 15) & 1;
        const int master_en = (dicr_ >> 23) & 1;
        if (master_en && (force || (flags & enables)))
            dicr_ |= (1u << 31);
        else
            dicr_ &= ~(1u << 31);
        return true;
    }

    // GPU
    if (phys == kGpuBase || phys == kGpuBase + 4)
    {
        if (gpu_)
            gpu_->mmio_write32(phys, v);
        return true;
    }

    // Timers
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t off = phys - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            switch (reg)
            {
            case 0: timers_[ch].count = (uint16_t)v; break;
            case 1:
                timers_[ch].mode = (uint16_t)v;
                timers_[ch].count = 0;
                break;
            case 2: timers_[ch].target = (uint16_t)v; break;
            }
        }
        return true;
    }

    // Cache control
    if (phys == kCacheCtrlAddr || addr == kCacheCtrlAddr)
    {
        cache_ctrl_ = v;
        return true;
    }

    // SPU 32-bit writes (split into two 16-bit)
    if (phys >= 0x1F801C00u && phys < 0x1F801E00u)
    {
        MemFault dummy{};
        write_u16(addr, (uint16_t)(v & 0xFFFF), dummy);
        write_u16(addr + 2, (uint16_t)((v >> 16) & 0xFFFF), dummy);
        return true;
    }

    // Scratchpad
    if (phys >= kScratchBase && phys + 4 <= kScratchBase + kScratchSize)
    {
        const uint32_t off = phys - kScratchBase;
        scratch_[off] = (uint8_t)(v & 0xFF);
        scratch_[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        scratch_[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        scratch_[off + 3] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    // I/O fallback
    if (phys >= kIoBase && phys + 4 <= kIoBase + kIoSize)
    {
        const uint32_t off = phys - kIoBase;
        io_[off] = (uint8_t)(v & 0xFF);
        io_[off + 1] = (uint8_t)((v >> 8) & 0xFF);
        io_[off + 2] = (uint8_t)((v >> 16) & 0xFF);
        io_[off + 3] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    return true;
}

// ================== SPU HELPERS ==================

void Bus::spu_tick_one()
{
    // Apply SPUCNT with delay
    if (spu_apply_delay_ > 0)
    {
        --spu_apply_delay_;
        if (spu_apply_delay_ == 0)
        {
            spu_cnt_applied_ = spu_cnt_reg_;
        }
    }

    // Clear busy flag after transfer delay
    if (spu_busy_delay_ > 0)
    {
        --spu_busy_delay_;
        if (spu_busy_delay_ == 0)
        {
            spu_busy_ = 0;
        }
    }
}

uint16_t Bus::spu_read_stat() const
{
    // SPUSTAT: bits 0-5 = current mode (from SPUCNT), bit 10 = transfer busy
    uint16_t stat = spu_cnt_applied_ & 0x3F;
    if (spu_busy_)
        stat |= (1 << 10);
    return stat;
}

uint32_t Bus::irq_pending_masked() const
{
    return i_stat_ & i_mask_;
}

// ================== DMA completion ==================

void Bus::dma_finish(int ch)
{
    dma_[ch].chcr &= ~0x01000000u; // clear busy/trigger bit

    // DICR: set flag bit (24+ch) for software polling
    dicr_ |= (1u << (24 + ch));

    // Recompute master flag (bit 31)
    const uint32_t flags   = (dicr_ >> 24) & 0x7Fu;
    const uint32_t enables = (dicr_ >> 16) & 0x7Fu;
    const int force = (dicr_ >> 15) & 1;
    const int master_en = (dicr_ >> 23) & 1;
    if (master_en && (force || (flags & enables)))
        dicr_ |= (1u << 31);
    else
        dicr_ &= ~(1u << 31);

    // Raise DMA IRQ (I_STAT bit 3) when DICR master flag is set.
    // The BIOS uses DMA IRQs during CD boot (e.g., sector transfers).
    if (dicr_ & (1u << 31))
        i_stat_ |= (1u << 3);
}

// ================== EVENT DELIVERY ==================

// Deliver events for a given class in the kernel event table.
// This is used as a workaround when the kernel IRQ handler can't
// dispatch certain IRQs (e.g. CDROM) without re-entrancy issues.
static void deliver_events_for_class(uint8_t* ram, uint32_t ram_size, uint32_t cls_match)
{
    const uint32_t ptr_off = 0x0120 & (ram_size - 1);
    const uint32_t evt_ptr = (uint32_t)ram[ptr_off] |
                             ((uint32_t)ram[ptr_off + 1] << 8) |
                             ((uint32_t)ram[ptr_off + 2] << 16) |
                             ((uint32_t)ram[ptr_off + 3] << 24);
    if (evt_ptr == 0)
        return;

    const uint32_t size_off = 0x0124 & (ram_size - 1);
    const uint32_t tbl_size = (uint32_t)ram[size_off] |
                              ((uint32_t)ram[size_off + 1] << 8) |
                              ((uint32_t)ram[size_off + 2] << 16) |
                              ((uint32_t)ram[size_off + 3] << 24);
    const uint32_t max_entries = (tbl_size > 0) ? (tbl_size / 0x1C) : 16;
    const uint32_t base_phys = evt_ptr & (ram_size - 1);

    for (uint32_t i = 0; i < max_entries && i < 64; ++i)
    {
        const uint32_t eoff = base_phys + i * 0x1C;
        if (eoff + 0x14 > ram_size)
            break;

        const uint32_t cls = (uint32_t)ram[eoff] |
                             ((uint32_t)ram[eoff + 1] << 8) |
                             ((uint32_t)ram[eoff + 2] << 16) |
                             ((uint32_t)ram[eoff + 3] << 24);
        if (cls != cls_match)
            continue;

        const uint32_t st_off = eoff + 0x04;
        const uint32_t status = (uint32_t)ram[st_off] |
                                ((uint32_t)ram[st_off + 1] << 8) |
                                ((uint32_t)ram[st_off + 2] << 16) |
                                ((uint32_t)ram[st_off + 3] << 24);

        if (!(status & 0x2000u))
            continue;

        // DeliverEvent: set status to "ready" (0x4000)
        const uint32_t new_status = 0x4000u;
        ram[st_off]     = (uint8_t)(new_status & 0xFF);
        ram[st_off + 1] = (uint8_t)((new_status >> 8) & 0xFF);
        ram[st_off + 2] = (uint8_t)((new_status >> 16) & 0xFF);
        ram[st_off + 3] = (uint8_t)((new_status >> 24) & 0xFF);
    }
}

// ================== CDROM IRQ EDGE CHECK ==================

void Bus::check_cdrom_irq_edge()
{
    if (!cdrom_)
        return;

    // CDROM IRQ: edge-triggered into I_STAT bit 2.
    // On real PS1, I_STAT latches on a 0->1 transition and is cleared by
    // writing 0 to I_STAT (not by the line going low).
    const uint8_t cdirq = cdrom_->irq_line();
    if (cdirq && !cdrom_irq_prev_)
    {
        i_stat_ |= (1u << 2);
        emu::logf(emu::LogLevel::info, "BUS", "CDROM IRQ edge: i_stat=0x%04X", (unsigned)i_stat_);
        // BIOS event system: mark CDROM class events as ready.
        // This mirrors the kernel DeliverEvent() that would normally run in the IRQ handler.
        deliver_events_for_class(ram_, ram_size_, 0x28u);
    }
    cdrom_irq_prev_ = cdirq;
}

// ================== TICK ==================

void Bus::tick(uint32_t cycles)
{
    static constexpr uint32_t kForceMaskAfterCycles = 600000u; // ~1 VBlank period worth of CPU cycles

    // Tick timers
    for (int ch = 0; ch < 3; ++ch)
    {
        if ((timers_[ch].mode & 0x0400) == 0) // Not stopped
        {
            uint32_t inc = cycles;

            // Accumulator-based prescaler for dotclock/hblank modes
            if (ch == 0 && (timers_[ch].mode & 0x0100))
            {
                // Dotclock: ~8 CPU cycles per dot
                timer_prescale_accum_[0] += cycles;
                inc = timer_prescale_accum_[0] / 8;
                timer_prescale_accum_[0] %= 8;
            }
            else if (ch == 1 && (timers_[ch].mode & 0x0100))
            {
                // HBlank: ~2150 CPU cycles per HBlank line
                timer_prescale_accum_[1] += cycles;
                inc = timer_prescale_accum_[1] / 2150;
                timer_prescale_accum_[1] %= 2150;
            }
            else if (ch == 2 && (timers_[ch].mode & 0x0100))
            {
                // Timer 2 with prescaler: sysclock/8 (bit 8 set = use sysclock/8)
                timer_prescale_accum_[2] += cycles;
                inc = timer_prescale_accum_[2] / 8;
                timer_prescale_accum_[2] %= 8;
            }

            uint32_t new_count = timers_[ch].count + inc;

            // Check target hit
            if ((timers_[ch].mode & 0x0008) && timers_[ch].target != 0)
            {
                if (timers_[ch].count < timers_[ch].target && new_count >= timers_[ch].target)
                {
                    timers_[ch].mode |= 0x0800; // Target reached flag
                    if (timers_[ch].mode & 0x0010)
                    {
                        // IRQ on target
                        i_stat_ |= (1u << (4 + ch));
                    }
                    if (timers_[ch].mode & 0x0008)
                    {
                        new_count = new_count - timers_[ch].target;
                    }
                }
            }

            // Check overflow
            if (new_count > 0xFFFF)
            {
                timers_[ch].mode |= 0x1000; // Overflow flag
                if (timers_[ch].mode & 0x0020)
                {
                    i_stat_ |= (1u << (4 + ch));
                }
                new_count &= 0xFFFF;
            }

            timers_[ch].count = (uint16_t)new_count;
        }
    }

    // SPU tick (apply simple cycle delays without per-cycle loop)
    if (cycles != 0)
    {
        // Apply SPUCNT with delay
        if (spu_apply_delay_ > 0)
        {
            if (cycles >= spu_apply_delay_)
            {
                spu_apply_delay_ = 0;
                spu_cnt_applied_ = spu_cnt_reg_;
            }
            else
            {
                spu_apply_delay_ -= cycles;
            }
        }

        // Clear busy flag after transfer delay
        if (spu_busy_delay_ > 0)
        {
            if (cycles >= spu_busy_delay_)
            {
                spu_busy_delay_ = 0;
                spu_busy_ = 0;
            }
            else
            {
                spu_busy_delay_ -= cycles;
            }
        }
    }

    // Tick SPU audio generation
    if (spu_)
    {
        spu_->tick_cycles(cycles);
    }

    // Tick GPU VBlank
    if (gpu_)
    {
        if (gpu_->tick_vblank(cycles))
        {
            i_stat_ |= (1u << 0); // VBlank IRQ (bit 0)

            // Workaround: some BIOS ROMs (e.g. SCPH-7502) never write I_MASK
            // to non-zero — all ROM write sites store r0. The kernel's
            // SysEnqIntRP dispatch chains ARE populated (VBlank at [0],
            // CDROM at [2]), so the exception handler works correctly once
            // IRQs are enabled. After 40 VBlanks (well past BIOS init),
            // we force I_MASK to enable the standard IRQ sources.
            if (i_mask_ == 0)
            {
                ++vblank_no_mask_count_;
                if (vblank_no_mask_count_ >= 40)
                {
                    // Enable standard IRQ sources, including CDROM.
                    // Note: earlier bring-up versions avoided CDROM IRQ due to
                    // re-entrancy concerns in the BIOS handler; we now prefer
                    // correctness so BIOS CD boot can work.
                    i_mask_ = 0x0075; // VBlank(0) | CDROM(2) | TMR0(4) | TMR1(5) | TMR2(6)
                    emu::logf(emu::LogLevel::info, "BUS", "Auto-enable I_MASK=0x%04X after %u VBlanks", (unsigned)i_mask_, (unsigned)vblank_no_mask_count_);
                }
            }
            else
            {
                vblank_no_mask_count_ = 0;
            }
        }
    }

    // Tick CDROM
    if (cdrom_)
    {
        cdrom_->tick(cycles);

        // CDROM IRQ edge detection is now done in check_cdrom_irq_edge() which
        // is called after every CDROM register access. This ensures I_STAT bit 2
        // is set before the game can poll the CDROM for the IRQ type.
        // We still do a check here as a fallback for async IRQs (reads, etc.).
        check_cdrom_irq_edge();
    }

    // If BIOS never enables IRQs (I_MASK stays 0), it can hang forever waiting on events.
    // Force-enable a minimal mask after some time has passed, even if VBlank is not reached
    // due to host throttling (e.g. UE5 time budget).
    if (i_mask_ == 0)
    {
        if (no_mask_cycles_ < 0xFFFFFFFFu - cycles)
            no_mask_cycles_ += cycles;
        else
            no_mask_cycles_ = 0xFFFFFFFFu;

        if (no_mask_cycles_ >= kForceMaskAfterCycles)
        {
            i_mask_ = 0x0075; // VBlank(0) | CDROM(2) | TMR0(4) | TMR1(5) | TMR2(6)
            emu::logf(emu::LogLevel::info, "BUS", "Auto-enable I_MASK=0x%04X after cycles=%u (VBlanks=%u)",
                (unsigned)i_mask_, (unsigned)no_mask_cycles_, (unsigned)vblank_no_mask_count_);
        }
    }
    else
    {
        no_mask_cycles_ = 0;
    }
}

} // namespace r3000
