#include "bus.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

#include "../audio/spu.h"
#include "../audio/wav_writer.h"
#include "../cdrom/cdrom.h"
#include "../emu/hooks.h"
#include "../gpu/gpu.h"
#include "../gpu/gpu_3d.h"
#include "../gte/gte_3d.h"
#include "../log/emu_log.h"

// ---- Global pad button state (avoids Hot Reload class-layout issues) ----
static std::atomic<uint16_t> g_pad_buttons{0xFFFFu};

namespace r3000
{

void Bus::set_pad_buttons(uint16_t v) { g_pad_buttons.store(v, std::memory_order_relaxed); }
uint16_t Bus::pad_buttons() const    { return g_pad_buttons.load(std::memory_order_relaxed); }

// Debug: returns address of the global pad storage — call from both threads to verify same addr
const void* Bus::pad_buttons_addr() { return (const void*)&g_pad_buttons; }

// Forward declaration for use in CDROM IRQ callback.
static void deliver_events_for_class(uint8_t* ram, uint32_t ram_size, uint32_t cls_match);

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
    // Version marker - update when making changes!
    emu::logf(emu::LogLevel::warn, "BUS", "BUS source v26 (face_token_flow)");

    // Initialize EXP1 region to 0xFF (open bus)
    std::memset(exp1_, 0xFF, sizeof(exp1_));
    ram_face_tokens_.assign((ram_size_ + 3u) / 4u, kNoFaceToken);
    ram_face_writer_pc_.assign((ram_size_ + 3u) / 4u, 0u);

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

