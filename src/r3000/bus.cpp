#include "bus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../audio/spu.h"
#include "../audio/wav_writer.h"
#include "../cdrom/cdrom.h"
#include "../gpu/gpu.h"

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

// ================== READ FUNCTIONS ==================

bool Bus::read_u8(uint32_t addr, uint8_t& out, MemFault& fault)
{
    const uint32_t phys = phys_addr(addr);

    // RAM (2 MB mirrored)
    if (phys < ram_size_)
    {
        out = ram_[phys];
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

    // RAM
    if (phys + 2 <= ram_size_)
    {
        out = (uint16_t)ram_[phys] | ((uint16_t)ram_[phys + 1] << 8);
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

    // RAM
    if (phys + 4 <= ram_size_)
    {
        out = (uint32_t)ram_[phys] | ((uint32_t)ram_[phys + 1] << 8) |
              ((uint32_t)ram_[phys + 2] << 16) | ((uint32_t)ram_[phys + 3] << 24);
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

    // RAM
    if (phys < ram_size_)
    {
        ram_[phys] = v;
        return true;
    }

    // CDROM
    if (phys >= kCdromBase && phys < kCdromBase + kCdromSize)
    {
        if (cdrom_)
            cdrom_->mmio_write8(phys, v);
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

    // RAM
    if (phys + 2 <= ram_size_)
    {
        ram_[phys] = (uint8_t)(v & 0xFF);
        ram_[phys + 1] = (uint8_t)((v >> 8) & 0xFF);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
        // Writing clears bits (acknowledge)
        i_stat_ &= v;
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        i_mask_ = v;
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
        std::fprintf(stderr, "[GUEST:MMIO] %u (0x%08X)\n", v, v);
        std::fflush(stderr);
        return true;
    }

    if ((addr & 3u) != 0u)
    {
        fault = {MemFault::Kind::unaligned, addr};
        return false;
    }

    const uint32_t phys = phys_addr(addr);

    // RAM
    if (phys + 4 <= ram_size_)
    {
        ram_[phys] = (uint8_t)(v & 0xFF);
        ram_[phys + 1] = (uint8_t)((v >> 8) & 0xFF);
        ram_[phys + 2] = (uint8_t)((v >> 16) & 0xFF);
        ram_[phys + 3] = (uint8_t)((v >> 24) & 0xFF);
        return true;
    }

    // IRQ Controller
    if (phys == kIrqStatAddr)
    {
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
                        dma_[ch].chcr &= ~0x01000000u;
                    }

                    // DMA4 (SPU)
                    if (ch == 4)
                    {
                        static int dma4_debug = 0;
                        if (++dma4_debug <= 10)
                        {
                            std::fprintf(stderr, "[DMA4] Transfer #%d: madr=0x%08X bcr=0x%08X chcr=0x%08X\n",
                                dma4_debug, dma_[ch].madr, dma_[ch].bcr, v);
                        }
                        const int dir = (v >> 0) & 1;
                        const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                        const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                        const uint32_t words = bs * (bc ? bc : 1);
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

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

                        dma_[ch].chcr &= ~0x01000000u;
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
                        dma_[ch].chcr &= ~0x01000000u;
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
        dicr_ = v;
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

// ================== TICK ==================

void Bus::tick(uint32_t cycles)
{
    // Tick timers
    for (int ch = 0; ch < 3; ++ch)
    {
        if ((timers_[ch].mode & 0x0400) == 0) // Not stopped
        {
            uint32_t inc = cycles;

            // Simple prescaler for dotclock/hblank modes
            if (ch == 0 && (timers_[ch].mode & 0x0100))
                inc = cycles / 8; // Dotclock div
            if (ch == 1 && (timers_[ch].mode & 0x0100))
                inc = cycles / 2150; // HBlank div approx

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

    // SPU tick
    for (uint32_t i = 0; i < cycles; ++i)
    {
        spu_tick_one();
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
        }
    }

    // Tick CDROM
    if (cdrom_)
    {
        // Check for CDROM IRQ
        uint8_t cdirq = cdrom_->irq_line();
        if (cdirq && !cdrom_irq_prev_)
        {
            i_stat_ |= (1u << 2); // CDROM IRQ
        }
        cdrom_irq_prev_ = cdirq;
    }
}

} // namespace r3000
