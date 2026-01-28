#include "gpu.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <codecvt>
#include <locale>
#include <string>
#endif

static std::FILE* fopen_utf8(const char* path, const char* mode)
{
    if (!path || !mode)
        return nullptr;
#if defined(_WIN32)
    std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> conv;
    const std::wstring wpath = conv.from_bytes(path);
    const std::wstring wmode = conv.from_bytes(mode);
    return _wfopen(wpath.c_str(), wmode.c_str());
#else
    return std::fopen(path, mode);
#endif
}

namespace gpu
{

static void gpu_log(
    const flog::Sink& s,
    const flog::Sink& combined,
    const flog::Clock& c,
    int has_clock,
    flog::Level lvl,
    const char* fmt,
    ...
)
{
    if (!has_clock || (!s.f && !combined.f) || !fmt)
        return;

    va_list args;
    va_start(args, fmt);
    flog::vlogf(s, c, lvl, "GPU", fmt, args);
    va_end(args);

    va_start(args, fmt);
    flog::vlogf(combined, c, lvl, "GPU", fmt, args);
    va_end(args);
}

Gpu::Gpu(rlog::Logger* logger)
    : logger_(logger)
    , vram_(std::make_unique<uint16_t[]>(kVramPixels))
{
    // Valeur "classique" utilisée par beaucoup d'emus comme status initial:
    // - ready to receive cmd
    // - DMA off
    // - display enabled (selon bits)
    //
    // NOTE: GPUSTAT reset state documenté (psx-spx): 14802000h.
    // On maintient un état minimal + une vue calculée au moment de la lecture.
    status_ = 0x1480'2000u;
    dma_dir_ = 0;
    vblank_div_ = 0;

    // VRAM reset: sur du vrai hardware, le contenu est indéterminé.
    // Pour le bring-up BIOS, un clear à 0 est suffisant et déterministe.
    std::memset(vram_.get(), 0, kVramPixels * sizeof(uint16_t));
}

int Gpu::tick_vblank(uint32_t cycles)
{
    // Approximate VBlank pulse used to raise IRQ0.
    // A real PS1 uses GPU timings (NTSC/PAL); for bring-up we keep an explicit, deterministic cadence.
    vblank_div_ += cycles;
    if (vblank_div_ >= kVblankPeriodCycles)
    {
        vblank_div_ = 0;
        return 1;
    }
    return 0;
}

void Gpu::set_log_sinks(const flog::Sink& gpu_only, const flog::Sink& combined, const flog::Clock& clock)
{
    log_gpu_ = gpu_only;
    log_io_ = combined;
    clock_ = clock;
    has_clock_ = 1;

    // Petit marqueur: permet de vérifier rapidement que le logging GPU est bien branché,
    // même si le BIOS/jeu n'a pas encore touché les registres GPU.
    gpu_log(
        log_gpu_,
        log_io_,
        clock_,
        has_clock_,
        flog::Level::info,
        "log start (gpu_level=%u io_level=%u)",
        (unsigned)log_gpu_.level,
        (unsigned)log_io_.level
    );
}

void Gpu::set_dump_file(const char* path)
{
    if (dump_)
    {
        std::fclose(dump_);
        dump_ = nullptr;
    }
    if (path && *path)
    {
        dump_ = fopen_utf8(path, "wb");
    }
}

void Gpu::dump_u32(uint32_t port, uint32_t v)
{
    if (!dump_)
        return;
    // Format simple: [u32 port][u32 value] en little-endian.
    (void)std::fwrite(&port, 1, sizeof(port), dump_);
    (void)std::fwrite(&v, 1, sizeof(v), dump_);
    std::fflush(dump_);
}

uint32_t Gpu::mmio_read32(uint32_t addr)
{
    switch (addr)
    {
        case 0x1F80'1810u: // GPUREAD (rare)
        {
            uint32_t out = 0;
            if (vram_to_cpu_active_ && vram_w_ != 0 && vram_h_ != 0)
            {
                // Read two 16-bit pixels per word, wrap in VRAM.
                for (uint32_t i = 0; i < 2; ++i)
                {
                    const uint32_t x = (uint32_t)(vram_x_ + vram_col_) % kVramWidth;
                    const uint32_t y = (uint32_t)(vram_y_ + vram_row_) % kVramHeight;
                    const uint16_t px = vram_[y * kVramWidth + x];
                    out |= (uint32_t)px << (i * 16u);

                    // Advance (pixel granularity).
                    vram_col_++;
                    if (vram_col_ >= vram_w_)
                    {
                        vram_col_ = 0;
                        vram_row_++;
                        if (vram_row_ >= vram_h_)
                        {
                            // End of transfer.
                            vram_to_cpu_active_ = false;
                            vram_x_ = vram_y_ = 0;
                            vram_w_ = vram_h_ = 0;
                            vram_col_ = vram_row_ = 0;
                            break;
                        }
                    }
                }
            }

            gpu_log(log_gpu_, log_io_, clock_, has_clock_, flog::Level::trace, "MMIO R32 GPUREAD -> 0x%08X", out);
            return out;
        }
        case 0x1F80'1814u: // GPUSTAT
            {
                // GPUSTAT "vue" calculée:
                // - Bit26 Ready to receive Cmd Word.
                // - Bit28 Ready to receive DMA block.
                // - Bit27 Ready to send VRAM->CPU: set during an active VRAM->CPU transfer (GP0(C0h)).
                // - Bits29-30 reflect GP1(04h) DMA direction.
                // - Bit25 depends on DMA direction (psx-spx):
                //   dir=0 -> 0
                //   dir=1 -> FIFO not full
                //   dir=2 -> same as bit28
                //   dir=3 -> same as bit27
                uint32_t v = status_;
                const uint32_t bit26 = 1u << 26;
                const uint32_t bit28 = 1u << 28;
                const uint32_t bit27 = 1u << 27;

                const uint32_t ready_cmd = vram_to_cpu_active_ ? 0u : 1u;
                const uint32_t ready_dma = vram_to_cpu_active_ ? 0u : 1u;
                const uint32_t ready_vram_to_cpu = vram_to_cpu_active_ ? 1u : 0u;

                v &= ~(bit26 | bit28 | bit27);
                if (ready_cmd)
                    v |= bit26;
                if (ready_dma)
                    v |= bit28;
                if (ready_vram_to_cpu)
                    v |= bit27;

                v &= ~(3u << 29);
                v |= (dma_dir_ & 3u) << 29;

                v &= ~(1u << 25);
                if ((dma_dir_ & 3u) == 1u)
                {
                    // FIFO state: treat as "not full" when we can accept DMA.
                    if (ready_dma)
                        v |= (1u << 25);
                }
                else if ((dma_dir_ & 3u) == 2u)
                {
                    if (ready_dma)
                        v |= (1u << 25);
                }
                else if ((dma_dir_ & 3u) == 3u)
                {
                    if (ready_vram_to_cpu)
                        v |= (1u << 25);
                }

                gpu_log(log_gpu_, log_io_, clock_, has_clock_, flog::Level::trace, "MMIO R32 GPUSTAT -> 0x%08X", v);
                return v;
            }
    }
    return 0;
}

void Gpu::mmio_write32(uint32_t addr, uint32_t v)
{
    switch (addr)
    {
        case 0x1F80'1810u: // GP0
        {
            gpu_log(log_gpu_, log_io_, clock_, has_clock_, flog::Level::trace, "MMIO W32 GP0 = 0x%08X", v);
            dump_u32(0, v);

            // Minimal GP0 command decoding needed for BIOS bring-up:
            // - GP0(C0h): VRAM -> CPU blit, followed by reads from GPUREAD.
            if (gp0_pending_ != Gp0Pending::none && gp0_params_left_ > 0)
            {
                // Parameter word for pending command.
                if (gp0_pending_ == Gp0Pending::vram_to_cpu)
                {
                    if (gp0_params_left_ == 2)
                    {
                        // Source coord (YyyyXxxxh)
                        vram_x_ = (uint16_t)(v & 0x03FFu);
                        vram_y_ = (uint16_t)((v >> 16) & 0x01FFu);
                    }
                    else if (gp0_params_left_ == 1)
                    {
                        // Size (YsizXsizh) with COPY masking rules.
                        const uint32_t xs = v & 0xFFFFu;
                        const uint32_t ys = (v >> 16) & 0xFFFFu;
                        const uint32_t w = (xs == 0u) ? 0x400u : (((xs - 1u) & 0x3FFu) + 1u);
                        const uint32_t h = (ys == 0u) ? 0x200u : (((ys - 1u) & 0x1FFu) + 1u);

                        vram_w_ = (uint16_t)w;
                        vram_h_ = (uint16_t)h;
                        vram_col_ = 0;
                        vram_row_ = 0;
                        vram_to_cpu_active_ = true;
                    }

                    gp0_params_left_--;
                    if (gp0_params_left_ == 0)
                    {
                        gp0_pending_ = Gp0Pending::none;
                    }
                }
                break;
            }

            // New command word.
            const uint8_t cmd = (uint8_t)(v >> 24);
            if (cmd == (uint8_t)Gp0Pending::vram_to_cpu)
            {
                // Start VRAM->CPU transfer: expects 2 parameter words.
                gp0_pending_ = Gp0Pending::vram_to_cpu;
                gp0_params_left_ = 2;
            }
            break;
        }
        case 0x1F80'1814u: // GP1
        {
            gpu_log(log_gpu_, log_io_, clock_, has_clock_, flog::Level::trace, "MMIO W32 GP1 = 0x%08X", v);
            dump_u32(1, v);
            // Quelques commandes GP1 utiles au BIOS
            const uint8_t cmd = (uint8_t)(v >> 24);
            if (cmd == 0x00)
            {
                // Reset GPU
                status_ = 0x1480'2000u;
                dma_dir_ = 0;
                gp0_pending_ = Gp0Pending::none;
                gp0_params_left_ = 0;
                vram_to_cpu_active_ = false;
                vram_x_ = vram_y_ = 0;
                vram_w_ = vram_h_ = 0;
                vram_col_ = vram_row_ = 0;
            }
            else if (cmd == 0x01)
            {
                // Clear FIFO: no-op
            }
            else if (cmd == 0x02)
            {
                // Ack IRQ1 (GPUSTAT.24)
                status_ &= ~(1u << 24);
            }
            else if (cmd == 0x03)
            {
                // Display enable/disable (bit0)
                const uint32_t off = v & 1u;
                if (off)
                    status_ |= (1u << 23);
                else
                    status_ &= ~(1u << 23);
            }
            else if (cmd == 0x04)
            {
                // DMA direction
                dma_dir_ = v & 3u;
            }
            else if (cmd == 0x05)
            {
                // Start of display area
            }
            else if (cmd == 0x06)
            {
                // Horizontal display range
            }
            else if (cmd == 0x07)
            {
                // Vertical display range
            }

            (void)cmd;
            break;
        }
    }
}

} // namespace gpu