    // Set up CDROM IRQ callback: push-model notification like DuckStation.
    // When CDROM IRQ line changes, immediately update I_STAT bit 2.
    if (cdrom_)
    {
        cdrom_->set_irq_callback([](int irq_state, void* user) {
            Bus* bus = static_cast<Bus*>(user);
            // Edge detection: only latch on rising edge (0->1).
            // This matches real PS1 behavior where I_STAT latches on rising edge
            // and is only cleared by writing to I_STAT.
            if (irq_state && !bus->cdrom_irq_prev_)
            {
                bus->i_stat_ |= (1u << 2);  // CDROM IRQ = bit 2
                emu::logf(emu::LogLevel::info, "BUS", "CDROM IRQ push: i_stat=0x%04X", (unsigned)bus->i_stat_);
                // BIOS event system: mark CDROM class events as ready.
                deliver_events_for_class(bus->ram_, bus->ram_size_, 0x28u);
            }
            bus->cdrom_irq_prev_ = (uint8_t)irq_state;
        }, this);
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

void Bus::set_ram_face_token(uint32_t paddr, uint32_t token)
{
    if (ram_face_tokens_.empty() || ram_size_ == 0)
        return;
    const uint32_t phys = paddr & (ram_size_ - 1u);
    const size_t idx = (phys >> 2) % ram_face_tokens_.size();
    ram_face_tokens_[idx] = token;
    if (!ram_face_writer_pc_.empty())
        ram_face_writer_pc_[idx] = cpu_pc_;
}

uint32_t Bus::ram_face_token(uint32_t paddr) const
{
    if (ram_face_tokens_.empty() || ram_size_ == 0)
        return kNoFaceToken;
    const uint32_t phys = paddr & (ram_size_ - 1u);
    return ram_face_tokens_[(phys >> 2) % ram_face_tokens_.size()];
}

uint32_t Bus::ram_face_writer_pc(uint32_t paddr) const
{
    if (ram_face_writer_pc_.empty() || ram_size_ == 0)
        return 0u;
    const uint32_t phys = paddr & (ram_size_ - 1u);
    return ram_face_writer_pc_[(phys >> 2) % ram_face_writer_pc_.size()];
}

bool Bus::consume_dma2_nohint_summary(Dma2NoHintSummary& out)
{
    if (!dma2_nohint_last_valid_)
        return false;
    out = dma2_nohint_last_;
    dma2_nohint_last_valid_ = false;
    return true;
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

// ------------------ Timers (DuckStation-accurate) ------------------

void Bus::timer_write_mode(int ch, uint16_t v)
{
    Timer& t = timers_[ch];

    // PSX-SPX: writing mode register:
    // - bits 0-9 are writable
    // - bit 10 (interrupt_request_n) is NOT writable, preserved
    // - bits 11-12 (reached flags) are NOT writable, preserved
    // DuckStation WRITE_MASK = 0b1110001111111111 = 0xE3FF
    // (preserves bits 10, 11, but bit 10 behavior: DuckStation doesn't force it)
    // However, per PSX-SPX: "Set after Writing" means bit 10 → 1 on write.
    // DuckStation's mask preserves bit 10 but then resets irq_done=false + clears IRQ line.
    t.mode = (v & 0x03FFu) | 0x0400u; // bits 0-9 from write, force bit 10 to 1 (no IRQ)
    t.count = 0;
    t.irq_done = false;

    // Determine clock source
    const uint8_t clk_src = (v >> 8) & 3u;
    if (ch == 2)
        t.use_external_clock = (clk_src & 2) != 0; // bit 9 selects sysclk/8 for timer 2
    else
        t.use_external_clock = (clk_src & 1) != 0; // bit 8 selects dotclock/hblank for timer 0/1

    // Clear IRQ line for this timer
    // (DuckStation: SetLineState(TMRn, false))
    // We don't have per-line state, but clearing i_stat is wrong here.
    // Just reset the timer's IRQ contribution.

    timer_update_counting(ch);
}

void Bus::timer_update_counting(int ch)
{
    Timer& t = timers_[ch];
    const bool sync_enable = (t.mode & 0x0001u) != 0;

    if (sync_enable)
    {
        const uint8_t sync_mode = (t.mode >> 1) & 3u;
        if (ch == 2)
        {
            // Timer 2 sync modes: 0 or 3 = stop counter, 1 or 2 = free run
            t.counting_enabled = (sync_mode == 1 || sync_mode == 2);
        }
        else
        {
            // Timer 0/1 sync modes (gate = HBlank for T0, VBlank for T1):
            // 0 = pause while gate active
            // 1 = reset on gate end (counting always enabled)
            // 2 = reset+run on gate start, pause outside gate
            // 3 = free run after gate end once
            switch (sync_mode)
            {
                case 0: t.counting_enabled = !t.gate; break;
                case 1: t.counting_enabled = true; break;
                case 2: t.counting_enabled = t.gate; break;
                case 3: t.counting_enabled = true; break; // simplified: always count
            }
        }
    }
    else
    {
        t.counting_enabled = true; // free run
    }
}

void Bus::timer_check_irq(int ch, uint32_t old_count)
{
    Timer& t = timers_[ch];
    bool irq_request = false;

    // Check target hit
    if (t.count >= t.target && (old_count < t.target || t.target == 0))
    {
        if (t.mode & 0x0010u) // irq_at_target
            irq_request = true;
        t.mode |= 0x0800u; // reached_target flag (bit 11)

        if ((t.mode & 0x0008u) && t.target > 0) // reset_at_target
            t.count %= t.target;
    }

    // Check overflow
    if (t.count >= 0xFFFFu)
    {
        if (t.mode & 0x0020u) // irq_on_overflow
            irq_request = true;
        t.mode |= 0x1000u; // reached_overflow flag (bit 12)
        t.count &= 0xFFFFu;
    }

    if (!irq_request) return;

    // IRQ generation with proper one-shot/repeat and pulse/toggle (DuckStation logic)
    const bool irq_pulse_n = (t.mode & 0x0080u) != 0; // bit 7: 0=pulse, 1=toggle
    const bool irq_repeat  = (t.mode & 0x0040u) != 0;  // bit 6: 0=one-shot, 1=repeat

    if (!irq_pulse_n)
    {
        // Pulse mode: brief IRQ pulse
        if (!t.irq_done || irq_repeat)
        {
            i_stat_ |= (1u << (4 + ch)); // fire IRQ
        }
        t.irq_done = true;
        t.mode |= 0x0400u; // interrupt_request_n = 1 (no IRQ pending)
    }
    else
    {
        // Toggle mode: XOR bit 10, fire only when bit 10 becomes 0
        t.mode ^= 0x0400u; // toggle interrupt_request_n
        if (!(t.mode & 0x0400u))
        {
            // bit 10 is now 0 → fire IRQ
            i_stat_ |= (1u << (4 + ch));
        }
    }
}

// ------------------ SIO0 (minimal controller) ------------------
uint16_t Bus::sio0_stat_value() const
{
    // Base: TX Ready 1 (bit 0) | TX Ready 2 (bit 2)
    uint16_t stat = (uint16_t)(sio0_stat_ | 0x0005u);
    // Bit 7: /ACK input level. HIGH (1) = idle, LOW (0) = ACK asserted.
    // When sio0_ack_countdown_ > 0, ACK is asserted (LOW/0).
    // When countdown is 0, ACK is idle (HIGH/1).
    if (sio0_ack_countdown_ == 0)
        stat |= 0x0080u; // /ACK HIGH (idle)
    // else: /ACK LOW (asserted) - bit 7 stays 0

    if (sio0_rx_ready_)
        stat |= 0x0002u; // RXRDY (bit 1)
    if (sio0_irq_flag_)
        stat |= 0x0200u; // IRQ flag (bit 9)
    return stat;
}

uint16_t Bus::sio0_stat_debug() const { return sio0_stat_value(); }

uint16_t Bus::sio0_read_data()
{
    uint16_t v = sio0_rx_ready_ ? (uint16_t)sio0_rx_data_ : 0x00FFu;
    const uint8_t phase = sio0_tx_phase_;
    sio0_rx_ready_ = 0;

    // Log what the game/BIOS actually reads back (first 50 reads with buttons pressed)
    {
        static uint32_t rd_log = 0;
        const uint16_t btns = pad_buttons();
        if (btns != 0xFFFFu && rd_log < 50)
        {
            ++rd_log;
            emu::logf(emu::LogLevel::warn, "BUS",
                "SIO0 READ data=0x%02X phase=%u rxrdy=%u btns=0x%04X (#%u)",
                v, phase, (sio0_rx_ready_ ? 1u : 0u), btns, rd_log);
        }
    }

    return v;
}

void Bus::sio0_write_ctrl(uint16_t v)
{
    sio0_ctrl_ = v;

    // Bit 6 (0x0040) = Reset: soft-reset SIO (like DuckStation SoftReset)
    if (v & 0x0040u)
    {
        if (sio0_state_ != Sio0State::Idle)
            sio0_end_transfer();
        sio0_tx_phase_ = 0u;
        sio0_rx_ready_ = 0;
        sio0_irq_flag_ = 0;
        sio0_rx_data_  = 0xFFu;
        sio0_stat_     = 0x0005u; // TX Ready flags
        sio0_mode_     = 0;
        sio0_baud_     = 0;
        sio0_ctrl_     = 0;       // reset clears itself
        sio0_ack_countdown_ = 0;
        sio0_transfer_countdown_ = 0;
        sio0_tx_buf_full_ = 0;
        sio0_tx_value_ = 0;
    }

    // Bit 4 (0x0010) = Acknowledge: clears STAT IRQ flag (bit 9) AND I_STAT bit 7.
    // This matches DuckStation: JOY_CTRL ACK clears INTR + SetLineState(false).
    if (v & 0x0010u)
    {
        sio0_irq_flag_ = 0;
        i_stat_ &= ~(1u << 7); // clear I_STAT SIO0 bit (like DuckStation SetLineState false)
    }

    // Bit 1 (0x0002) = SELECT: if deasserted, reset device transfer state
    if (!(v & 0x0002u))
    {
        sio0_tx_phase_ = 0u; // reset protocol phase (device deselected)
    }

    // If SELECT=0 or TXEN=0, abort any in-progress transfer
    if (!(v & 0x0002u) || !(v & 0x0001u))
    {
        if (sio0_state_ != Sio0State::Idle)
            sio0_end_transfer();
    }
    else
    {
        // SELECT=1 and TXEN=1: if we have data buffered, start transfer
        if (sio0_state_ == Sio0State::Idle && sio0_can_transfer())
            sio0_begin_transfer();
    }
}

// DuckStation-style: check if we can start a transfer
bool Bus::sio0_can_transfer() const
{
    return sio0_tx_buf_full_ &&
           (sio0_ctrl_ & 0x0002u) && // SELECT
           (sio0_ctrl_ & 0x0001u);   // TXEN
}

// Start a byte transfer: move data from TX buffer, schedule transfer event
void Bus::sio0_begin_transfer()
{
    sio0_tx_value_ = sio0_tx_buf_full_ ? sio0_data_ & 0xFF : 0xFF;
    sio0_tx_buf_full_ = 0;
    sio0_state_ = Sio0State::Transmitting;

    // Transfer time = BAUD * 8 ticks (DuckStation: GetTransferTicks)
    // BAUD is typically 0x0088 (136) for controllers → 136*8 = 1088 ticks
    // Minimum: use 200 ticks if BAUD=0 (safety)
    uint32_t xfer_ticks = (uint32_t)sio0_baud_ * 8u;
    if (xfer_ticks < 200u) xfer_ticks = 200u;
    sio0_transfer_countdown_ = xfer_ticks;
}

// Transfer complete: compute response, set RX buffer, schedule ACK
void Bus::sio0_do_transfer()
{
    const uint8_t v = sio0_tx_value_;
    uint8_t resp = 0xFFu;
    const uint32_t prev_phase = sio0_tx_phase_;

    switch (sio0_tx_phase_)
    {
        case 0:
            resp = 0xFFu;
            if (v == 0x01u)
                sio0_tx_phase_ = 1u;
            break;
        case 1:
            (void)v;
            resp = 0x41u; // digital pad ID
            sio0_tx_phase_ = 2u;
            break;
        case 2:
            resp = 0x5Au; // access byte
            sio0_tx_phase_ = 3u;
            break;
        case 3:
        {
            const uint16_t btns = pad_buttons();
            resp = (uint8_t)(btns & 0xFF);
            sio0_tx_phase_ = 4u;
            break;
        }
        case 4:
        {
            const uint16_t btns = pad_buttons();
            resp = (uint8_t)(btns >> 8);
            sio0_tx_phase_ = 0u;
            break;
        }
        default:
            resp = 0xFFu;
            sio0_tx_phase_ = 0u;
            break;
    }

    // Put response in RX buffer
    sio0_rx_data_ = resp;
    sio0_rx_ready_ = 1;

    // RXINTEN: trigger IRQ when RX data arrives (CTRL bit 11, 0x0800)
    if (sio0_ctrl_ & 0x0800u)
    {
        sio0_irq_flag_ = 1;
        i_stat_ |= (1u << 7); // SIO0 IRQ
    }

    // Does the device ACK this byte? All bytes except the last one.
    bool ack = (sio0_tx_phase_ != 0u);

    if (!ack)
    {
        // No ACK: transfer ends here
        sio0_end_transfer();

        // Deliver SIO0 event when full pad transfer completes (phase 4→0)
        if (prev_phase == 4u)
        {
            deliver_events_for_class(ram_, ram_size_, 0xF000'0009u);
        }
    }
    else
    {
        // Schedule ACK delay: 450 ticks for controllers (DuckStation)
        sio0_state_ = Sio0State::WaitingForACK;
        sio0_ack_countdown_ = 450;
    }

    // Trace SIO0 transfers
    {
        static uint32_t sio0_xfer_count = 0;
        static uint32_t sio0_pressed_log = 0;
        const uint16_t btns = pad_buttons();

        if (prev_phase == 0 && sio0_tx_phase_ == 1u)
        {
            ++sio0_xfer_count;
            if (sio0_xfer_count <= 20 || (sio0_xfer_count % 100 == 0))
            {
                emu::logf(emu::LogLevel::info, "BUS",
                    "SIO0 xfer #%u START: btns=0x%04X baud=%u",
                    sio0_xfer_count, btns, (unsigned)sio0_baud_);
            }
        }

        if (btns != 0xFFFFu && prev_phase == 4u && sio0_pressed_log < 20)
        {
            ++sio0_pressed_log;
            emu::logf(emu::LogLevel::warn, "BUS",
                "SIO0 xfer COMPLETE: btns=0x%04X lo=0x%02X hi=0x%02X (#%u)",
                btns, (unsigned)(btns & 0xFF), (unsigned)(btns >> 8), sio0_pressed_log);
        }
    }
}

// ACK pulse completed: set IRQ flag, trigger I_STAT if ACKINTEN
void Bus::sio0_do_ack()
{
    // ACKINTEN: CTRL bit 12 (0x1000)
    sio0_irq_flag_ = 1;
    if (sio0_ctrl_ & 0x1000u)
    {
        i_stat_ |= (1u << 7); // SIO0 IRQ via ACK
    }

    sio0_end_transfer();

    // If TX buffer has more data, start next transfer automatically (pipeline)
    if (sio0_can_transfer())
        sio0_begin_transfer();
}

void Bus::sio0_end_transfer()
{
    sio0_state_ = Sio0State::Idle;
    sio0_transfer_countdown_ = 0;
    sio0_ack_countdown_ = 0;
}

// DuckStation-style: write to JOY_DATA buffers the byte, starts transfer if ready
void Bus::sio0_write_data(uint8_t v)
{
    if (sio0_tx_buf_full_)
    {
        static uint32_t overrun_log = 0;
        if (overrun_log++ < 5)
            emu::logf(emu::LogLevel::warn, "BUS", "SIO0 TX FIFO overrun (v=0x%02X)", v);
    }

    sio0_data_ = v;
    sio0_tx_buf_full_ = 1;

    // TXINTEN: trigger IRQ on TX write (CTRL bit 10, 0x0400)
    if (sio0_ctrl_ & 0x0400u)
    {
        sio0_irq_flag_ = 1;
        i_stat_ |= (1u << 7);
    }

    // If idle and conditions met, start transfer (delayed)
    if (sio0_state_ == Sio0State::Idle && sio0_can_transfer())
        sio0_begin_transfer();
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
            case 0: out = (uint16_t)(timers_[ch].count & 0xFFFFu); break;
            case 1:
                // Reading mode register returns current value then clears bits 11-12
                // (target reached and overflow flags). This is PS1 hardware behavior.
                out = timers_[ch].mode;
                timers_[ch].mode &= ~0x1800u; // Clear bits 11 (reached_target) and 12 (reached_overflow)
                break;
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
        set_ram_face_token(mp, kNoFaceToken);

        // Fire write hooks (zero-cost when no hooks registered)
        if (hooks_ && hooks_->has_write())
        {
            if (hooks_->on_write_has_wildcard || hooks_->watches_addr(mp))
                hooks_->fire_write(mp, (uint32_t)v, 1);
        }
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
        uint32_t old_mask = i_mask_;
        i_mask_ = (i_mask_ & ~((uint32_t)0xFF << shift)) | ((uint32_t)v << shift);
        if (old_mask != i_mask_)
        {
            emu::logf(emu::LogLevel::info, "IRQ", "I_MASK byte write: 0x%04X -> 0x%04X (off=%u val=0x%02X)",
                (unsigned)old_mask, (unsigned)i_mask_, (unsigned)byte_off, (unsigned)v);
            // CRITICAL: Log when VBlank (bit 0) is disabled
            if ((old_mask & 0x01) && !(i_mask_ & 0x01))
            {
                {
                static uint32_t imask_log = 0;
                if (imask_log < 3)
                {
                    ++imask_log;
                    emu::logf(emu::LogLevel::warn, "BUS", "I_MASK VBlank off (byte): 0x%04X -> 0x%04X (#%u)",
                        (unsigned)old_mask, (unsigned)i_mask_, imask_log);
                }
            }
            }
        }
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
                sio0_write_ctrl((uint16_t)((sio0_ctrl_ & 0xFF00u) | v));
                break;
            case 0xB:
                sio0_write_ctrl((uint16_t)((sio0_ctrl_ & 0x00FFu) | ((uint16_t)v << 8)));
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
        set_ram_face_token(mp0, kNoFaceToken);

        // Fire write hooks (zero-cost when no hooks registered)
        if (hooks_ && hooks_->has_write())
        {
            if (hooks_->on_write_has_wildcard || hooks_->watches_addr(mp0))
                hooks_->fire_write(mp0, (uint32_t)v, 2);
        }
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
        uint32_t old_mask = i_mask_;
        i_mask_ = v;
        if (old_mask != i_mask_)
        {
            emu::logf(emu::LogLevel::info, "IRQ", "I_MASK word write: 0x%04X -> 0x%04X",
                (unsigned)old_mask, (unsigned)i_mask_);
            // CRITICAL: Log when VBlank (bit 0) is disabled - this causes VSync timeout!
            if ((old_mask & 0x01) && !(i_mask_ & 0x01))
            {
                {
                static uint32_t imask_log2 = 0;
                if (imask_log2 < 3)
                {
                    ++imask_log2;
                    emu::logf(emu::LogLevel::warn, "BUS", "I_MASK VBlank off (word): 0x%04X -> 0x%04X (#%u)",
                        (unsigned)old_mask, (unsigned)i_mask_, imask_log2);
                }
            }
            }
        }
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
            case 0xA: sio0_write_ctrl(v); break;  // CTRL
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
            case 0: timers_[ch].count = (uint16_t)v; break;
            case 1: timer_write_mode(ch, (uint16_t)v); break;
            case 2: timers_[ch].target = (uint16_t)v; break;
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
        set_ram_face_token(mp0, kNoFaceToken);

        // Fire write hooks (zero-cost when no hooks registered)
        if (hooks_ && hooks_->has_write())
        {
            if (hooks_->on_write_has_wildcard || hooks_->watches_addr(mp0))
                hooks_->fire_write(mp0, v, 4);
        }
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
        uint32_t old_mask = i_mask_;
        i_mask_ = v;
        if (old_mask != i_mask_)
        {
            emu::logf(emu::LogLevel::info, "IRQ", "I_MASK hw write: 0x%04X -> 0x%04X",
                (unsigned)old_mask, (unsigned)i_mask_);
        }
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

                        emu::logf(emu::LogLevel::info, "BUS", "DMA2 GPU dir=%d mode=%d madr=0x%08X bcr=0x%08X",
                            dir, mode, dma_[ch].madr, dma_[ch].bcr);

                        if (dir == 0) // From GPU (VRAM→RAM via GPUREAD)
                        {
                            if (mode == 0 || mode == 1) // Burst or Block
                            {
                                const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                                const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                                const uint32_t words = (mode == 0) ? bs : bs * bc;
                                uint32_t ma = dma_[ch].madr & 0x1FFFFF;
                                for (uint32_t i = 0; i < words; ++i)
                                {
                                    const uint32_t w = gpu_->mmio_read32(kGpuBase);
                                    ram_[ma]     = (uint8_t)(w);
                                    ram_[ma + 1] = (uint8_t)(w >> 8);
                                    ram_[ma + 2] = (uint8_t)(w >> 16);
                                    ram_[ma + 3] = (uint8_t)(w >> 24);
                                    ma = (ma + 4) & 0x1FFFFF;
                                }
                                emu::logf(emu::LogLevel::info, "BUS", "DMA2 GPU→RAM: %u words to 0x%05X",
                                    words, dma_[ch].madr & 0x1FFFFF);
                            }
                        }
                        else // dir == 1: To GPU
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
                                    const uint32_t tok = ram_face_token(ma);
                                    if (tok == kNoFaceToken)
                                    {
                                        ++dma2_nohint_words_;
                                        ++dma2_nohint_pc_hist_[ram_face_writer_pc(ma)];
                                    }
                                    if (gpu_3d_) gpu_3d_->gp0_with_face_hint(w, tok);
                                    ma = (ma + 4) & 0x1FFFFF;
                                }
                            }
                            else if (mode == 2) // Linked list
                            {
                                uint32_t node = dma_[ch].madr & 0x1FFFFF;
                                uint32_t start_node = node;
                                uint32_t total_ll_words = 0;
                                uint32_t ll_nodes = 0;
                                uint32_t first_header = 0;
                                uint32_t second_header = 0;
                                bool hit_safety = false;
                                // OT Z tracking: empty nodes (0 data words) are OT
                                // entry boundaries. Count them to determine depth level.
                                uint32_t ot_z = 0;
                                for (int safety = 0; safety < 0x100000; ++safety)
                                {
                                    uint32_t header = (uint32_t)ram_[node] |
                                                      ((uint32_t)ram_[node + 1] << 8) |
                                                      ((uint32_t)ram_[node + 2] << 16) |
                                                      ((uint32_t)ram_[node + 3] << 24);
                                    if (ll_nodes == 0) first_header = header;
                                    if (ll_nodes == 1) second_header = header;
                                    uint32_t words = header >> 24;
                                    total_ll_words += words;
                                    ll_nodes++;
                                    if (words == 0)
                                    {
                                        // Empty OT entry = Z boundary
                                        ++ot_z;
                                    }
                                    else
                                    {
                                        // Data node: tell shadow GPU the current OT depth
                                        if (gpu_3d_) gpu_3d_->set_ot_z(ot_z);
                                    }
                                    for (uint32_t i = 0; i < words; ++i)
                                    {
                                        uint32_t off2 = (node + 4 + i * 4) & 0x1FFFFF;
                                        uint32_t w = (uint32_t)ram_[off2] |
                                                     ((uint32_t)ram_[off2 + 1] << 8) |
                                                     ((uint32_t)ram_[off2 + 2] << 16) |
                                                     ((uint32_t)ram_[off2 + 3] << 24);
                                        gpu_->mmio_write32(kGpuBase, w);
                                        const uint32_t tok = ram_face_token(off2);
                                        if (tok == kNoFaceToken)
                                        {
                                            ++dma2_nohint_words_;
                                            ++dma2_nohint_pc_hist_[ram_face_writer_pc(off2)];
                                        }
                                        if (gpu_3d_) gpu_3d_->gp0_with_face_hint(w, tok);
                                    }
                                    if ((header & 0x00FFFFFF) == 0x00FFFFFF)
                                        break;
                                    node = header & 0x1FFFFF;
                                    if (safety == 0x100000 - 1) hit_safety = true;
                                }
                                emu::logf(emu::LogLevel::info, "BUS", "DMA2 LL: start=0x%05X nodes=%u words=%u hdr0=0x%08X hdr1=0x%08X %s",
                                    start_node, ll_nodes, total_ll_words, first_header, second_header, hit_safety ? "SAFETY" : "");
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

                        emu::logf(emu::LogLevel::info, "BUS", "DMA6 OTC: madr=0x%08X bcr=0x%08X words=%u (OT tail at 0x%05X)",
                            dma_[ch].madr, dma_[ch].bcr, words, ma);

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
        // Forward to shadow GPU for 3D reconstruction (tag decoding)
        if (gpu_3d_)
        {
            if (phys == kGpuBase)
                gpu_3d_->gp0_with_face_hint(v, kNoFaceToken);
            else
                gpu_3d_->gp1(v);
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
            case 0: timers_[ch].count = (uint16_t)v; break;
            case 1: timer_write_mode(ch, (uint16_t)v); break;
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
    const int old_master_flag = (dicr_ >> 31) & 1;
    if (master_en && (force || (flags & enables)))
        dicr_ |= (1u << 31);
    else
        dicr_ &= ~(1u << 31);
    const int new_master_flag = (dicr_ >> 31) & 1;

    // Raise DMA IRQ (I_STAT bit 3) when DICR master flag is set.
    // The BIOS uses DMA IRQs during CD boot (e.g., sector transfers).
    if (dicr_ & (1u << 31))
        i_stat_ |= (1u << 3);

    // Debug: log DMA completion state for channels 3 and 4 (CDROM/SPU)
    if (ch == 3 || ch == 4)
    {
        emu::logf(emu::LogLevel::debug, "BUS",
            "DMA%d finish: DICR=0x%08X flags=0x%02X en=0x%02X master_en=%d force=%d flag_set=%d irq_fired=%d",
            ch, dicr_, flags, enables, master_en, force, new_master_flag, (dicr_ & (1u << 31)) ? 1 : 0);
    }
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

    // Tick timers (DuckStation-style: counting_enabled + proper IRQ logic)
    for (int ch = 0; ch < 3; ++ch)
    {
        Timer& t = timers_[ch];
        if (!t.counting_enabled) continue;

        uint32_t inc = cycles;

        // Prescaler for external clock sources
        if (t.use_external_clock)
        {
            if (ch == 0)
            {
                // Dotclock: ~8 CPU cycles per dot (320px mode)
                timer_prescale_accum_[0] += cycles;
                inc = timer_prescale_accum_[0] / 8;
                timer_prescale_accum_[0] %= 8;
            }
            else if (ch == 1)
            {
                // HBlank: ~2150 CPU cycles per HBlank line
                timer_prescale_accum_[1] += cycles;
                inc = timer_prescale_accum_[1] / 2150;
                timer_prescale_accum_[1] %= 2150;
            }
            else // ch == 2
            {
                // Sysclock/8
                timer_prescale_accum_[2] += cycles;
                inc = timer_prescale_accum_[2] / 8;
                timer_prescale_accum_[2] %= 8;
            }
        }

        if (inc == 0) continue;

        const uint32_t old_count = t.count;
        t.count += inc;
        timer_check_irq(ch, old_count);
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

    // Tick SIO0 state machine (DuckStation-style delayed transfers)
    if (sio0_state_ == Sio0State::Transmitting && sio0_transfer_countdown_ > 0)
    {
        if (cycles >= sio0_transfer_countdown_)
        {
            sio0_transfer_countdown_ = 0;
            sio0_do_transfer(); // execute the byte transfer
        }
        else
        {
            sio0_transfer_countdown_ -= cycles;
        }
    }
    else if (sio0_state_ == Sio0State::WaitingForACK && sio0_ack_countdown_ > 0)
    {
        if (cycles >= sio0_ack_countdown_)
        {
            sio0_ack_countdown_ = 0;
            sio0_do_ack(); // ACK pulse ended
        }
        else
        {
            sio0_ack_countdown_ -= cycles;
        }
    }

    // Tick GPU VBlank
    if (gpu_)
    {
        if (gpu_->tick_vblank(cycles))
        {
            i_stat_ |= (1u << 0); // VBlank IRQ (bit 0)
            ++vblank_total_count_;

            // Shadow 3D systems: swap buffers at VBlank
            if (gte_3d_) gte_3d_->swap_frame();
            if (gpu_3d_) gpu_3d_->on_vblank();

            // Fire VBlank hooks (zero-cost when no hooks registered)
            if (hooks_ && hooks_->has_vblank())
                hooks_->fire_vblank(vblank_total_count_);

            // DMA2 no-token diagnostics: build per-vblank summary and optionally log top PCs.
            if (dma2_nohint_words_ > 0)
            {
                std::vector<std::pair<uint32_t, uint32_t>> top;
                top.reserve(dma2_nohint_pc_hist_.size());
                for (const auto& kv : dma2_nohint_pc_hist_)
                    top.emplace_back(kv.first, kv.second);
                std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) {
                    return a.second > b.second;
                });

                dma2_nohint_last_.vblank = vblank_total_count_;
                dma2_nohint_last_.nohint_words = dma2_nohint_words_;
                dma2_nohint_last_.top_pcs.clear();
                const size_t keep = (top.size() < 16) ? top.size() : 16;
                for (size_t i = 0; i < keep; ++i)
                    dma2_nohint_last_.top_pcs.push_back(top[i]);
                dma2_nohint_last_valid_ = true;

                if (vblank_total_count_ < 10 || (vblank_total_count_ % 60) == 0)
                {
                const size_t n = (top.size() < 6) ? top.size() : 6;
                emu::logf(
                    emu::LogLevel::warn,
                    "DMA2_NOHINT",
                    "vblank=%u nohint_words=%u unique_pcs=%u",
                    vblank_total_count_,
                    dma2_nohint_words_,
                    (uint32_t)top.size());
                for (size_t i = 0; i < n; ++i)
                {
                    emu::logf(
                        emu::LogLevel::warn,
                        "DMA2_NOHINT",
                        "  top[%u] pc=0x%08X words=%u",
                        (uint32_t)i,
                        top[i].first,
                        top[i].second);
                }
                }
            }
            dma2_nohint_words_ = 0;
            dma2_nohint_pc_hist_.clear();

            // ===== VBlank STUCK DETECTION =====
            // Check if GPU is making real frame progress (submitting primitives)
            // Use prev_frame_stats() which was saved BEFORE the reset
            const auto& stats = gpu_->prev_frame_stats();
            const uint32_t real_prims = stats.triangles + stats.quads + stats.rects + stats.lines + stats.fills;

            if (real_prims > 0)
            {
                // Real frame progress - reset stuck counter
                vblank_last_frame_ = vblank_total_count_;
                vblank_stuck_count_ = 0;
                vblank_stuck_logged_ = 0;  // Reset so we can log again if it happens later
            }
            else
            {
                // No primitives - check if we're stuck
                vblank_stuck_count_++;

                // If stuck for 100 VBlanks (~2 seconds) and not yet logged
                if (vblank_stuck_count_ >= 100 && !vblank_stuck_logged_)
                {
                    vblank_stuck_logged_ = 1;

                    // Read kernel event table for diagnosis
                    const uint32_t evt_ptr_off = 0x0120 & (ram_size_ - 1);
                    const uint32_t evt_ptr = (uint32_t)ram_[evt_ptr_off] |
                                             ((uint32_t)ram_[evt_ptr_off + 1] << 8) |
                                             ((uint32_t)ram_[evt_ptr_off + 2] << 16) |
                                             ((uint32_t)ram_[evt_ptr_off + 3] << 24);

                    // Read SysEnqIntRP chain pointers (priority 0-3)
                    uint32_t chain_ptrs[4] = {0};
                    for (int i = 0; i < 4; ++i)
                    {
                        const uint32_t chain_off = (0x0100 + i * 4) & (ram_size_ - 1);
                        chain_ptrs[i] = (uint32_t)ram_[chain_off] |
                                        ((uint32_t)ram_[chain_off + 1] << 8) |
                                        ((uint32_t)ram_[chain_off + 2] << 16) |
                                        ((uint32_t)ram_[chain_off + 3] << 24);
                    }

                    // Read COP0 Status from TCB if available
                    const uint32_t pcb_ptr_off = 0x0108 & (ram_size_ - 1);
                    const uint32_t pcb_ptr = (uint32_t)ram_[pcb_ptr_off] |
                                             ((uint32_t)ram_[pcb_ptr_off + 1] << 8) |
                                             ((uint32_t)ram_[pcb_ptr_off + 2] << 16) |
                                             ((uint32_t)ram_[pcb_ptr_off + 3] << 24);
                    uint32_t tcb_ptr = 0;
                    if (pcb_ptr != 0 && (pcb_ptr & (ram_size_ - 1)) + 4 <= ram_size_)
                    {
                        const uint32_t pcb_off = pcb_ptr & (ram_size_ - 1);
                        tcb_ptr = (uint32_t)ram_[pcb_off] |
                                  ((uint32_t)ram_[pcb_off + 1] << 8) |
                                  ((uint32_t)ram_[pcb_off + 2] << 16) |
                                  ((uint32_t)ram_[pcb_off + 3] << 24);
                    }

                    // Log comprehensive state dump
                    emu::logf(emu::LogLevel::warn, "BUS", "===== VSYNC STUCK DETECTED =====");
                    emu::logf(emu::LogLevel::warn, "BUS", "VBlank #%u: stuck for %u VBlanks (no primitives)",
                        vblank_total_count_, vblank_stuck_count_);
                    emu::logf(emu::LogLevel::warn, "BUS", "Last real frame: VBlank #%u", vblank_last_frame_);
                    emu::logf(emu::LogLevel::warn, "BUS", "I_STAT=0x%04X I_MASK=0x%04X pending=0x%04X",
                        (unsigned)i_stat_, (unsigned)i_mask_, (unsigned)(i_stat_ & i_mask_));
                    emu::logf(emu::LogLevel::warn, "BUS", "CPU PC=0x%08X", cpu_pc_);
                    emu::logf(emu::LogLevel::warn, "BUS", "Event table ptr=0x%08X", evt_ptr);
                    emu::logf(emu::LogLevel::warn, "BUS", "SysEnqIntRP chains: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
                        chain_ptrs[0], chain_ptrs[1], chain_ptrs[2], chain_ptrs[3]);
                    emu::logf(emu::LogLevel::warn, "BUS", "PCB=0x%08X TCB=0x%08X", pcb_ptr, tcb_ptr);

                    // Scan event table for VSync-related events
                    if (evt_ptr != 0)
                    {
                        const uint32_t size_off = 0x0124 & (ram_size_ - 1);
                        const uint32_t tbl_size = (uint32_t)ram_[size_off] |
                                                  ((uint32_t)ram_[size_off + 1] << 8) |
                                                  ((uint32_t)ram_[size_off + 2] << 16) |
                                                  ((uint32_t)ram_[size_off + 3] << 24);
                        const uint32_t max_entries = (tbl_size > 0) ? (tbl_size / 0x1C) : 16;
                        const uint32_t base_phys = evt_ptr & (ram_size_ - 1);

                        emu::logf(emu::LogLevel::warn, "BUS", "Event table size=%u max_entries=%u", tbl_size, max_entries);

                        int found_vsync = 0;
                        for (uint32_t i = 0; i < max_entries && i < 32; ++i)
                        {
                            const uint32_t eoff = base_phys + i * 0x1C;
                            if (eoff + 0x14 > ram_size_)
                                break;

                            const uint32_t cls = (uint32_t)ram_[eoff] |
                                                 ((uint32_t)ram_[eoff + 1] << 8) |
                                                 ((uint32_t)ram_[eoff + 2] << 16) |
                                                 ((uint32_t)ram_[eoff + 3] << 24);
                            if (cls == 0)
                                continue;

                            const uint32_t status = (uint32_t)ram_[eoff + 0x04] |
                                                    ((uint32_t)ram_[eoff + 0x05] << 8) |
                                                    ((uint32_t)ram_[eoff + 0x06] << 16) |
                                                    ((uint32_t)ram_[eoff + 0x07] << 24);
                            const uint32_t spec = (uint32_t)ram_[eoff + 0x08] |
                                                  ((uint32_t)ram_[eoff + 0x09] << 8) |
                                                  ((uint32_t)ram_[eoff + 0x0A] << 16) |
                                                  ((uint32_t)ram_[eoff + 0x0B] << 24);

                            // Log ALL events to understand what the game uses
                            const char* st_str = (status == 0x4000u) ? "READY" :
                                                 (status == 0x2000u) ? "BUSY" :
                                                 (status == 0x1000u) ? "ALLOCATED" : "???";
                            emu::logf(emu::LogLevel::warn, "BUS", "  Event[%u]: cls=0x%08X spec=0x%04X status=0x%04X (%s)",
                                i, cls, spec, status, st_str);

                            // Check for VSync-related classes
                            if (cls == 0xF2000003u || cls == 0xF0000001u)
                                found_vsync = 1;
                        }
                        if (!found_vsync)
                        {
                            emu::logf(emu::LogLevel::warn, "BUS", "  (No VSync events - game may use callbacks instead)");
                        }
                    }
                    emu::logf(emu::LogLevel::warn, "BUS", "===== END STUCK DUMP =====");
                }
            }

            // WORKAROUND: In non-HLE mode, the BIOS exception handler's VBlank
            // event delivery doesn't always work correctly with our hardware emulation.
            // After being stuck for 50+ VBlanks, force-mark ALL busy events as ready.
            if (vblank_stuck_count_ >= 50)
            {
                // Scan event table and mark ALL busy events as ready
                const uint32_t evt_ptr_off = 0x0120 & (ram_size_ - 1);
                const uint32_t evt_ptr = (uint32_t)ram_[evt_ptr_off] |
                                         ((uint32_t)ram_[evt_ptr_off + 1] << 8) |
                                         ((uint32_t)ram_[evt_ptr_off + 2] << 16) |
                                         ((uint32_t)ram_[evt_ptr_off + 3] << 24);
                if (evt_ptr != 0)
                {
                    const uint32_t base_phys = evt_ptr & (ram_size_ - 1);
                    for (uint32_t i = 0; i < 32; ++i)
                    {
                        const uint32_t eoff = base_phys + i * 0x1C;
                        if (eoff + 0x0C > ram_size_)  // Need 0x0C bytes: status at +4, spec at +8..+B
                            break;

                        const uint32_t st_off = eoff + 0x04;
                        const uint32_t status = (uint32_t)ram_[st_off] |
                                                ((uint32_t)ram_[st_off + 1] << 8) |
                                                ((uint32_t)ram_[st_off + 2] << 16) |
                                                ((uint32_t)ram_[st_off + 3] << 24);

                        // If event is BUSY (0x2000), mark it READY (0x4000)
                        if (status == 0x2000u)
                        {
                            // Read class and spec for logging (to identify which event is the key one)
                            const uint32_t cls_off = eoff + 0x00;
                            const uint32_t spec_off = eoff + 0x08;
                            const uint32_t evt_class = (uint32_t)ram_[cls_off] |
                                                       ((uint32_t)ram_[cls_off + 1] << 8) |
                                                       ((uint32_t)ram_[cls_off + 2] << 16) |
                                                       ((uint32_t)ram_[cls_off + 3] << 24);
                            const uint32_t evt_spec = (uint32_t)ram_[spec_off] |
                                                      ((uint32_t)ram_[spec_off + 1] << 8) |
                                                      ((uint32_t)ram_[spec_off + 2] << 16) |
                                                      ((uint32_t)ram_[spec_off + 3] << 24);

                            emu::logf(emu::LogLevel::warn, "BUS",
                                "RESCUE: Event[%u] cls=0x%08X spec=0x%04X BUSY->READY",
                                i, evt_class, evt_spec);

                            const uint32_t new_status = 0x4000u;
                            ram_[st_off]     = (uint8_t)(new_status & 0xFF);
                            ram_[st_off + 1] = (uint8_t)((new_status >> 8) & 0xFF);
                            ram_[st_off + 2] = (uint8_t)((new_status >> 16) & 0xFF);
                            ram_[st_off + 3] = (uint8_t)((new_status >> 24) & 0xFF);
                        }
                    }
                }
            }

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
