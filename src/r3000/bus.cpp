#include "bus.h"

// Disable heavyweight diagnostics for performance. Undefine to re-enable.
#define R3000_NO_DIAG

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
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
#include "../mdec/mdec.h"

// ---- Global pad button state (avoids Hot Reload class-layout issues) ----
static std::atomic<uint16_t> g_pad_buttons{0xFFFFu};

// ---- Clock period constants ----
static constexpr double kSysclkPeriodNs = 1e9 / 33868800.0;  // ~29.5ns
static constexpr double kSysclk8PeriodNs = kSysclkPeriodNs * 8.0; // ~236ns
static constexpr double kDotclkPeriodNs = 1e9 / 5322240.0;   // ~188ns (320px)

namespace r3000
{

static bool stage67_watch_pc(uint32_t pc)
{
    return pc >= 0x80065400u && pc <= 0x8006A800u;
}

static const char* stage67_watch_name(uint32_t phys)
{
    if (phys >= 0x0008ABDCu && phys <= 0x0008ACFFu) return "DAT_8008ABDC..ACFF";
    if (phys >= 0x00126240u && phys <= 0x0012624Bu) return "DAT_80126240..4B";
    if (phys >= 0x00127068u && phys <= 0x00127069u) return "DAT_80127068..69";
    if (phys >= 0x001270B4u && phys <= 0x001270B7u) return "DAT_801270B4..B7";
    if (phys >= 0x0012713Cu && phys <= 0x0012713Fu) return "DAT_8012713C..3F";
    if (phys >= 0x000E57D4u && phys <= 0x000E57DFu) return "DAT_800E57D4..DF";
    return nullptr;
}

#ifdef R3000_NO_DIAG
// Stub out all diagnostic helpers when diagnostics are disabled
static inline void log_stage67_watch(uint32_t&, const char*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
static inline bool stage67_global_name(uint32_t, const char**) { return false; }
#else
static void log_stage67_watch(
    uint32_t& log_count,
    const char* op,
    uint32_t pc,
    uint32_t addr,
    uint32_t phys,
    uint32_t value,
    uint32_t size
)
{
    const char* name = stage67_watch_name(phys);
    if (!name || !stage67_watch_pc(pc) || log_count >= 300u)
        return;
    ++log_count;
    emu::logf(
        emu::LogLevel::warn,
        "STAGE67",
        "%s pc=0x%08X addr=0x%08X phys=0x%08X size=%u val=0x%08X %s (#%u)",
        op,
        pc,
        addr,
        phys,
        size,
        value,
        name,
        log_count);
}

static bool stage67_global_name(uint32_t phys, const char** out_name)
{
    switch (phys)
    {
    case 0x0008ABDCu: *out_name = "DAT_8008ABDC"; return true;
    case 0x0008ABE0u: *out_name = "DAT_8008ABE0"; return true;
    case 0x0008ABE4u: *out_name = "DAT_8008ABE4"; return true;
    case 0x0008ABE8u: *out_name = "DAT_8008ABE8"; return true;
    case 0x0008ABECu: *out_name = "DAT_8008ABEC"; return true;
    case 0x0008ACFCu: *out_name = "DAT_8008ACFC"; return true;
    case 0x000B7BB0u: *out_name = "DAT_800B7BB0"; return true;
    case 0x000B7BC8u: *out_name = "DAT_800B7BC8"; return true;
    case 0x000B7BD0u: *out_name = "DAT_800B7BD0"; return true;
    case 0x00127068u: *out_name = "DAT_80127068"; return true;
    case 0x00127069u: *out_name = "DAT_80127069"; return true;
    case 0x001270B4u: *out_name = "DAT_801270B4"; return true;
    case 0x001270B6u: *out_name = "DAT_801270B6"; return true;
    case 0x0012713Cu: *out_name = "DAT_8012713C"; return true;
    case 0x001130D4u: *out_name = "DAT_801130D4_START_SLOT_GATE"; return true;
    // SCES_000.05 state machine variables (Tekken stage67 title-screen stall)
    case 0x001b72ccu: *out_name = "DAT_801b72cc_SOUND_LOOKUP"; return true;
    case 0x001cb3d4u: *out_name = "DAT_801cb3d4_LDSTATE"; return true;
    case 0x001cb3dcu: *out_name = "DAT_801cb3dc_LDSUB"; return true;
    case 0x001cb2ecu: *out_name = "DAT_801cb2ec_LOOPCTR"; return true;
    case 0x001cb2fcu: *out_name = "DAT_801cb2fc_ANIMCTR"; return true;
    case 0x001cb24cu: *out_name = "DAT_801cb24c_LDSTATE_EXIT"; return true;  // set=1 when exit() path taken in FUN_8015e440
    case 0x001d6190u: *out_name = "DAT_801d6190_DYNCODE_LOADED"; return true; // set=0x801FFF00 by FUN_8015d4e0 before calling 0x80140400
    default: return false;
    }
}

static uint32_t stage67_ram_rd32(const uint8_t* ram, uint32_t ram_size, uint32_t phys)
{
    if (!ram || phys + 3u >= ram_size)
        return 0u;
    return (uint32_t)ram[phys] |
           ((uint32_t)ram[phys + 1u] << 8) |
           ((uint32_t)ram[phys + 2u] << 16) |
           ((uint32_t)ram[phys + 3u] << 24);
}
#endif // R3000_NO_DIAG — static helpers

#ifdef R3000_NO_DIAG
void Bus::log_stage67_mmio_read(uint32_t, uint32_t, uint32_t) {}
#else
void Bus::log_stage67_mmio_read(uint32_t phys, uint32_t value, uint32_t size)
{
    if (!stage67_watch_pc(cpu_pc_) || stage67_mmio_log_count_ >= 256u)
        return;

    if (!(phys == 0x1F801814u ||
          phys == 0x1F801110u ||
          phys == 0x1F801114u ||
          phys == 0x1F801118u ||
          phys == 0x1F801070u ||
          phys == 0x1F801074u))
    {
        return;
    }

    const uint32_t abdc = stage67_ram_rd32(ram_, ram_size_, 0x0008ABDCu);
    const uint32_t abe0 = stage67_ram_rd32(ram_, ram_size_, 0x0008ABE0u);
    const uint32_t abe4 = stage67_ram_rd32(ram_, ram_size_, 0x0008ABE4u);
    const uint32_t abe8 = stage67_ram_rd32(ram_, ram_size_, 0x0008ABE8u);
    const uint32_t abec = stage67_ram_rd32(ram_, ram_size_, 0x0008ABECu);
    const uint32_t acfc = stage67_ram_rd32(ram_, ram_size_, 0x0008ACFCu);

    ++stage67_mmio_log_count_;
    if (phys == 0x1F801814u && gpu_)
    {
        const gpu::Stage67GpuDebug gd = gpu_->stage67_debug();
        emu::logf(
            emu::LogLevel::warn,
            "STAGE67MMIO",
            "RD%u pc=0x%08X phys=0x%08X -> 0x%08X bit31=%u scan=%u line_lsb=%u dy=%u hres=%u vres=%u pal=%u interlace=%u vblank=%u field=%u frame=%u abdc=0x%08X abe0=0x%08X abe4=0x%08X abe8=0x%08X abec=0x%08X acfc=0x%08X i_stat=0x%04X i_mask=0x%04X (#%u)",
            size * 8u,
            cpu_pc_,
            phys,
            value,
            (unsigned)((gd.gpustat >> 31) & 1u),
            (unsigned)gd.scanline,
            (unsigned)gd.display_line_lsb,
            (unsigned)gd.display_y,
            (unsigned)gd.h_res,
            (unsigned)gd.v_res,
            (unsigned)gd.is_pal,
            (unsigned)gd.interlace,
            (unsigned)gd.in_vblank,
            (unsigned)gd.even_odd_field,
            (unsigned)gd.frame_count,
            abdc,
            abe0,
            abe4,
            abe8,
            abec,
            acfc,
            (unsigned)i_stat_,
            (unsigned)i_mask_,
            stage67_mmio_log_count_);
        return;
    }

    emu::logf(
        emu::LogLevel::warn,
        "STAGE67MMIO",
        "RD%u pc=0x%08X phys=0x%08X -> 0x%08X abdc=0x%08X abe0=0x%08X abe4=0x%08X abe8=0x%08X abec=0x%08X acfc=0x%08X i_stat=0x%04X i_mask=0x%04X (#%u)",
        size * 8u,
        cpu_pc_,
        phys,
        value,
        abdc,
        abe0,
        abe4,
        abe8,
        abec,
        acfc,
        (unsigned)i_stat_,
        (unsigned)i_mask_,
        stage67_mmio_log_count_);
}
#endif // R3000_NO_DIAG — log_stage67_mmio_read + static helpers

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
    emu::logf(emu::LogLevel::warn, "BUS", "BUS source v55 (irq_ext_pending)");

    // Initialize EXP1 region to 0xFF (open bus)
    std::memset(exp1_, 0xFF, sizeof(exp1_));
    ram_face_tokens_.assign((ram_size_ + 3u) / 4u, kNoFaceToken);
    ram_face_writer_pc_.assign((ram_size_ + 3u) / 4u, 0u);
    scratch_face_tokens_.assign((kScratchSize + 3u) / 4u, kNoFaceToken);
    scratch_face_writer_pc_.assign((kScratchSize + 3u) / 4u, 0u);

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
                emu::logf(emu::LogLevel::warn, "IRQ", "CDROM IRQ push: i_stat=0x%04X i_mask=0x%04X",
                    (unsigned)bus->i_stat_, (unsigned)bus->i_mask_);
                bus->cdrom_->debug_log_bus_irq_latched(bus->i_stat_, bus->i_mask_);
            }
            bus->cdrom_irq_prev_ = (uint8_t)irq_state;
        }, this);
    }
}

Bus::~Bus()
{
    stop_gpu_thread();
    stop_timer_threads();
    stop_sio0_thread();

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
    if (paddr >= kScratchBase && paddr < (kScratchBase + kScratchSize))
    {
        if (scratch_face_tokens_.empty())
            return;
        const uint32_t off = paddr - kScratchBase;
        const size_t idx = (off >> 2) % scratch_face_tokens_.size();
        scratch_face_tokens_[idx] = token;
        if (!scratch_face_writer_pc_.empty())
            scratch_face_writer_pc_[idx] = cpu_pc_;
        return;
    }
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
    if (paddr >= kScratchBase && paddr < (kScratchBase + kScratchSize))
    {
        if (scratch_face_tokens_.empty())
            return kNoFaceToken;
        const uint32_t off = paddr - kScratchBase;
        return scratch_face_tokens_[(off >> 2) % scratch_face_tokens_.size()];
    }
    if (ram_face_tokens_.empty() || ram_size_ == 0)
        return kNoFaceToken;
    const uint32_t phys = paddr & (ram_size_ - 1u);
    return ram_face_tokens_[(phys >> 2) % ram_face_tokens_.size()];
}

uint32_t Bus::ram_face_writer_pc(uint32_t paddr) const
{
    if (paddr >= kScratchBase && paddr < (kScratchBase + kScratchSize))
    {
        if (scratch_face_writer_pc_.empty())
            return 0u;
        const uint32_t off = paddr - kScratchBase;
        return scratch_face_writer_pc_[(off >> 2) % scratch_face_writer_pc_.size()];
    }
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
    timer_check_irq(ch, t.count);

    // Signal timer thread to recalculate its sleep
    t.reconfig.store(1, std::memory_order_release);
    t.start_time = std::chrono::steady_clock::now();
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
            // Timer 2 has no gate input. DuckStation treats it like other timers
            // with gate=false: mode 0 (PauseWhileGateActive) → counts when !gate = true
            // mode 1 (ResetOnGateEnd) → always counts
            // mode 2 (ResetAndRunOnGateStart) → counts when gate = false → stopped
            // mode 3 (FreeRunOnGateEnd) → counts when gate = false → stopped
            // PSX-SPX says 0,3=stop and 1,2=free run, but DuckStation disagrees:
            // mode 0 counts (gate=false → !false=true), mode 3 stops (gate=false).
            switch (sync_mode)
            {
                case 0: t.counting_enabled = true; break;  // DuckStation: !gate = true
                case 1: t.counting_enabled = true; break;  // free run
                case 2: t.counting_enabled = false; break;  // gate=false → stopped
                case 3: t.counting_enabled = false; break;  // gate=false → stopped
            }
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

uint16_t Bus::timer_compute_count(int ch) const
{
    const Timer& t = timers_[ch];
    if (!t.counting_enabled)
        return (uint16_t)(t.count & 0xFFFFu);

    // When timer threads are active, compute count from elapsed real time
    if (timer_threads_running_.load(std::memory_order_relaxed))
    {
        const auto now = std::chrono::steady_clock::now();
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - t.start_time).count();
        if (elapsed <= 0) return (uint16_t)(t.count & 0xFFFFu);

        // Determine tick period based on clock source
        double ns_per_tick = kSysclkPeriodNs;
        if (t.use_external_clock)
        {
            if (ch == 0) ns_per_tick = kDotclkPeriodNs;
            else if (ch == 1) return (uint16_t)(t.count & 0xFFFFu); // HBlank: GPU thread handles count
            else if (ch == 2) ns_per_tick = kSysclk8PeriodNs;
        }

        const uint64_t ticks = (uint64_t)(elapsed / ns_per_tick);
        const bool reset_at_target = (t.mode & 0x0008u) != 0;
        const uint32_t period = reset_at_target ? ((uint32_t)t.target + 1u) : 0x10000u;
        return (uint16_t)((t.count + ticks) % period);
    }

    // Legacy: return stored count (updated by fast path tick)
    return (uint16_t)(t.count & 0xFFFFu);
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

        if (t.mode & 0x0008u) // reset_at_target
        {
            // The PS1 counter is effectively 16-bit with a target period of
            // (target + 1), not "target". Using modulo target shortens the
            // cycle by one tick and is especially wrong for target=0xFFFF.
            const uint32_t period = (uint32_t)t.target + 1u;
            t.count %= period;
        }
    }

    // Check overflow
    if (t.count > 0xFFFFu)
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
uint16_t Bus::sio0_stat_value()
{
    // Base: TX Ready 1 (bit 0) | TX Ready 2 (bit 2)
    uint16_t stat = (uint16_t)(sio0_stat_ | 0x0005u);

    // Bit 7: ACKINPUT — latched flag, set by do_ack(), cleared on read.
    // DuckStation: set in DoACK(), cleared when BIOS reads JOY_STAT.
    if (sio0_ack_input_flag_)
    {
        stat |= 0x0080u;
        sio0_ack_input_flag_ = 0; // clear on read
    }

    if (sio0_rx_ready_)
        stat |= 0x0002u; // RXRDY (bit 1)
    if (sio0_irq_flag_)
        stat |= 0x0200u; // IRQ flag (bit 9)
    return stat;
}

uint16_t Bus::sio0_stat_debug() const { return const_cast<Bus*>(this)->sio0_stat_value(); }

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
            emu::logf(emu::LogLevel::debug, "BUS",
                "SIO0 READ data=0x%02X phase=%u rxrdy=%u btns=0x%04X (#%u)",
                v, phase, (sio0_rx_ready_ ? 1u : 0u), btns, rd_log);
        }
    }

    return v;
}

void Bus::sio0_write_ctrl(uint16_t v)
{
    const uint16_t old_ctrl = sio0_ctrl_; // save BEFORE overwrite

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
        sio0_ack_input_flag_ = 0;
        return; // reset clears everything, nothing more to do
    }

    sio0_ctrl_ = v; // apply new value AFTER edge detection saved old_ctrl

    // Bit 4 (0x0010) = Acknowledge: clears STAT IRQ flag (bit 9) AND I_STAT bit 7.
    // This matches DuckStation: JOY_CTRL ACK clears INTR + SetLineState(false).
    if (v & 0x0010u)
    {
        sio0_irq_flag_ = 0;
        i_stat_ &= ~(1u << 7); // clear I_STAT SIO0 bit (like DuckStation SetLineState false)
    }

    // DuckStation: JOY_CTRL is a full write register. The BIOS writes the
    // ENTIRE value each time (including SELECT, TXEN, ACKINTEN etc.).
    // Only reset the device when SELECT transitions from 1 to 0.
    const bool old_select = (old_ctrl & 0x0002u) != 0;
    const bool new_select = (v & 0x0002u) != 0;
    if (old_select && !new_select)
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
    uint32_t xfer_ticks = (uint32_t)sio0_baud_ * 8u;
    if (xfer_ticks < 200u) xfer_ticks = 200u;

    if (sio0_thread_running_.load(std::memory_order_relaxed))
    {
        sio0_transfer_delay_ns_.store((uint32_t)(xfer_ticks * kSysclkPeriodNs), std::memory_order_release);
        sio0_transfer_signal_.store(0, std::memory_order_release);
        sio0_wake_thread(1); // wake thread: transfer requested
    }
    else
    {
        sio0_transfer_countdown_ = xfer_ticks;
    }
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
            else if (v == 0x81u)
            {
                // Memory card (slot 1) — NOT IMPLEMENTED
                // No ACK → game sees "no card connected" and won't send more bytes.
                // Log every attempt prominently so we know what the game wants.
                ++mc_access_count_;
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "========================================");
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "  MEMORY CARD ACCESS #%u  (NO CARD)",
                    mc_access_count_);
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "  device=0x81 slot=1 vbl=%u ctrl=0x%04X",
                    vblank_total_count_, sio0_ctrl_);
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "  STUB: returning 0xFF, no ACK");
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "  (Memory card emulation not yet implemented)");
                emu::logf(emu::LogLevel::warn, "SIO0_MEMCARD",
                    "========================================");
                // Stay in phase 0, no ACK → game detects absence
            }
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

    // Debug: log SIO0 phase transitions after game boot (vbl > 300)
    {
        static uint32_t phase_log = 0;
        if (phase_log < 50 && vblank_total_count_ > 300)
        {
            ++phase_log;
            emu::logf(emu::LogLevel::warn, "SIO0_PHASE",
                "[%u] tx=0x%02X resp=0x%02X phase=%u->%u ack=%d vbl=%u",
                phase_log, v, resp, prev_phase, sio0_tx_phase_,
                (sio0_tx_phase_ != 0u) ? 1 : 0, vblank_total_count_);
        }
    }

    // SIO0 IRQ: fire on every transfer completion.
    // The PS1 SIO0 fires IRQ7 whenever a byte transfer completes.
    // Games (especially Soul Reaver's custom MC driver) poll i_stat
    // bit 7 directly without necessarily setting RXINTEN/ACKINTEN.
    sio0_irq_flag_ = 1;
    i_stat_ |= (1u << 7); // SIO0 IRQ

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

            // Debug: dump pad buffer after SIO0 transfer complete.
            // Scan RAM for the BIOS pad buffer by looking for the pattern
            // that InitTAP wrote (IsOK + ID at known addresses).
            // The BIOS writes the SIO0 response into the buffer passed to InitTAP.
            static uint32_t pad_dump_count = 0;
            if (pad_dump_count < 30)
            {
                ++pad_dump_count;
                // Read first 8 bytes at a few candidate addresses
                auto rd8 = [&](uint32_t p) -> uint8_t {
                    return (p < ram_size_) ? ram_[p] : 0;
                };
                auto rd16 = [&](uint32_t p) -> uint16_t {
                    return (p + 1 < ram_size_) ?
                        ((uint16_t)ram_[p] | ((uint16_t)ram_[p+1] << 8)) : 0;
                };
                // Try the address from tstjoy.map: 0x17080
                const uint32_t base = 0x17080u;
                emu::logf(emu::LogLevel::warn, "PAD_BUF",
                    "[%u] @0x%05X IsOK=%d ID=0x%02X Pad=0x%04X byte2=0x%02X byte3=0x%02X btns_sent=0x%04X",
                    pad_dump_count, base,
                    (int)(int8_t)rd8(base),
                    rd8(base + 1),
                    rd16(base + 2),
                    rd8(base + 4), rd8(base + 5),
                    pad_buttons());
            }
        }
    }
    else
    {
        // Schedule ACK delay: 450 ticks for controllers (DuckStation)
        sio0_state_ = Sio0State::WaitingForACK;
        if (sio0_thread_running_.load(std::memory_order_relaxed))
        {
            sio0_ack_delay_ns_.store((uint32_t)(450.0 * kSysclkPeriodNs), std::memory_order_release);
            sio0_wake_thread(2); // wake thread: ACK requested
        }
        else
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
                emu::logf(emu::LogLevel::warn, "BUS",
                    "SIO0 xfer #%u START: btns=0x%04X baud=%u vbl=%u",
                    sio0_xfer_count, btns, (unsigned)sio0_baud_, vblank_total_count_);
            }
        }

        if (btns != 0xFFFFu && prev_phase == 4u && sio0_pressed_log < 20)
        {
            ++sio0_pressed_log;
            emu::logf(emu::LogLevel::debug, "BUS",
                "SIO0 xfer COMPLETE: btns=0x%04X lo=0x%02X hi=0x%02X (#%u)",
                btns, (unsigned)(btns & 0xFF), (unsigned)(btns >> 8), sio0_pressed_log);
        }
    }
}

// ACK pulse completed: set ACKINPUT flag + IRQ
void Bus::sio0_do_ack()
{
    sio0_ack_input_flag_ = 1; // Latched — cleared when BIOS reads STAT

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
            emu::logf(emu::LogLevel::debug, "BUS", "SIO0 TX FIFO overrun (v=0x%02X)", v);
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
        log_stage67_watch(stage67_watch_log_count_, "RD8", cpu_pc_, addr, phys & (ram_size_ - 1), out, 1);
        if (cpu_pc_ >= 0xBFC03000u && cpu_pc_ <= 0xBFC07000u &&
            phys >= 0x0000B000u && phys < 0x0000B900u &&
            bios_cd_ram_log_count_ < 256u)
        {
            ++bios_cd_ram_log_count_;
            const unsigned v = (unsigned)out;
            const char c = (v >= 0x20u && v <= 0x7Eu) ? (char)v : '.';
            emu::logf(
                emu::LogLevel::warn,
                "CDRAM",
                "BIOS RD8 pc=0x%08X addr=0x%08X phys=0x%05X -> 0x%02X '%c' i_stat=0x%04X i_mask=0x%04X (#%u)",
                cpu_pc_,
                addr,
                phys & (ram_size_ - 1),
                v,
                c,
                (unsigned)i_stat_,
                (unsigned)i_mask_,
                bios_cd_ram_log_count_);
        }
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
        if (cdrom_ &&
            cpu_pc_ >= 0xBFC04A00u && cpu_pc_ <= 0xBFC05580u &&
            bios_cd_mmio_log_count_ < 400)
        {
            ++bios_cd_mmio_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CDMMIO",
                "BIOS RD pc=0x%08X addr=0x%08X off=%u -> 0x%02X idx=%u stat=0x%02X irqf=0x%02X irqe=0x%02X i_stat=0x%04X i_mask=0x%04X",
                cpu_pc_,
                phys,
                (unsigned)(phys - kCdromBase),
                (unsigned)out,
                (unsigned)cdrom_->debug_index_raw(),
                (unsigned)cdrom_->debug_status_reg(),
                (unsigned)cdrom_->irq_flags_raw(),
                (unsigned)cdrom_->irq_enable_raw(),
                (unsigned)i_stat_,
                (unsigned)i_mask_);
        }
        // Check if CDROM IRQ edge occurred (e.g., after reading status that clears IRQ)
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

    // DPCR / DICR (byte read — LBU from DMA control registers)
    if (phys >= 0x1F8010F0u && phys < 0x1F8010F8u)
    {
        const uint32_t reg = (phys < 0x1F8010F4u) ? dpcr_ : dicr_;
        const uint32_t byte_off = phys & 3u;
        out = (uint8_t)((reg >> (byte_off * 8)) & 0xFFu);
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

    // Timer registers (byte read)
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t aligned = phys & ~3u;
        const uint32_t off = aligned - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            uint32_t val32 = 0;
            switch (reg)
            {
            case 0: val32 = timer_compute_count(ch); break;
            case 1: val32 = timers_[ch].mode; timers_[ch].mode &= ~0x1800u; break;
            case 2: val32 = timers_[ch].target; break;
            }
            out = (uint8_t)(val32 >> ((phys & 3) * 8));
            return true;
        }
    }

    // GPU registers (byte read)
    if (phys >= kGpuBase && phys < kGpuBase + 8)
    {
        const uint32_t val32 = gpu_ ? gpu_->mmio_read32(phys & ~3u) : 0x14802000u;
        out = (uint8_t)(val32 >> ((phys & 3) * 8));
        return true;
    }

    // DMA registers (byte read)
    if (phys >= 0x1F801080u && phys < 0x1F801100u)
    {
        const uint32_t aligned = phys & ~3u;
        const uint32_t dma_off = aligned - 0x1F801080u;
        const int ch = dma_off / 0x10;
        const int reg = (dma_off % 0x10) / 4;
        if (ch < 7)
        {
            uint32_t val32 = 0;
            switch (reg) { case 0: val32 = dma_[ch].madr; break; case 1: val32 = dma_[ch].bcr; break; case 2: val32 = dma_[ch].chcr; break; }
            out = (uint8_t)(val32 >> ((phys & 3) * 8));
            return true;
        }
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
        // Log unhandled MMIO reads that hit the fallback (potential bugs)
        if (phys >= 0x1F801000u && phys < 0x1F802000u)
        {
            static uint32_t io_fb_r8 = 0;
            if (io_fb_r8 < 50) { ++io_fb_r8; emu::logf(emu::LogLevel::warn, "CORE", "IO_FB_R8 phys=0x%08X val=0x%02X pc=0x%08X", phys, io_[phys - kIoBase], cpu_pc_); }
        }
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
        log_stage67_watch(stage67_watch_log_count_, "RD16", cpu_pc_, addr, mp0, out, 2);
        if (cpu_pc_ >= 0xBFC03000u && cpu_pc_ <= 0xBFC07000u &&
            phys >= 0x0000B000u && phys < 0x0000B900u &&
            bios_cd_ram_log_count_ < 256u)
        {
            ++bios_cd_ram_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CDRAM",
                "BIOS RD16 pc=0x%08X addr=0x%08X phys=0x%05X -> 0x%04X i_stat=0x%04X i_mask=0x%04X (#%u)",
                cpu_pc_,
                addr,
                mp0,
                (unsigned)out,
                (unsigned)i_stat_,
                (unsigned)i_mask_,
                bios_cd_ram_log_count_);
        }
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

    // DMA registers (16-bit read)
    if (phys >= 0x1F801080u && phys < 0x1F801100u)
    {
        const uint32_t off = phys - 0x1F801080u;
        const int ch = off / 0x10;
        const int reg = (off % 0x10) / 4;
        const bool lo = ((off % 4) == 0); // low or high halfword
        if (ch < 7)
        {
            uint32_t val32 = 0;
            switch (reg)
            {
            case 0: val32 = dma_[ch].madr; break;
            case 1: val32 = dma_[ch].bcr; break;
            case 2: val32 = dma_[ch].chcr; break;
            }
            out = lo ? (uint16_t)(val32 & 0xFFFFu) : (uint16_t)(val32 >> 16);
            return true;
        }
    }
    if (phys == 0x1F8010F0u || phys == 0x1F8010F2u)
    {
        out = (phys & 2) ? (uint16_t)(dpcr_ >> 16) : (uint16_t)(dpcr_ & 0xFFFFu);
        return true;
    }
    if (phys == 0x1F8010F4u || phys == 0x1F8010F6u)
    {
        out = (phys & 2) ? (uint16_t)(dicr_ >> 16) : (uint16_t)(dicr_ & 0xFFFFu);
        return true;
    }

    // GPU registers (16-bit read)
    if (phys >= kGpuBase && phys < kGpuBase + 8)
    {
        const uint32_t val32 = gpu_ ? gpu_->mmio_read32(phys & ~3u) : 0x14802000u;
        out = (phys & 2) ? (uint16_t)(val32 >> 16) : (uint16_t)(val32 & 0xFFFFu);
        return true;
    }

    // Timer registers (16-bit read — BIOS chkRC2wait uses LHU on Timer 2)
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t off = phys - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            switch (reg)
            {
            case 0: out = (uint16_t)(timer_compute_count(ch)); break;
            case 1:
                out = (uint16_t)timers_[ch].mode;
                timers_[ch].mode &= ~0x1800u;
                break;
            case 2: out = (uint16_t)timers_[ch].target; break;
            default: out = 0; break;
            }
            return true;
        }
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
        if (phys >= 0x1F801000u && phys < 0x1F802000u)
        {
            static uint32_t io_fb_r16 = 0;
            if (io_fb_r16 < 50) { ++io_fb_r16; emu::logf(emu::LogLevel::warn, "CORE", "IO_FB_R16 phys=0x%08X pc=0x%08X", phys, cpu_pc_); }
        }
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
        // Unconditional read trace: start_slot gate variable (any reader, any PC)
        if (mp0 == 0x001130D4u)
        {
            emu::logf(emu::LogLevel::debug, "STAGE67",
                "START_SLOT_GATE_READ pc=0x%08X val=0x%08X (%d signed)",
                cpu_pc_, out, (int32_t)out);
        }
        log_stage67_watch(stage67_watch_log_count_, "RD32", cpu_pc_, addr, mp0, out, 4);
        if (cpu_pc_ >= 0xBFC03000u && cpu_pc_ <= 0xBFC07000u &&
            phys >= 0x0000B000u && phys < 0x0000B900u &&
            bios_cd_ram_log_count_ < 256u)
        {
            ++bios_cd_ram_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CDRAM",
                "BIOS RD32 pc=0x%08X addr=0x%08X phys=0x%05X -> 0x%08X i_stat=0x%04X i_mask=0x%04X (#%u)",
                cpu_pc_,
                addr,
                mp0,
                out,
                (unsigned)i_stat_,
                (unsigned)i_mask_,
                bios_cd_ram_log_count_);
        }
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
        log_stage67_mmio_read(phys, out, 4);
        return true;
    }
    if (phys == kIrqMaskAddr)
    {
        out = i_mask_;
        log_stage67_mmio_read(phys, out, 4);
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
        log_stage67_mmio_read(phys, out, 4);
        return true;
    }

    // MDEC registers (0x1F801820 = data/response, 0x1F801824 = status)
    if (mdec_ && (phys == 0x1F80'1820u || phys == 0x1F80'1824u))
    {
        out = mdec_->read_reg(phys);
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
            case 0: out = (uint16_t)(timer_compute_count(ch)); break;
            case 1:
                // Reading mode register returns current value then clears bits 11-12
                // (target reached and overflow flags). This is PS1 hardware behavior.
                out = timers_[ch].mode;
                timers_[ch].mode &= ~0x1800u; // Clear bits 11 (reached_target) and 12 (reached_overflow)
                break;
            case 2: out = timers_[ch].target; break;
            default: out = 0; break;
            }
            log_stage67_mmio_read(phys, out, 4);
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
        if (mp >= 0x00050000u && mp < 0x00070000u && code_overlay_log_count_ < 256u)
        {
            ++code_overlay_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODEOVL",
                "WR8 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%02X (#%u)",
                cpu_pc_, addr, mp, (unsigned)v, code_overlay_log_count_);
        }
        if (mp >= 0x00054A80u && mp < 0x00054AC0u && code_stage54_log_count_ < 128u)
        {
            ++code_stage54_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE54",
                "WR8 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%02X (#%u)",
                cpu_pc_, addr, mp, (unsigned)v, code_stage54_log_count_);
        }
        if (mp >= 0x000677D0u && mp < 0x00067820u && code_stage67win_log_count_ < 128u)
        {
            ++code_stage67win_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE67",
                "WR8 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%02X (#%u)",
                cpu_pc_, addr, mp, (unsigned)v, code_stage67win_log_count_);
        }
        if (!runtime_loader_dumped_ &&
            (mp >= 0x00054A80u && mp < 0x00054AC0u || mp >= 0x000677D0u && mp < 0x00067820u) &&
            cpu_pc_ >= 0x801C0000u && cpu_pc_ < 0x801D0000u)
        {
            runtime_loader_dumped_ = 1;
            const uint32_t dump_base = 0x001C0D00u;
            if (dump_base + 0x200u <= ram_size_)
            {
                if (std::FILE* f = std::fopen("logs/runtime_loader_801C0D00.bin", "wb"))
                {
                    std::fwrite(ram_ + dump_base, 1, 0x200u, f);
                    std::fclose(f);
                }
                emu::logf(
                    emu::LogLevel::warn,
                    "LOADERDUMP",
                    "pc=0x%08X dumped logs/runtime_loader_801C0D00.bin base=0x801C0D00",
                    cpu_pc_);
                for (uint32_t off = 0; off < 0x80u; off += 4u)
                {
                    const uint32_t o = dump_base + off;
                    const uint32_t w = (uint32_t)ram_[o] |
                                       ((uint32_t)ram_[o + 1] << 8) |
                                       ((uint32_t)ram_[o + 2] << 16) |
                                       ((uint32_t)ram_[o + 3] << 24);
                    emu::logf(
                        emu::LogLevel::warn,
                        "LOADERDUMP",
                        "0x%08X: 0x%08X",
                        0x801C0D00u + off,
                        w);
                }
            }
        }
        log_stage67_watch(stage67_watch_log_count_, "WR8", cpu_pc_, addr, mp, v, 1);
        {
            const char* gname = nullptr;
            if (stage67_global_name(mp, &gname) && stage67_watch_log_count_ < 500u)
            {
                ++stage67_watch_log_count_;
                emu::logf(emu::LogLevel::debug, "STAGE67",
                    "GWR8 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%02X %s (#%u)",
                    cpu_pc_, addr, mp, (unsigned)v, gname, stage67_watch_log_count_);
            }
        }

        // Watch INT3 callback (DAT_801ef68c) byte writes — catches CdSetCallback SW or SB
        if (mp >= 0x001EF68Cu && mp <= 0x001EF68Fu)
        {
            const uint32_t cur = (uint32_t)ram_[0x1EF68Cu] | ((uint32_t)ram_[0x1EF68Du]<<8)
                               | ((uint32_t)ram_[0x1EF68Eu]<<16) | ((uint32_t)ram_[0x1EF68Fu]<<24);
            emu::logf(emu::LogLevel::debug, "INT3CB",
                "INT3_CB_WR8 pc=0x%08X byte@%08X=0x%02X (u32_before=0x%08X)",
                cpu_pc_, mp, (unsigned)v, cur);
        }
        // Watch EXE_DST_PTR (DAT_801CB33C) byte writes
        if (mp >= 0x001CB33Cu && mp <= 0x001CB33Fu)
        {
            const uint32_t cur = (uint32_t)ram_[0x1CB33Cu] | ((uint32_t)ram_[0x1CB33Du]<<8)
                               | ((uint32_t)ram_[0x1CB33Eu]<<16) | ((uint32_t)ram_[0x1CB33Fu]<<24);
            emu::logf(emu::LogLevel::debug, "EXEDST",
                "EXE_DST_WR8 pc=0x%08X byte@%08X=0x%02X (u32_before=0x%08X)",
                cpu_pc_, mp, (unsigned)v, cur);
        }

        // B9D0 write-watch (byte path)
        if (mp >= 0x0000B9D0u && mp <= 0x0000B9D3u)
        {
            static uint32_t b9d0_u8_cnt = 0;
            const uint32_t vbl = vblank_total_count_;
            if (b9d0_u8_cnt < 10u || (vbl >= 2420u && vbl <= 2560u))
            {
                emu::logf(emu::LogLevel::debug, "B9D0WATCH",
                    "WR8 pc=0x%08X byte@%05X=0x%02X vblank=%u (#%u)",
                    cpu_pc_, mp, (unsigned)v, vbl, ++b9d0_u8_cnt);
            }
            else ++b9d0_u8_cnt;
        }

        // Monitor all writes to CDROM event table status bytes (any size, any value)
        // Events[0-4] status fields at 0xE02C, 0xE048, 0xE064, 0xE080, 0xE09C
        {
            static uint32_t evt_u8_cnt = 0;
            if (evt_u8_cnt < 256u && mp >= 0x0000E028u && mp < 0x0000E0B4u)
            {
                ++evt_u8_cnt;
                emu::logf(emu::LogLevel::debug, "EVT_TBL",
                    "U8[%u] phys=0x%05X val=0x%02X pc=0x%08X",
                    evt_u8_cnt, mp, (unsigned)v, cpu_pc_);
            }
        }

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
        if (cdrom_ &&
            cpu_pc_ >= 0xBFC04A00u && cpu_pc_ <= 0xBFC05580u &&
            bios_cd_mmio_log_count_ < 400)
        {
            ++bios_cd_mmio_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CDMMIO",
                "BIOS WR pc=0x%08X addr=0x%08X off=%u val=0x%02X idx=%u stat=0x%02X irqf=0x%02X irqe=0x%02X i_stat=0x%04X i_mask=0x%04X",
                cpu_pc_,
                phys,
                (unsigned)(phys - kCdromBase),
                (unsigned)v,
                (unsigned)cdrom_->debug_index_raw(),
                (unsigned)cdrom_->debug_status_reg(),
                (unsigned)cdrom_->irq_flags_raw(),
                (unsigned)cdrom_->irq_enable_raw(),
                (unsigned)i_stat_,
                (unsigned)i_mask_);
        }
        if (cdrom_)
        {
            // Trace: log SetLoc zero params (the bug we're hunting)
            if ((phys & 3) == 2 && v == 0x00)
            {
                emu::logf(emu::LogLevel::warn, "CORE",
                    "CDWR pc=0x%08X param=0x00 reg=%u vbl=%u",
                    cpu_pc_, (unsigned)(phys & 3), vblank_total_count_);
            }
            cdrom_->mmio_write8(phys, v);
        }
        // Check for IRQ edge after write (command execution, IRQ ack, etc.)
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
        log_irq_stat_cd_clear(old, i_stat_, "SB", byte_off);
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
                    emu::logf(emu::LogLevel::debug, "BUS", "I_MASK VBlank off (byte): 0x%04X -> 0x%04X (#%u)",
                        (unsigned)old_mask, (unsigned)i_mask_, imask_log);
                }
            }
            }
        }
        return true;
    }

    // DPCR / DICR (byte write — SB to DMA control registers)
    if (phys >= 0x1F8010F0u && phys < 0x1F8010F8u)
    {
        if (phys < 0x1F8010F4u)
        {
            // DPCR byte write
            const uint32_t byte_off = phys - 0x1F8010F0u;
            const uint32_t shift = byte_off * 8;
            dpcr_ = (dpcr_ & ~((uint32_t)0xFFu << shift)) | ((uint32_t)v << shift);
            return true;
        }

        // DICR byte write
        const uint32_t byte_off = phys - 0x1F8010F4u;
        const uint32_t shift = byte_off * 8;

        if (byte_off == 3)
        {
            // Byte 3 (bits 24-31): bits 24-30 acknowledge (write 1 clears), bit 31 read-only
            const uint32_t ack_mask = ((uint32_t)(v & 0x7Fu)) << 24;
            dicr_ &= ~ack_mask;
        }
        else
        {
            // Bytes 0-2: apply writable mask per byte
            const uint32_t byte_mask = (uint32_t)0xFFu << shift;
            constexpr uint32_t wr_mask = 0x00FF803Fu; // bits 0-5, 15, 16-23
            const uint32_t effective = byte_mask & wr_mask;
            dicr_ = (dicr_ & ~effective) | (((uint32_t)v << shift) & effective);
        }

        // Recompute master flag
        const uint32_t flags     = (dicr_ >> 24) & 0x7Fu;
        const int      force     = (dicr_ >> 15) & 1;
        const int      master_en = (dicr_ >> 23) & 1;
        const int      old_mf    = (dicr_ >> 31) & 1;
        if (force || (master_en && flags))
            dicr_ |= (1u << 31);
        else
            dicr_ &= ~(1u << 31);
        if (!old_mf && (dicr_ & (1u << 31)))
            i_stat_ |= (1u << 3);

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

    // Timer registers (byte write — read-modify-write)
    if (phys >= kTimerBase && phys < kTimerBase + kTimerSpan)
    {
        const uint32_t aligned = phys & ~3u;
        const uint32_t off = aligned - kTimerBase;
        const int ch = off / kTimerBlock;
        const int reg = (off % kTimerBlock) / 4;
        if (ch < 3)
        {
            const uint32_t shift = (phys & 3) * 8;
            uint32_t cur = 0;
            switch (reg) { case 0: cur = timers_[ch].count; break; case 1: cur = timers_[ch].mode; break; case 2: cur = timers_[ch].target; break; }
            cur = (cur & ~(0xFFu << shift)) | ((uint32_t)v << shift);
            switch (reg)
            {
            case 0: { const uint32_t old = timers_[ch].count; timers_[ch].count = cur & 0xFFFFu; timer_check_irq(ch, old); break; }
            case 1: timer_write_mode(ch, (uint16_t)cur); break;
            case 2: timers_[ch].target = cur & 0xFFFFu; timer_check_irq(ch, timers_[ch].count); break;
            }
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
        if (mp0 >= 0x00050000u && mp0 < 0x00070000u && code_overlay_log_count_ < 256u)
        {
            ++code_overlay_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODEOVL",
                "WR16 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%04X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_overlay_log_count_);
        }
        if (mp0 >= 0x00054A80u && mp0 < 0x00054AC0u && code_stage54_log_count_ < 128u)
        {
            ++code_stage54_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE54",
                "WR16 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%04X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_stage54_log_count_);
        }
        if (mp0 >= 0x000677D0u && mp0 < 0x00067820u && code_stage67win_log_count_ < 128u)
        {
            ++code_stage67win_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE67",
                "WR16 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%04X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_stage67win_log_count_);
        }
        log_stage67_watch(stage67_watch_log_count_, "WR16", cpu_pc_, addr, mp0, v, 2);
        {
            const char* gname = nullptr;
            if (stage67_global_name(mp0, &gname) && stage67_watch_log_count_ < 500u)
            {
                ++stage67_watch_log_count_;
                emu::logf(emu::LogLevel::debug, "STAGE67",
                    "GWR16 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%04X %s (#%u)",
                    cpu_pc_, addr, mp0, (unsigned)v, gname, stage67_watch_log_count_);
            }
        }

        // Watch INT3 callback (DAT_801ef68c) and EXE_DST_PTR via u16 writes
        if (mp0 >= 0x001EF68Cu && mp0 <= 0x001EF68Fu)
        {
            const uint32_t cur = (uint32_t)ram_[0x1EF68Cu] | ((uint32_t)ram_[0x1EF68Du]<<8)
                               | ((uint32_t)ram_[0x1EF68Eu]<<16) | ((uint32_t)ram_[0x1EF68Fu]<<24);
            emu::logf(emu::LogLevel::debug, "INT3CB",
                "INT3_CB_WR16 pc=0x%08X half@%08X=0x%04X (u32_before=0x%08X)",
                cpu_pc_, mp0, (unsigned)v, cur);
        }
        if (mp0 >= 0x001CB33Cu && mp0 <= 0x001CB33Fu)
        {
            const uint32_t cur = (uint32_t)ram_[0x1CB33Cu] | ((uint32_t)ram_[0x1CB33Du]<<8)
                               | ((uint32_t)ram_[0x1CB33Eu]<<16) | ((uint32_t)ram_[0x1CB33Fu]<<24);
            emu::logf(emu::LogLevel::debug, "EXEDST",
                "EXE_DST_WR16 pc=0x%08X half@%08X=0x%04X (u32_before=0x%08X)",
                cpu_pc_, mp0, (unsigned)v, cur);
        }

        // B9D0 write-watch (u16 path)
        if (mp0 >= 0x0000B9D0u && mp0 <= 0x0000B9D2u)
        {
            static uint32_t b9d0_u16_cnt = 0;
            const uint32_t vbl = vblank_total_count_;
            if (b9d0_u16_cnt < 10u || (vbl >= 2420u && vbl <= 2560u))
            {
                emu::logf(emu::LogLevel::debug, "B9D0WATCH",
                    "WR16 pc=0x%08X half@%05X=0x%04X vblank=%u (#%u)",
                    cpu_pc_, mp0, (unsigned)v, vbl, ++b9d0_u16_cnt);
            }
            else ++b9d0_u16_cnt;
        }

        // Monitor all writes to CDROM event table area (any size, any value)
        {
            static uint32_t evt_u16_cnt = 0;
            if (evt_u16_cnt < 256u && mp0 >= 0x0000E028u && mp0 < 0x0000E0B4u)
            {
                ++evt_u16_cnt;
                emu::logf(emu::LogLevel::debug, "EVT_TBL",
                    "U16[%u] phys=0x%05X val=0x%04X pc=0x%08X",
                    evt_u16_cnt, mp0, (unsigned)v, cpu_pc_);
            }
        }

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
        log_irq_stat_cd_clear(old, i_stat_, "SH", v);
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
            // Diagnostic: when re-enabled from 0, log which IRQ is stuck
            if (old_mask == 0 && i_mask_ != 0)
            {
                static uint32_t diag16 = 0;
                if (diag16 < 5)
                {
                    ++diag16;
                    emu::logf(emu::LogLevel::warn, "IRQ",
                        "DIAG16 I_MASK re-enable: 0x%04X I_STAT=0x%04X pending=0x%04X (#%u)",
                        (unsigned)i_mask_, (unsigned)i_stat_, (unsigned)(i_stat_ & i_mask_), diag16);
                }
            }
            // CRITICAL: Log when VBlank (bit 0) is disabled - this causes VSync timeout!
            if ((old_mask & 0x01) && !(i_mask_ & 0x01))
            {
                {
                static uint32_t imask_log2 = 0;
                if (imask_log2 < 3)
                {
                    ++imask_log2;
                    emu::logf(emu::LogLevel::debug, "BUS", "I_MASK VBlank off (word): 0x%04X -> 0x%04X (#%u)",
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

    // DMA registers (16-bit write — promote to 32-bit read-modify-write)
    if (phys >= 0x1F801080u && phys < 0x1F801100u)
    {
        const uint32_t aligned = phys & ~3u;
        const uint32_t dma_off = aligned - 0x1F801080u;
        const int ch = dma_off / 0x10;
        const int reg = (dma_off % 0x10) / 4;
        if (ch < 7)
        {
            uint32_t cur = 0;
            switch (reg) { case 0: cur = dma_[ch].madr; break; case 1: cur = dma_[ch].bcr; break; case 2: cur = dma_[ch].chcr; break; }
            if (phys & 2) cur = (cur & 0x0000FFFFu) | ((uint32_t)v << 16);
            else          cur = (cur & 0xFFFF0000u) | (uint32_t)v;
            switch (reg) { case 0: dma_[ch].madr = cur; break; case 1: dma_[ch].bcr = cur; break; case 2: dma_[ch].chcr = cur; break; }
        }
        return true;
    }
    if (phys == 0x1F8010F0u || phys == 0x1F8010F2u)
    {
        if (phys & 2) dpcr_ = (dpcr_ & 0x0000FFFFu) | ((uint32_t)v << 16);
        else          dpcr_ = (dpcr_ & 0xFFFF0000u) | (uint32_t)v;
        return true;
    }
    if (phys == 0x1F8010F4u || phys == 0x1F8010F6u)
    {
        if (phys & 2) dicr_ = (dicr_ & 0x0000FFFFu) | ((uint32_t)v << 16);
        else          dicr_ = (dicr_ & 0xFFFF0000u) | (uint32_t)v;
        return true;
    }

    // GPU registers (16-bit write — promote to 32-bit)
    if (phys >= kGpuBase && phys < kGpuBase + 8)
    {
        if (gpu_) gpu_->mmio_write32(phys & ~3u, (uint32_t)v);
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
            case 0:
            {
                const uint32_t old_count = timers_[ch].count;
                timers_[ch].count = (uint16_t)v;
                timer_check_irq(ch, old_count);
                break;
            }
            case 1: timer_write_mode(ch, (uint16_t)v); break;
            case 2:
                timers_[ch].target = (uint16_t)v;
                timer_check_irq(ch, timers_[ch].count);
                break;
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
        if (mp0 >= 0x00050000u && mp0 < 0x00070000u && code_overlay_log_count_ < 256u)
        {
            ++code_overlay_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODEOVL",
                "WR32 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%08X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_overlay_log_count_);
        }
        if (mp0 >= 0x00054A80u && mp0 < 0x00054AC0u && code_stage54_log_count_ < 128u)
        {
            ++code_stage54_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE54",
                "WR32 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%08X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_stage54_log_count_);
        }
        if (mp0 >= 0x000677D0u && mp0 < 0x00067820u && code_stage67win_log_count_ < 128u)
        {
            ++code_stage67win_log_count_;
            emu::logf(
                emu::LogLevel::warn,
                "CODE67",
                "WR32 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%08X (#%u)",
                cpu_pc_, addr, mp0, (unsigned)v, code_stage67win_log_count_);
        }
        // Unconditional watch: start_slot gate variable (any writer, any PC)
        if (mp0 == 0x001130D4u)
        {
            emu::logf(emu::LogLevel::debug, "STAGE67",
                "START_SLOT_GATE_WRITE pc=0x%08X val=0x%08X (%d signed)",
                cpu_pc_, v, (int32_t)v);
        }
        // Watch INT3 callback (DAT_801ef68c) and EXE_DST_PTR (DAT_801CB33C)
        if (mp0 == 0x001EF68Cu)
        {
            const uint32_t old_cb = (uint32_t)ram_[0x1EF68Cu] | ((uint32_t)ram_[0x1EF68Du]<<8)
                                  | ((uint32_t)ram_[0x1EF68Eu]<<16) | ((uint32_t)ram_[0x1EF68Fu]<<24);
            emu::logf(emu::LogLevel::debug, "INT3CB",
                "INT3_CB_WRITE pc=0x%08X old=0x%08X new=0x%08X",
                cpu_pc_, old_cb, v);
        }
        if (mp0 == 0x001CB33Cu)
        {
            const uint32_t old_dst = (uint32_t)ram_[0x1CB33Cu] | ((uint32_t)ram_[0x1CB33Du]<<8)
                                   | ((uint32_t)ram_[0x1CB33Eu]<<16) | ((uint32_t)ram_[0x1CB33Fu]<<24);
            emu::logf(emu::LogLevel::debug, "EXEDST",
                "EXE_DST_WRITE pc=0x%08X old=0x%08X new=0x%08X",
                cpu_pc_, old_dst, v);
        }
        // B938 watchpoint: BIOS I_STAT ACK guard flag (*0xA000B938 phys=0x0000B938)
        if (mp0 == 0x0000B938u)
        {
            emu::logf(emu::LogLevel::warn, "B938WATCH",
                "WRITE pc=0x%08X val=0x%08X (old=0x%08X)",
                cpu_pc_, v,
                (uint32_t)ram_[0x0000B938u] |
                ((uint32_t)ram_[0x0000B939u] << 8) |
                ((uint32_t)ram_[0x0000B93Au] << 16) |
                ((uint32_t)ram_[0x0000B93Bu] << 24));
        }
        // B93C watchpoint: companion guard flag
        if (mp0 == 0x0000B93Cu)
        {
            emu::logf(emu::LogLevel::warn, "B938WATCH",
                "B93C WRITE pc=0x%08X val=0x%08X",
                cpu_pc_, v);
        }
        // B9D0 write-watch: BIOS VBlank event flag (0xA000B9D0 phys=0x0000B9D0).
        // Logs every write so we can see if/when the BIOS stops setting it on VBlank.
        if (mp0 == 0x0000B9D0u)
        {
            const uint32_t vbl = vblank_total_count_;
            static uint32_t b9d0_cnt = 0;
            // Log all writes near the transition window, plus first 10 ever.
            if (b9d0_cnt < 10u || (vbl >= 2420u && vbl <= 2560u))
            {
                emu::logf(emu::LogLevel::debug, "B9D0WATCH",
                    "WRITE pc=0x%08X val=0x%08X vblank=%u (#%u)",
                    cpu_pc_, v, vbl, ++b9d0_cnt);
            }
            else
            {
                ++b9d0_cnt;
            }
        }

        // Watch ALL writes to CDROM event table area [0xE028, 0xE0B4) — any value, u32 path.
        // Events[0-4]: status at 0xE02C, 0xE048, 0xE064, 0xE080, 0xE09C (stride=0x1C, offset+4)
        {
            static uint32_t evt_u32_cnt = 0;
            if (evt_u32_cnt < 512u && mp0 >= 0x0000E028u && mp0 < 0x0000E0B4u)
            {
                ++evt_u32_cnt;
                emu::logf(emu::LogLevel::debug, "EVT_TBL",
                    "U32[%u] phys=0x%05X val=0x%08X pc=0x%08X",
                    evt_u32_cnt, mp0, v, cpu_pc_);
            }
        }
        log_stage67_watch(stage67_watch_log_count_, "WR32", cpu_pc_, addr, mp0, v, 4);
        {
            const char* gname = nullptr;
            if (stage67_global_name(mp0, &gname) && stage67_watch_log_count_ < 500u)
            {
                ++stage67_watch_log_count_;
                emu::logf(emu::LogLevel::debug, "STAGE67",
                    "GWR32 pc=0x%08X addr=0x%08X phys=0x%08X val=0x%08X %s (#%u)",
                    cpu_pc_, addr, mp0, v, gname, stage67_watch_log_count_);
            }
        }

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
        const uint32_t old = i_stat_;
        i_stat_ &= v;
        log_irq_stat_cd_clear(old, i_stat_, "SW", v);
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
            // Diagnostic: when I_MASK is re-enabled from 0, log I_STAT to identify stuck IRQ
            if (old_mask == 0 && i_mask_ != 0)
            {
                static uint32_t diag_log = 0;
                if (diag_log < 5)
                {
                    ++diag_log;
                    emu::logf(emu::LogLevel::warn, "IRQ",
                        "DIAG I_MASK re-enable: 0x%04X I_STAT=0x%04X pending=0x%04X (#%u)",
                        (unsigned)i_mask_, (unsigned)i_stat_, (unsigned)(i_stat_ & i_mask_), diag_log);
                }
            }
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
            case 0:
                dma_[ch].madr = v;
                break;
            case 1: dma_[ch].bcr = v; break;
            case 2: // CHCR
                dma_[ch].chcr = v;
                // Handle DMA start
                if (v & 0x01000000u)
                {
                    // MDEC: DMA0 (MDEC IN) and DMA1 (MDEC OUT)
                    if ((ch == 0 || ch == 1) && mdec_)
                    {
                        const uint32_t bs = dma_[ch].bcr & 0xFFFF;
                        const uint32_t bc = (dma_[ch].bcr >> 16) & 0xFFFF;
                        const uint32_t words = bs * (bc ? bc : 1);
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

                        if (ch == 0)
                        {
                            // DMA0: RAM → MDEC (compressed data in)
                            // Tick CDROM so streaming sectors continue arriving during
                            // MDEC decode. Without this, DecDCTvlc runs too fast relative
                            // to sector delivery and reads empty ring buffer slots.
                            if (cdrom_)
                            {
                                const uint32_t dma0_cycles = words; // ~1 cycle/word
                                // No CD tick from DMA0
                                check_cdrom_irq_edge();
                            }
                            std::vector<uint32_t> buf(words);
                            for (uint32_t i = 0; i < words; ++i)
                            {
                                buf[i] = (uint32_t)ram_[ma] |
                                         ((uint32_t)ram_[ma + 1] << 8) |
                                         ((uint32_t)ram_[ma + 2] << 16) |
                                         ((uint32_t)ram_[ma + 3] << 24);
                                ma = (ma + 4) & 0x1FFFFF;
                            }
                            {
                                static int dma0_log = 0;
                                if (dma0_log < 10)
                                {
                                    dma0_log++;
                                    emu::logf(emu::LogLevel::debug, "BUS",
                                        "DMA0 MDEC_IN #%d: madr=0x%08X words=%u",
                                        dma0_log, dma_[0].madr, words);
                                }
                            }
                            mdec_->dma_write(buf.data(), words);
                        }
                        else
                        {
                            // DMA1: MDEC → RAM (decoded pixels out)
                            {
                                static int dma1_log = 0;
                                if (dma1_log < 3) {
                                    dma1_log++;
                                    emu::logf(emu::LogLevel::debug, "BUS",
                                        "DMA1 MDEC_OUT #%d: madr=0x%08X words=%u",
                                        dma1_log, dma_[1].madr, words);
                                }
                            }
                            // Tick CDROM with realistic MDEC decode time.
                            // Real PS1: MDEC is cycle-accurate, takes real time to decode.
                            // Our MDEC is instant, so we compensate by ticking the CDROM
                            // with realistic MDEC+DMA time so streaming sectors arrive.
                            // Each DMA1 chunk = 1920 words = 10 MBs at 24-bit.
                            // Real PS1 timing: ~20000 cycles/MB (decode + mem stalls + DMA + GPU wait).
                            // This gives the CDROM enough time during MDEC output for
                            // the next frame's sectors to accumulate in the ring buffer.
                            const uint32_t mbs_per_chunk = (words > 192) ? words / 192 : 1;
                            const uint32_t dma1_cycles = words + mbs_per_chunk * 20000;
                            if (cdrom_)
                                // No CD tick from DMA1
                            check_cdrom_irq_edge();

                            std::vector<uint32_t> buf(words);
                            mdec_->dma_read(buf.data(), words);
                            for (uint32_t i = 0; i < words; ++i)
                            {
                                ram_[ma]     = (uint8_t)(buf[i]);
                                ram_[ma + 1] = (uint8_t)(buf[i] >> 8);
                                ram_[ma + 2] = (uint8_t)(buf[i] >> 16);
                                ram_[ma + 3] = (uint8_t)(buf[i] >> 24);
                                ma = (ma + 4) & 0x1FFFFF;
                            }
                            // Notify GPU that MDEC has produced output
                            if (gpu_) gpu_->notify_mdec_output();

                            // Count MDEC output frames for debugging
                            {
                                static uint32_t mdec_out_count = 0;
                                ++mdec_out_count;
                                if (mdec_out_count <= 5 || (mdec_out_count % 50 == 0))
                                {
                                    emu::logf(emu::LogLevel::warn, "CORE",
                                        "MDEC OUT #%u: %u words madr=0x%08X vbl=%u",
                                        mdec_out_count, words, dma_[1].madr, vblank_total_count_);
                                }
                            }
                        }
                        dma_finish(ch);
                    }
                    // DMA2 (GPU)
                    else if (ch == 2 && gpu_)
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
                                    if (ma >= 0xE000u && ma < 0xF000u)
                                    {
                                        static uint32_t dma2_evt = 0;
                                        if (dma2_evt < 8u)
                                        {
                                            ++dma2_evt;
                                            emu::logf(emu::LogLevel::warn, "DMA2_EVT",
                                                "[%u] GPU→RAM phys=0x%05X val=0x%08X madr=0x%08X",
                                                dma2_evt, ma, w, dma_[ch].madr);
                                        }
                                    }
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
                                    if (gpu_3d_) gpu_3d_->gp0_with_face_hint(w, tok, ram_face_writer_pc(ma));
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
                                        if (gpu_3d_) gpu_3d_->gp0_with_face_hint(w, tok, ram_face_writer_pc(off2));
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
                        // PS1 DMA3 is gated by DREQ from the CDROM (asserted when sector FIFO
                        // is non-empty). If the game triggers DMA3 before INT1 fills the FIFO
                        // (e.g., from an INT3 callback), defer the transfer until the FIFO is
                        // ready. bus.tick() will execute it after cdrom.tick() fills the FIFO.
                        if (cdrom_->is_fifo_empty())
                        {
                            dma3_pending_ = 1;
                            emu::logf(emu::LogLevel::debug, "BUS",
                                "DMA3 deferred (FIFO empty): madr=0x%08X bcr=0x%08X PC=0x%08X",
                                dma_[3].madr, dma_[3].bcr, cpu_pc_);
                            // CHCR bit 24 stays set — game sees DMA as in-progress.
                        }
                        else
                        {
                            exec_dma3_transfer();
                        }
                    }


                    // DMA6 (OTC - Ordering Table Clear)
                    if (ch == 6)
                    {
                        uint32_t words = dma_[ch].bcr & 0xFFFF;
                        if (words == 0) words = 0x10000;
                        uint32_t ma = dma_[ch].madr & 0x1FFFFF;

                        {
                            uint32_t ot_lowest = (words > 0) ? ((ma - (words - 1) * 4) & 0x1FFFFF) : ma;
                            emu::logf(emu::LogLevel::debug, "BUS",
                                "DMA6 OTC: madr=0x%08X words=%u range=[0x%06X - 0x%06X] ring=0x1F61E0 %s",
                                dma_[ch].madr, words, ot_lowest, ma,
                                (ot_lowest <= 0x1F61E0 && ma >= 0x1F61E0) ? "**OVERLAP!**" : "ok");
                        }

                        // Guard: OTC walks backward — check if it reaches kernel area
                        {
                            uint32_t lowest = (words > 0) ? ((ma - (words - 1) * 4) & 0x1FFFFF) : ma;
                            if (lowest < 0x200u)
                            {
                                emu::logf(emu::LogLevel::error, "BUS",
                                    "DMA6 OTC KERNEL CORRUPT! madr=0x%05X words=%u lowest=0x%05X",
                                    ma, words, lowest);
                            }
                        }

                        for (uint32_t i = 0; i < words; ++i)
                        {
                            uint32_t w = (i == words - 1) ? 0x00FFFFFFu : ((ma - 4) & 0x1FFFFF);
                            // DMA6_EVT: detect OTC overwriting CDROM event table [0xE000,0xF000)
                            if (ma >= 0x0000E000u && ma < 0x0000F000u)
                            {
                                static uint32_t dma6_evt_hit = 0;
                                if (dma6_evt_hit < 32u)
                                {
                                    ++dma6_evt_hit;
                                    emu::logf(emu::LogLevel::warn, "DMA6_EVT",
                                        "[%u] OTC writing phys=0x%05X val=0x%08X word=%u/%u madr_orig=0x%08X vblank=%u",
                                        dma6_evt_hit, ma, w, i, words, dma_[ch].madr, vblank_total_count_);
                                }
                            }
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
            return true;
        }
        // ch == 7: fall through to DPCR/DICR handlers below
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
        emu::logf(emu::LogLevel::debug, "DICR", "[DICR] write pc=0x%08X v=0x%08X old=0x%08X master_en_new=%d",
                  cpu_pc_, v, dicr_, (v >> 23) & 1);
        const uint32_t ack_mask = v & 0x7F000000u; // bits 24-30: writing 1 clears flag
        const uint32_t wr_mask  = 0x00FF803Fu;     // bits 0-5, 15-23: writable directly
        dicr_ = (dicr_ & ~wr_mask) | (v & wr_mask);
        dicr_ &= ~ack_mask; // acknowledge flags

        // Recompute master flag: force_irq || (master_enable && ANY_flag_set)
        const uint32_t flags   = (dicr_ >> 24) & 0x7Fu;
        const int force = (dicr_ >> 15) & 1;
        const int master_en = (dicr_ >> 23) & 1;
        const int old_mf = (dicr_ >> 31) & 1;
        if (force || (master_en && flags))
            dicr_ |= (1u << 31);
        else
            dicr_ &= ~(1u << 31);
        // Edge-trigger I_STAT bit 3 on 0→1 master flag transition
        if (!(old_mf) && (dicr_ & (1u << 31)))
            i_stat_ |= (1u << 3);
        return true;
    }

    // MDEC registers (write)
    if (mdec_ && (phys == 0x1F80'1820u || phys == 0x1F80'1824u))
    {
        mdec_->write_reg(phys, v);
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
                gpu_3d_->gp0_with_face_hint(v, kNoFaceToken, 0);
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
            case 0:
            {
                const uint32_t old_count = timers_[ch].count;
                timers_[ch].count = (uint16_t)v;
                timer_check_irq(ch, old_count);
                break;
            }
            case 1: timer_write_mode(ch, (uint16_t)v); break;
            case 2:
                timers_[ch].target = (uint16_t)v;
                timer_check_irq(ch, timers_[ch].count);
                break;
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

void Bus::log_irq_stat_cd_clear(uint32_t old_stat, uint32_t new_stat, const char* access_kind, uint32_t detail)
{
    if ((old_stat & 0x0004u) == 0u || (new_stat & 0x0004u) != 0u)
        return;
    if (irq_cd_clear_log_count_ >= 128u)
        return;
    ++irq_cd_clear_log_count_;
    emu::logf(emu::LogLevel::warn, "IRQ",
        "I_STAT ACK CD via %s pc=0x%08X old=0x%04X new=0x%04X detail=0x%08X i_mask=0x%04X (#%u)",
        access_kind,
        cpu_pc_,
        (unsigned)(old_stat & 0xFFFFu),
        (unsigned)(new_stat & 0xFFFFu),
        (unsigned)detail,
        (unsigned)(i_mask_ & 0xFFFFu),
        irq_cd_clear_log_count_);
}

uint32_t Bus::irq_pending_masked() const
{
    const uint32_t pending = i_stat_ & i_mask_;
    if (pending != 0u && irq_pending_log_count_ < 256u)
    {
        ++const_cast<Bus*>(this)->irq_pending_log_count_;
        emu::logf(emu::LogLevel::warn, "IRQ",
            "pending_masked=0x%04X i_stat=0x%04X i_mask=0x%04X cpu_pc=0x%08X (#%u)",
            (unsigned)pending, (unsigned)i_stat_, (unsigned)i_mask_, cpu_pc_, irq_pending_log_count_);
    }
    return pending;
}

// ================== DMA3 transfer ==================

void Bus::exec_dma3_transfer()
{
    if (!cdrom_) return;

    const uint32_t bs = dma_[3].bcr & 0xFFFF;
    const uint32_t bc = (dma_[3].bcr >> 16) & 0xFFFF;
    const uint32_t words = bs * (bc ? bc : 1);
    const uint32_t start_ma = dma_[3].madr & 0x1FFFFF;
    uint32_t ma = start_ma;
    const uint32_t end_addr = (ma + words * 4) & 0x1FFFFF;

    emu::logf(emu::LogLevel::debug, "BUS", "DMA3 CD→RAM madr=0x%08X bcr=0x%08X words=%u",
        dma_[3].madr, dma_[3].bcr, words);
    cdrom_->debug_log_dma3_start(dma_[3].madr, dma_[3].bcr, words);

    // DMA3 state snapshot: log PC + CD driver destination/callback for first 60 game-phase DMAs.
    {
        static uint32_t dma3_game_count = 0;
        const bool in_game_pc = (cpu_pc_ >= 0x80010000u && cpu_pc_ <= 0x8020FFFFu)
                             || (cpu_pc_ >= 0xA0010000u && cpu_pc_ <= 0xA020FFFFu);
        if (in_game_pc && dma3_game_count < 60u)
        {
            ++dma3_game_count;
            auto rd32_safe = [&](uint32_t phys) -> uint32_t {
                if (phys + 4u > ram_size_) return 0xDEADBEEFu;
                return (uint32_t)ram_[phys] | ((uint32_t)ram_[phys+1]<<8)
                     | ((uint32_t)ram_[phys+2]<<16) | ((uint32_t)ram_[phys+3]<<24);
            };
            const uint32_t cd_dest = rd32_safe(0x1ca694u);
            const uint32_t cd_cb   = rd32_safe(0x1ef68cu);
            const uint32_t ldst    = rd32_safe(0x1cb3d4u);
            const uint32_t exe_dst = rd32_safe(0x1cb33cu);
            emu::logf(emu::LogLevel::debug, "DMA3_SNAP",
                "[%u] PC=0x%08X madr=0x%08X words=%u cd_dest=0x%08X cd_cb=0x%08X ldst=%d exe_dst=0x%08X",
                dma3_game_count, cpu_pc_, dma_[3].madr, words,
                cd_dest, cd_cb, (int32_t)ldst, exe_dst);
        }
    }

    // NOTE: games may intentionally DMA to low RAM (0x0000-0xFFFF) to replace the BIOS
    // kernel area with game-specific code (e.g., Tekken's Galaga sub-EXE at KUSEG 0).
    if (ma < 0x200u)
    {
        emu::logf(emu::LogLevel::debug, "BUS",
            "DMA3 low-RAM: madr=0x%05X words=%u (intentional kernel overwrite?) PC=0x%08X",
            ma, words, cpu_pc_);
    }

    for (uint32_t i = 0; i < words; ++i)
    {
        const uint8_t b0 = cdrom_->mmio_read8(0x1F801802u);
        const uint8_t b1 = cdrom_->mmio_read8(0x1F801802u);
        const uint8_t b2 = cdrom_->mmio_read8(0x1F801802u);
        const uint8_t b3 = cdrom_->mmio_read8(0x1F801802u);
        const uint32_t w = (uint32_t)b0 | ((uint32_t)b1 << 8) |
                           ((uint32_t)b2 << 16) | ((uint32_t)b3 << 24);
        if (ma >= 0x0000E000u && ma < 0x0000F000u)
        {
            static uint32_t dma3_evt_hit = 0;
            if (dma3_evt_hit < 16u)
            {
                ++dma3_evt_hit;
                emu::logf(emu::LogLevel::debug, "DMA3_EVT",
                    "[%u] DMA3 writing phys=0x%05X val=0x%08X madr_orig=0x%08X word=%u/%u pc=0x%08X",
                    dma3_evt_hit, ma, w, dma_[3].madr, i, words, cpu_pc_);
            }
        }
        ram_[ma]     = (uint8_t)(w & 0xFF);
        ram_[ma + 1] = (uint8_t)((w >> 8) & 0xFF);
        ram_[ma + 2] = (uint8_t)((w >> 16) & 0xFF);
        ram_[ma + 3] = (uint8_t)((w >> 24) & 0xFF);
        ma = (ma + 4) & 0x1FFFFF;
    }

    if (cpu_pc_ >= 0xBFC06000u && cpu_pc_ <= 0xBFC06800u && bios_cd_dma_dump_count_ < 12)
    {
        ++bios_cd_dma_dump_count_;
        bios_pvd_post_pc_trace_count_ = 512u;
        bios_pvd_post_last_pc_ = 0xFFFFFFFFu;
        if (cdrom_)
        {
            const uint32_t read_lba = cdrom_->debug_read_lba();
            const uint32_t data_lba = cdrom_->debug_data_lba();
            if (read_lba == 60643u || data_lba == 60643u)
            {
                bios_post_60643_trace_active_ = 1u;
                bios_post_60643_exit_logged_ = 0u;
            }
        }

        char hexbuf[3 * 16 + 1]{};
        char asciibuf[16 + 1]{};
        for (uint32_t i = 0; i < 16; ++i)
        {
            const uint8_t b = ram_[(start_ma + i) & 0x1FFFFF];
            std::snprintf(&hexbuf[i * 3], 4, "%02X ", (unsigned)b);
            asciibuf[i] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
        }
        asciibuf[16] = '\0';

        emu::logf(emu::LogLevel::debug, "BUS",
            "DMA3 BIOS RAM dump pc=0x%08X madr=0x%08X start=0x%05X end=0x%05X words=%u read_lba=%u data_lba=%u last_cmd=0x%02X data=%s ascii='%s'",
            cpu_pc_, dma_[3].madr, start_ma, end_addr, words,
            cdrom_ ? (unsigned)cdrom_->debug_read_lba() : 0u,
            cdrom_ ? (unsigned)cdrom_->debug_data_lba() : 0u,
            cdrom_ ? (unsigned)cdrom_->debug_last_cmd() : 0u,
            hexbuf, asciibuf);

        const uint32_t probe0 = 0x0B888u;
        const uint32_t probe1 = 0x0B88Cu;
        const uint32_t range_end = (start_ma + words * 4u - 1u) & 0x1FFFFFu;
        const bool linear = range_end >= start_ma;
        const auto in_dma_range = [&](uint32_t addr) -> bool
        {
            if (linear) return addr >= start_ma && addr <= range_end;
            return addr >= start_ma || addr <= range_end;
        };
        if (in_dma_range(probe0) || in_dma_range(probe1))
        {
            const uint32_t v0 = (uint32_t)ram_[probe0] | ((uint32_t)ram_[probe0+1]<<8)
                              | ((uint32_t)ram_[probe0+2]<<16) | ((uint32_t)ram_[probe0+3]<<24);
            const uint32_t v1 = (uint32_t)ram_[probe1] | ((uint32_t)ram_[probe1+1]<<8)
                              | ((uint32_t)ram_[probe1+2]<<16) | ((uint32_t)ram_[probe1+3]<<24);
            emu::logf(emu::LogLevel::debug, "BUS",
                "DMA3 BIOS scratch hit pc=0x%08X madr=0x%08X start=0x%05X end=0x%05X read_lba=%u data_lba=%u b888=0x%08X b88c=0x%08X",
                cpu_pc_, dma_[3].madr, start_ma, end_addr,
                cdrom_ ? (unsigned)cdrom_->debug_read_lba() : 0u,
                cdrom_ ? (unsigned)cdrom_->debug_data_lba() : 0u,
                (unsigned)v0, (unsigned)v1);
        }
    }

    // Log STR header for 8-word DMA3 (header read)
    if (words == 8 && cdrom_->is_streaming_mode())
    {
        static uint32_t str_hdr_cnt = 0;
        if (str_hdr_cnt < 60)
        {
            ++str_hdr_cnt;
            // Read back the 32 bytes from RAM
            auto rd16 = [&](uint32_t p) -> uint16_t {
                return (uint16_t)ram_[p] | ((uint16_t)ram_[p+1] << 8);
            };
            const uint32_t base = start_ma;
            const uint16_t magic    = rd16(base + 0);
            const uint16_t type     = rd16(base + 2);
            const uint16_t chunk_id = rd16(base + 4);
            const uint16_t n_chunks = rd16(base + 6);
            const uint32_t frame_no = (uint32_t)ram_[base+8] | ((uint32_t)ram_[base+9]<<8) |
                                      ((uint32_t)ram_[base+10]<<16) | ((uint32_t)ram_[base+11]<<24);
            const uint16_t demux_sz = rd16(base + 12);
            const uint16_t width    = rd16(base + 16);
            const uint16_t height   = rd16(base + 18);
            emu::logf(emu::LogLevel::warn, "STRHDR",
                "[%u] lba=%u madr=0x%05X magic=0x%04X type=0x%04X chunk=%u/%u frame=%u demux=%u %ux%u last=%d",
                str_hdr_cnt, cdrom_->debug_data_lba(), start_ma,
                magic, type, chunk_id, n_chunks, frame_no, demux_sz, width, height,
                (chunk_id == n_chunks - 1) ? 1 : 0);
        }
    }

    // Dump DMA3 content for BIGFILE.DAT analysis (first 5 transfers after LBA 400)
    {
        static int dma3_dump_count = 0;
        const uint32_t data_lba = cdrom_ ? cdrom_->debug_data_lba() : 0;
        if (dma3_dump_count < 8 && data_lba >= 410 && data_lba <= 1025)
        {
            ++dma3_dump_count;
            const uint32_t base = start_ma;
            const uint32_t w0 = *(uint32_t*)(ram_ + base);
            const uint32_t w1 = *(uint32_t*)(ram_ + base + 4);
            const uint32_t w2 = *(uint32_t*)(ram_ + base + 8);
            const uint32_t w3 = (words > 3) ? *(uint32_t*)(ram_ + base + 12) : 0;
            emu::logf(emu::LogLevel::warn, "DMA3_DUMP",
                "[%d] lba=%u madr=0x%08X words=%u RAM[0:16]=%08X %08X %08X %08X",
                dma3_dump_count, data_lba, dma_[3].madr, words,
                w0, w1, w2, w3);
        }
    }

    dma_finish(3);
    cdrom_->debug_log_dma3_end(dma_[3].madr, words, 0);

    // After streaming DMA3: advance CDROM time so the next sector
    // arrives before the game reads the ring buffer. This compensates
    // for our CPU running faster than real PS1 (no cache/pipeline stalls).
    // Only for ReadS (streaming) — normal ReadN boot must NOT be affected.
    if (cdrom_->is_reading_active() && cdrom_->is_streaming_mode())
    {
        // No CD tick from DMA3 — let Bus::tick() in the main loop handle it.
        // The tick_clock_only caused read_lba to cascade when deliveries
        // caught up with the advanced clock.
    }

    // Check if read buffer is consumed — promote next buffer if available.
    cdrom_->check_sector_read_complete();
    check_cdrom_irq_edge();
}

// ================== DMA completion ==================

void Bus::dma_finish(int ch)
{
    dma_[ch].chcr &= ~0x01000000u; // clear busy/trigger bit

    // DuckStation/PS1 behavior: completion only raises the per-channel IRQ flag
    // when both the channel IRQ enable bit and the DMA master enable bit are set.
    const int master_en = (dicr_ >> 23) & 1;
    const int ch_irq_en = (dicr_ >> (16 + ch)) & 1;
    if (master_en && ch_irq_en)
        dicr_ |= (1u << (24 + ch));

    // Recompute master flag (bit 31)
    // DuckStation: master_flag = force_irq || (master_enable && ANY_flag_set)
    // Note: master flag triggers on ANY flag, not just enabled ones.
    const uint32_t flags   = (dicr_ >> 24) & 0x7Fu;
    const int force = (dicr_ >> 15) & 1;
    const int old_master_flag = (dicr_ >> 31) & 1;
    if (force || (master_en && flags))
        dicr_ |= (1u << 31);
    else
        dicr_ &= ~(1u << 31);
    const int new_master_flag = (dicr_ >> 31) & 1;

    // Raise DMA IRQ (I_STAT bit 3) on 0→1 transition of master flag.
    if (!old_master_flag && new_master_flag)
        i_stat_ |= (1u << 3);

    // Debug: log DMA completion state for channels 3 and 4 (CDROM/SPU)
    if (ch == 3 || ch == 4)
    {
        emu::logf(emu::LogLevel::debug, "BUS",
            "DMA%d finish: DICR=0x%08X flags=0x%02X master_en=%d ch_en=%d force=%d flag_set=%d irq_fired=%d",
            ch, dicr_, flags, master_en, ch_irq_en, force, new_master_flag, (!old_master_flag && new_master_flag) ? 1 : 0);
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
        emu::logf(emu::LogLevel::debug, "BUS", "CDROM IRQ edge: i_stat=0x%04X", (unsigned)i_stat_);
        cdrom_->debug_log_bus_irq_latched(i_stat_, i_mask_);
    }
    cdrom_irq_prev_ = cdirq;
}

// ================== EXTERNAL VBLANK / IRQ THREADS ==================

void Bus::set_external_vblank(bool enabled)
{
    external_vblank_ = enabled;
    if (enabled)
    {
        // Start IRQ threads: real-time hardware timing
        if (cdrom_) cdrom_->start_sector_thread();
        start_gpu_thread(true); // PAL default — TODO: detect from disc region
        start_timer_threads();
        emu::logf(emu::LogLevel::warn, "BUS", "IRQ threads started (GPU + CDROM + Timers)");
    }
    else
    {
        if (cdrom_) cdrom_->stop_sector_thread();
        stop_gpu_thread();
        stop_timer_threads();
    }
}

void Bus::fire_vblank_external()
{
    // IRQ thread model: set VBlank bit via atomic pending register.
    fire_irq_external(0); // I_STAT bit 0 = VBlank

    // GPU draw list swap (needed for UE5 rendering bridge — safe from any thread)
    if (gpu_)
        gpu_->tick_vblank_swap_only();
}

void Bus::fire_hblank_external()
{
    // Timer 1 external clock: increment count on each HBlank
    Timer& t1 = timers_[1];
    if (t1.counting_enabled && t1.use_external_clock)
    {
        const uint32_t old_count = t1.count;
        t1.count += 1;

        // Check target/overflow — fire IRQ via atomic if needed
        bool irq = false;
        if (t1.count >= t1.target && (old_count < t1.target || t1.target == 0))
        {
            if (t1.mode & 0x0010u) irq = true;
            t1.mode |= 0x0800u;
            if (t1.mode & 0x0008u)
                t1.count %= ((uint32_t)t1.target + 1u);
        }
        if (t1.count > 0xFFFFu)
        {
            if (t1.mode & 0x0020u) irq = true;
            t1.mode |= 0x1000u;
            t1.count &= 0xFFFFu;
        }
        if (irq)
        {
            const bool pulse_n = (t1.mode & 0x0080u) != 0;
            const bool repeat  = (t1.mode & 0x0040u) != 0;
            if (!pulse_n)
            {
                if (!t1.irq_done || repeat)
                    fire_irq_external(5); // I_STAT bit 5 = Timer 1
                t1.irq_done = true;
                t1.mode |= 0x0400u;
            }
            else
            {
                t1.mode ^= 0x0400u;
                if (!(t1.mode & 0x0400u))
                    fire_irq_external(5);
            }
        }
    }

    // TODO: Timer 0/1 gate signals (HBlank gate for Timer 0, VBlank gate for Timer 1)
}

void Bus::start_gpu_thread(bool pal)
{
    stop_gpu_thread();
    gpu_thread_pal_ = pal;
    gpu_thread_running_.store(true, std::memory_order_release);
    gpu_thread_ = std::thread([this]() {
        // Scanline timing:
        // PAL:  314 scanlines/frame, 50 frames/s → ~63.7µs per scanline
        // NTSC: 263 scanlines/frame, 60 frames/s → ~63.5µs per scanline
        const uint32_t scanlines_per_frame = gpu_thread_pal_ ? 314u : 263u;
        const uint32_t vblank_start = gpu_thread_pal_ ? 288u : 240u;
        const auto scanline_interval = gpu_thread_pal_
            ? std::chrono::nanoseconds(63694)  // 20ms / 314
            : std::chrono::nanoseconds(63492); // 16.67ms / 263

        emu::logf(emu::LogLevel::warn, "GPU_THREAD",
            "Started (%s, %u scanlines, VBlank@%u, %lld ns/line)",
            gpu_thread_pal_ ? "PAL" : "NTSC",
            scanlines_per_frame, vblank_start,
            (long long)scanline_interval.count());

        uint32_t scanline = 0;
        auto next = std::chrono::steady_clock::now() + scanline_interval;

        while (gpu_thread_running_.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_until(next);
            next += scanline_interval;

            // HBlank fires at end of each scanline
            fire_hblank_external();

            scanline++;
            if (scanline == vblank_start)
            {
                // VBlank start
                fire_vblank_external();
            }
            if (scanline >= scanlines_per_frame)
            {
                scanline = 0;
            }
        }

        emu::logf(emu::LogLevel::warn, "GPU_THREAD", "Stopped");
    });
}

void Bus::stop_gpu_thread()
{
    if (gpu_thread_running_.load(std::memory_order_acquire))
    {
        gpu_thread_running_.store(false, std::memory_order_release);
        if (gpu_thread_.joinable())
            gpu_thread_.join();
    }
}

// ================== SIO0 THREAD ==================

void Bus::sio0_wake_thread(uint8_t reason)
{
    sio0_wake_flag_.store(reason, std::memory_order_release);
    sio0_wake_cv_.notify_one();
}

void Bus::start_sio0_thread()
{
    stop_sio0_thread();
    sio0_thread_running_.store(true, std::memory_order_release);
    sio0_thread_ = std::thread([this]() {
        emu::logf(emu::LogLevel::warn, "SIO0_THREAD", "Started");
        while (sio0_thread_running_.load(std::memory_order_acquire))
        {
            // Wait for wake signal (zero-latency via condition_variable)
            {
                std::unique_lock<std::mutex> lk(sio0_wake_mutex_);
                sio0_wake_cv_.wait(lk, [this]() {
                    return sio0_wake_flag_.load(std::memory_order_acquire) != 0 ||
                           !sio0_thread_running_.load(std::memory_order_acquire);
                });
            }
            if (!sio0_thread_running_.load(std::memory_order_acquire)) break;

            const uint8_t reason = sio0_wake_flag_.exchange(0, std::memory_order_acquire);
            if (reason == 1)
            {
                // Transfer requested: sleep for transfer duration
                const uint32_t ns = sio0_transfer_delay_ns_.load(std::memory_order_acquire);
                if (ns > 0)
                    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
                sio0_transfer_signal_.store(1, std::memory_order_release);
            }
            else if (reason == 2)
            {
                // ACK requested: sleep for ACK delay
                const uint32_t ns = sio0_ack_delay_ns_.load(std::memory_order_acquire);
                if (ns > 0)
                    std::this_thread::sleep_for(std::chrono::nanoseconds(ns));
                sio0_transfer_signal_.store(2, std::memory_order_release);
            }
        }
        emu::logf(emu::LogLevel::warn, "SIO0_THREAD", "Stopped");
    });
}

void Bus::stop_sio0_thread()
{
    if (sio0_thread_running_.load(std::memory_order_acquire))
    {
        sio0_thread_running_.store(false, std::memory_order_release);
        sio0_wake_cv_.notify_one(); // wake thread so it exits
        if (sio0_thread_.joinable())
            sio0_thread_.join();
    }
}

// ================== TIMER THREADS ==================

void Bus::timer_thread_func(int ch)
{
    emu::logf(emu::LogLevel::warn, "TMR_THREAD", "Timer %d thread started", ch);

    while (timer_threads_running_.load(std::memory_order_acquire))
    {
        Timer& t = timers_[ch];

        if (!t.counting_enabled ||
            t.use_external_clock)
        {
            // Not counting or using external clock (HBlank/dotclock handled elsewhere)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
            continue;
        }

        // Compute ticks until next IRQ event
        const uint16_t mode = t.mode;
        const uint16_t target = t.target;
        const uint32_t count = t.count;
        const bool irq_at_target = (mode & 0x0010u) != 0;
        const bool irq_on_overflow = (mode & 0x0020u) != 0;

        if (!irq_at_target && !irq_on_overflow)
        {
            // No IRQ configured — sleep and recheck
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }

        // Determine clock period
        double ns_per_tick = kSysclkPeriodNs;
        if (ch == 2 && t.use_external_clock)
            ns_per_tick = kSysclk8PeriodNs;

        // Calculate ticks until next event
        uint32_t ticks_to_event = 0xFFFF; // default: overflow from current count

        if (irq_at_target && target > 0 && count < target)
            ticks_to_event = target - count;
        else if (irq_on_overflow && count <= 0xFFFF)
            ticks_to_event = 0x10000 - count;

        if (ticks_to_event == 0) ticks_to_event = 1;

        // Sleep for the computed duration
        const auto sleep_ns = std::chrono::nanoseconds((int64_t)(ticks_to_event * ns_per_tick));

        // Cap sleep to 2ms max — recheck config regularly
        const auto max_sleep = std::chrono::microseconds(2000);
        const auto actual_sleep = (sleep_ns < max_sleep) ? sleep_ns : max_sleep;

        std::this_thread::sleep_for(actual_sleep);

        // Check if game reconfigured the timer while we slept
        if (t.reconfig.exchange(0, std::memory_order_acquire))
            continue; // recalculate

        // If we slept the full duration (not capped), fire the IRQ
        if (sleep_ns <= max_sleep)
        {
            // Advance count
            t.count = (count + ticks_to_event) & 0xFFFF;

            // Fire IRQ via atomic
            const bool pulse_n = (mode & 0x0080u) != 0;
            const bool repeat = (mode & 0x0040u) != 0;
            if (!pulse_n)
            {
                if (!t.irq_done || repeat)
                    fire_irq_external(4 + ch);
                t.irq_done = true;
                t.mode |= 0x0400u;
            }
            else
            {
                uint16_t old_mode = t.mode; t.mode ^= 0x0400u;
                if (old_mode & 0x0400u) // was 1, now 0 → fire
                    fire_irq_external(4 + ch);
            }

            // Set reached flags
            if (irq_at_target && target > 0 && (count + ticks_to_event) >= target)
                t.mode |= 0x0800u;
            if ((count + ticks_to_event) > 0xFFFF)
                t.mode |= 0x1000u;
        }
    }

    emu::logf(emu::LogLevel::warn, "TMR_THREAD", "Timer %d thread stopped", ch);
}

void Bus::start_timer_threads()
{
    stop_timer_threads();
    timer_threads_running_.store(true, std::memory_order_release);
    for (int ch = 0; ch < 3; ++ch)
        timer_threads_[ch] = std::thread(&Bus::timer_thread_func, this, ch);
}

void Bus::stop_timer_threads()
{
    if (timer_threads_running_.load(std::memory_order_acquire))
    {
        timer_threads_running_.store(false, std::memory_order_release);
        for (int ch = 0; ch < 3; ++ch)
            if (timer_threads_[ch].joinable())
                timer_threads_[ch].join();
    }
}

// ================== TICK ==================

void Bus::tick_peripherals(uint32_t cycles)
{
    // When external_vblank_ is active, the fast path in tick() handles
    // SIO0, CDROM, DMA3, all timers per-instruction. tick_peripherals
    // handles GPU, SPU, Timer 0/1 external clock, VBlank.

    if (!external_vblank_)
    {
        tick(cycles);
        return;
    }

    // GPU scanline + Timer 1 HBlank: handled by GPU thread (fire_hblank_external).
    // Timer 0 dotclock: still ticked in fast path (per instruction).

    // ---- SPU ----
    if (cycles != 0)
    {
        if (spu_apply_delay_ > 0) { if (cycles >= spu_apply_delay_) { spu_apply_delay_ = 0; spu_cnt_applied_ = spu_cnt_reg_; } else spu_apply_delay_ -= cycles; }
        if (spu_busy_delay_ > 0) { if (cycles >= spu_busy_delay_) { spu_busy_delay_ = 0; spu_busy_ = 0; } else spu_busy_delay_ -= cycles; }
    }
    if (spu_) spu_->tick_cycles(cycles);
}

void Bus::tick(uint32_t cycles)
{
    // In external peripheral mode, only tick SIO0 (needs per-instruction
    // precision for ACK timing) and return. Everything else is handled by
    // tick_peripherals() called every ~256 cycles from the worker thread.
    if (external_vblank_)
    {
        // Consume IRQ bits from external threads (atomic exchange — thread-safe).
        // Each bit set by fire_irq_external() is edge-latched into i_stat_.
        {
            const uint32_t ext = irq_ext_pending_.exchange(0, std::memory_order_acquire);
            if (ext)
            {
                // Edge-trigger: only set bits that aren't already pending
                const uint32_t new_bits = ext & ~i_stat_;
                i_stat_ |= new_bits;
                // VBlank bookkeeping (bit 0)
                if (ext & (1u << 0))
                {
                    ++vblank_total_count_;
                    if (gte_3d_) gte_3d_->swap_frame();
                    if (gpu_3d_) gpu_3d_->on_vblank();
                    if (hooks_ && hooks_->has_vblank())
                        hooks_->fire_vblank(vblank_total_count_);
                }
            }
        }

        // Tick CDROM + hardware timers every instruction in external_vblank mode.
        // Without CDROM tick: CdSync loops forever (timer-driven delivery stalls).
        // Without timer tick: chkRC2wait() timeout never fires (Timer 2 frozen).
        if (cdrom_)
        {
            cdrom_->tick(cycles);
            check_cdrom_irq_edge();
        }
        // Soul Reaver game state dump (periodic)
        if (vblank_total_count_ > 800 && (vblank_total_count_ % 100) == 0)
        {
            static uint32_t sr_state_last_vbl = 0;
            if (vblank_total_count_ != sr_state_last_vbl)
            {
                sr_state_last_vbl = vblank_total_count_;
                const uint32_t state = *(uint32_t*)(ram_ + (0x800d19acu & (ram_size_ - 1)));
                const uint32_t vidx = *(uint32_t*)(ram_ + (0x800d19b4u & (ram_size_ - 1)));
                const uint32_t dd9c0 = *(uint32_t*)(ram_ + (0x800dd9c0u & (ram_size_ - 1)));
                const uint32_t cb6e4 = *(uint32_t*)(ram_ + (0x800cb6e4u & (ram_size_ - 1)));
                const uint32_t dd9ac = *(uint32_t*)(ram_ + (0x800dd9acu & (ram_size_ - 1)));
                const uint32_t dma1_chcr = dma_[1].chcr;
                emu::logf(emu::LogLevel::warn, "CORE",
                    "SR_STATE vbl=%u st=%u vi=%u d9c0=%u d9ac=%u cb6e4=0x%08X dma1=0x%08X pc=0x%08X",
                    vblank_total_count_, state, vidx, dd9c0, dd9ac, cb6e4, dma1_chcr, cpu_pc_);
            }
        }

        // Tick hardware timers — skip when timer threads handle IRQs
        // (count is computed on-read via timer_compute_count)
        if (!timer_threads_running_.load(std::memory_order_relaxed))
        for (int ch = 0; ch < 3; ++ch)
        {
            Timer& t = timers_[ch];
            if (!t.counting_enabled) continue;
            uint32_t inc = cycles;
            if (t.use_external_clock)
            {
                if (ch == 2) { timer_prescale_accum_[2] += cycles; inc = timer_prescale_accum_[2] / 8; timer_prescale_accum_[2] %= 8; }
                else continue; // dotclock/hblank handled in full tick
            }
            if (inc == 0) continue;
            const uint32_t old_count = t.count;
            t.count += inc;
            timer_check_irq(ch, old_count);
        }

        // DMA3 deferred check: if DMA3 was triggered but CDROM FIFO was empty,
        // check if FIFO is now populated and execute the transfer.
        if (dma3_pending_ && cdrom_ && !cdrom_->is_fifo_empty())
        {
            dma3_pending_ = 0;
            exec_dma3_transfer();
        }

        // SIO0: thread signal or legacy cycle countdown
        if (sio0_thread_running_.load(std::memory_order_relaxed))
        {
            const uint8_t sig = sio0_transfer_signal_.load(std::memory_order_acquire);
            if (sig == 1) { sio0_transfer_signal_.store(0, std::memory_order_release); sio0_do_transfer(); }
            else if (sig == 2) { sio0_transfer_signal_.store(0, std::memory_order_release); sio0_do_ack(); }
        }
        else
        {
            if (sio0_state_ == Sio0State::Transmitting && sio0_transfer_countdown_ > 0)
            {
                if (cycles >= sio0_transfer_countdown_) { sio0_transfer_countdown_ = 0; sio0_do_transfer(); }
                else sio0_transfer_countdown_ -= cycles;
            }
            else if (sio0_state_ == Sio0State::WaitingForACK && sio0_ack_countdown_ > 0)
            {
                if (cycles >= sio0_ack_countdown_) { sio0_ack_countdown_ = 0; sio0_do_ack(); }
                else sio0_ack_countdown_ -= cycles;
            }
        }
        // Soul Reaver trace: PC + key state every ~2M cycles during stall
        {
            static uint64_t sr_last_cyc = 0;
            // One-shot: log at VBL 100 to confirm fast path runs
            {
                static int fp_alive = 0;
                if (!fp_alive && vblank_total_count_ >= 100)
                {
                    fp_alive = 1;
                    emu::logf(emu::LogLevel::warn, "SR_ALIVE",
                        "VBL=%u pc=0x%08X ext_vbl=%d cdrom=%d batch=%u",
                        vblank_total_count_, cpu_pc_, (int)external_vblank_,
                        cdrom_ ? 1 : 0, 0u);
                }
            }
            if (cdrom_ && vblank_total_count_ >= 400 && vblank_total_count_ <= 2000)
            {
                static uint32_t sr_fp_cnt = 0;
                const uint64_t now = cdrom_->now_cycles_debug();
                if (sr_fp_cnt < 80 && now > sr_last_cyc + 2000000)
                {
                    sr_last_cyc = now;
                    ++sr_fp_cnt;
                    const uint32_t gv = *(uint32_t*)(ram_ + (0x800cda48u & (ram_size_ - 1)));
                    const uint32_t evt = *(uint16_t*)(ram_ + (0x800cb782u & (ram_size_ - 1)));
                    const uint32_t sync = *(uint8_t*)(ram_ + (0x800cd5bcu & (ram_size_ - 1)));
                    // Software IRQ mask used by BIOS handler: I_MASK & DAT_800cb7b0 & I_STAT
                    const uint32_t sw_mask = *(uint32_t*)(ram_ + (0x800cb7b0u & (ram_size_ - 1)));
                    // CDROM handler pointer (index 2 in handler table at 0x800cb784)
                    const uint32_t cd_handler = *(uint32_t*)(ram_ + (0x800cb78cu & (ram_size_ - 1)));
                    // Intr timeout counter
                    const uint32_t intr_timeout = *(uint32_t*)(ram_ + (0x800cc818u & (ram_size_ - 1)));
                    // Timer 2 state (chkRC2wait uses this for timeout)
                    const Timer& t2 = timers_[2];
                    cdrom_->log_external("SR_TRACE VBL=%u pc=0x%08X gvsync=%u evt=%u sync=%u irq=0x%02X istat=0x%04X imask=0x%04X sw_mask=0x%04X cd_h=0x%08X intr_to=%u T2:cnt=%u mode=0x%04X tgt=%u en=%d ext=%d",
                        vblank_total_count_, cpu_pc_, gv, evt, sync,
                        cdrom_->irq_flags_debug(), i_stat_, i_mask_, sw_mask, cd_handler, intr_timeout,
                        t2.count, t2.mode, t2.target, (int)t2.counting_enabled, (int)t2.use_external_clock);
                }
            }
        }
        return;
    }

    if (bios_post_60643_trace_active_ &&
        !bios_post_60643_exit_logged_ &&
        cpu_pc_ < 0xBFC00000u)
    {
        bios_post_60643_exit_logged_ = 1u;
        emu::logf(
            emu::LogLevel::warn,
            "BIOSXFER",
            "First non-BIOS PC after LBA60643 DMA: pc=0x%08X i_stat=0x%04X i_mask=0x%04X",
            cpu_pc_,
            (unsigned)i_stat_,
            (unsigned)i_mask_);
    }

    if (bios_pvd_post_pc_trace_count_ != 0u &&
        cpu_pc_ >= 0xBFC00000u && cpu_pc_ < 0xBFC80000u &&
        cpu_pc_ != bios_pvd_post_last_pc_)
    {
        bios_pvd_post_last_pc_ = cpu_pc_;
        --bios_pvd_post_pc_trace_count_;
        emu::logf(
            emu::LogLevel::warn,
            "PVDPC",
            "pc=0x%08X i_stat=0x%04X i_mask=0x%04X",
            cpu_pc_,
            (unsigned)i_stat_,
            (unsigned)i_mask_);
    }

    // Must wait long enough for BIOS to install SysEnqIntRP chain handlers
    // before enabling IRQs.  600k was FAR too early (fired before VBlank #1,
    // causing infinite exception loop because no handler acknowledged I_STAT).
    // 30 PAL frames ≈ 30*680688 ≈ 20M cycles — well past BIOS init.
    static constexpr uint32_t kForceMaskAfterCycles = 30u * 680688u;
    uint32_t gpu_scanline_delta = 0;
    bool gpu_vblank_fired = false;

    if (gpu_)
    {
        // Always tick scanline counter (needed for GPUSTAT polling).
        const uint32_t old_scanline = gpu_->current_scanline();
        const uint32_t total_scanlines = gpu_->total_scanlines();
        gpu_vblank_fired = gpu_->tick_vblank(cycles) != 0;
        const uint32_t new_scanline = gpu_->current_scanline();
        if (total_scanlines != 0u)
            gpu_scanline_delta = (new_scanline + total_scanlines - old_scanline) % total_scanlines;
    }

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
                // HBlank: phase-lock Timer 1 to the actual GPU scanline
                // progression so TMR1 and GPUSTAT bit31 advance together.
                if (gpu_)
                {
                    inc = gpu_scanline_delta;
                }
                else
                {
                    const uint32_t frame_cycles = 571088u;
                    const uint32_t lines_per_frame = 263u;
                    timer_prescale_accum_[1] += cycles * lines_per_frame;
                    inc = timer_prescale_accum_[1] / frame_cycles;
                    timer_prescale_accum_[1] %= frame_cycles;
                }
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

    // Handle GPU VBlank edge latched at the start of this tick.
    // When external_vblank_ is active, the IRQ + draw swap are done by
    // fire_vblank_external() at a fixed rate. Diagnostics still run here.
    if (gpu_vblank_fired)
    {
            if (!external_vblank_)
            {
                if (!(i_stat_ & (1u << 0))) // edge-triggered: don't re-set if pending
                    i_stat_ |= (1u << 0);
                ++vblank_total_count_;
            }

#ifndef R3000_NO_DIAG
            // Dump BIOS IRQ handler chain once at vblank 200
            // The BIOS stores priority chain head pointers at 0x0100-0x010F
            // Each entry: +0=next, +4=handler_func, +8=verifier
            if (vblank_total_count_ == 200)
            {
                auto rd32 = [&](uint32_t p) -> uint32_t {
                    if (p + 4 > ram_size_) return 0xDEADu;
                    return (uint32_t)ram_[p] | ((uint32_t)ram_[p+1]<<8)
                         | ((uint32_t)ram_[p+2]<<16) | ((uint32_t)ram_[p+3]<<24);
                };
                emu::logf(emu::LogLevel::debug, "IRQ_CHAIN",
                    "VBL#200 i_stat=0x%04X i_mask=0x%04X",
                    i_stat_, i_mask_);
                // Dump priority chain heads (4 priorities × 4 bytes at 0x100)
                for (int p = 0; p < 4; p++)
                {
                    uint32_t head = rd32(0x100 + p * 4);
                    emu::logf(emu::LogLevel::debug, "IRQ_CHAIN",
                        "  Priority[%d] head=0x%08X", p, head);
                    // Walk chain (max 8 entries)
                    uint32_t ptr = head;
                    for (int j = 0; j < 8 && ptr != 0 && ptr != 0xFFFFFFFF; j++)
                    {
                        uint32_t phys = ptr & 0x1FFFFF;
                        if (phys + 12 > ram_size_) break;
                        uint32_t next = rd32(phys);
                        uint32_t func = rd32(phys + 4);
                        uint32_t verf = rd32(phys + 8);
                        emu::logf(emu::LogLevel::debug, "IRQ_CHAIN",
                            "    [%d] @0x%08X next=0x%08X func=0x%08X verifier=0x%08X",
                            j, ptr, next, func, verf);
                        ptr = next;
                    }
                }
            }

#endif // R3000_NO_DIAG — IRQ chain dump

            // Shadow 3D systems: swap buffers at VBlank
            // (handled by fire_vblank_external when external_vblank_ is active)
            if (!external_vblank_)
            {
                if (gte_3d_) gte_3d_->swap_frame();
                if (gpu_3d_) gpu_3d_->on_vblank();
            }

            // Fire VBlank hooks (handled by fire_vblank_external when external)
            if (!external_vblank_ && hooks_ && hooks_->has_vblank())
                hooks_->fire_vblank(vblank_total_count_);

#ifndef R3000_NO_DIAG
            // Per-VBlank: scan CDROM event status fields, log when any changes to 0x4000
            // Events[0-4] at 0xE028+i*0x1C, status at offset+4
            {
                static uint32_t last_cdrom_status[5] = {0,0,0,0,0};
                static uint32_t cdrom_flip_count = 0;
                const uint32_t bases[5] = {0xE028u, 0xE044u, 0xE060u, 0xE07Cu, 0xE098u};
                for (int ei = 0; ei < 5; ++ei)
                {
                    const uint32_t soff = bases[ei] + 4u;
                    const uint32_t st = (uint32_t)ram_[soff] | ((uint32_t)ram_[soff+1] << 8) |
                                        ((uint32_t)ram_[soff+2] << 16) | ((uint32_t)ram_[soff+3] << 24);
                    if (st != last_cdrom_status[ei] && cdrom_flip_count < 64u)
                    {
                        ++cdrom_flip_count;
                        emu::logf(emu::LogLevel::debug, "EVT_FLIP",
                            "[%u] Event[%d]@0x%05X status 0x%04X->0x%04X vblank=%u pc=0x%08X",
                            cdrom_flip_count, ei, bases[ei], last_cdrom_status[ei], st,
                            vblank_total_count_, cpu_pc_);
                        last_cdrom_status[ei] = st;
                    }
                }
            }

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

                // After VBlank 550 (post-loading phase): log DMA2 at warn level so it's
                // always visible. This tells us if the title screen code issues GPU transfers.
                const bool dma2_verbose = (vblank_total_count_ > 550u) ||
                                          (vblank_total_count_ < 10u) ||
                                          ((vblank_total_count_ % 60u) == 0u);
                if (dma2_verbose)
                {
                const auto lvl = (vblank_total_count_ > 550u) ? emu::LogLevel::warn : emu::LogLevel::debug;
                const size_t n = (top.size() < 6) ? top.size() : 6;
                emu::logf(
                    lvl,
                    "DMA2_NOHINT",
                    "vblank=%u nohint_words=%u unique_pcs=%u",
                    vblank_total_count_,
                    dma2_nohint_words_,
                    (uint32_t)top.size());
                for (size_t i = 0; i < n; ++i)
                {
                    emu::logf(
                        lvl,
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

            // ===== TEKKEN STATE MACHINE DUMP (VBlank 1-5000) =====
            // Monitor loading state machine, sound lookup result, and title screen transition.
            // Physical addresses (virtual - 0x80000000):
            //   0x1b72cc = DAT_801b72cc  — sound seq lookup result (-1=not found, 1=found)
            //   0x1cb3d4 = DAT_801cb3d4  — loading state machine (0-3, -1=error)
            //   0x1cb3dc = DAT_801cb3dc  — loading sub-state (0-5)
            //   0x1cb2ec = DAT_801cb2ec  — main loop counter (starts 2)
            //   0x1cb2fc = DAT_801cb2fc  — animation counter (0 or 1)
            //   0x1ca7c8 = PSYQ VBlank counter (offset -0x5838 from 0x801D0000)
            //   0x1d6190 = DAT_801d6190  — set to 0x801FFF00 by FUN_8015d4e0 (title screen loader)
            //              non-zero means the code-copy step was executed and 0x80140400 is running
            {
                // Sample every 20 VBlanks up to 560, then every 10 up to 1000, every 50 normally.
                // Extra: every VBlank between 2430-2560 to capture the vsync_ctr freeze transition.
                const bool should_sample =
                    (vblank_total_count_ >= 1u && vblank_total_count_ <= 5000u) &&
                    ((vblank_total_count_ <= 560u  && (vblank_total_count_ % 20u) == 0u) ||
                     (vblank_total_count_ <= 1000u && (vblank_total_count_ % 10u) == 0u) ||
                     (vblank_total_count_ >= 2430u && vblank_total_count_ <= 2560u) ||
                     (vblank_total_count_ <= 5000u && (vblank_total_count_ % 50u) == 0u));
                if (should_sample)
                {
                    auto rd32 = [&](uint32_t phys) -> uint32_t {
                        if (phys + 3u >= ram_size_) return 0u;
                        return (uint32_t)ram_[phys] | ((uint32_t)ram_[phys+1]<<8) |
                               ((uint32_t)ram_[phys+2]<<16) | ((uint32_t)ram_[phys+3]<<24);
                    };
                    const int32_t b72cc = (int32_t)rd32(0x1b72ccu); // sound lookup
                    const int32_t cb3d4 = (int32_t)rd32(0x1cb3d4u); // ld state
                    const int32_t cb3dc = (int32_t)rd32(0x1cb3dcu); // ld sub-state
                    const int32_t cb2ec = (int32_t)rd32(0x1cb2ecu); // loop ctr
                    const uint32_t ca7c8 = rd32(0x1ca7c8u); // PSYQ VBlank counter
                    const uint32_t ca7ec = rd32(0x1ca7ecu); // VBlank callback fn ptr
                    // Track callback pointer changes — log on every transition.
                    static uint32_t prev_vblank_cb = 0;
                    if (ca7ec != prev_vblank_cb && prev_vblank_cb != 0)
                    {
                        emu::logf(emu::LogLevel::warn, "VBLANK_CB_CHG",
                            "vblank=%u  0x%08X -> 0x%08X  vsync_ctr=%u",
                            vblank_total_count_, prev_vblank_cb, ca7ec, ca7c8);
                    }
                    prev_vblank_cb = ca7ec;
                    const uint32_t ca680 = rd32(0x1ca680u); // DAT_801ca680: CD status (0=pending,2=ready,5=err)
                    const uint32_t ca818 = rd32(0x1ca818u); // DAT_801ca818: game IRQ mask (bit2=CDROM,bit0=VBlank)
                    const uint32_t ca7f4 = rd32(0x1ca7f4u); // DAT_801ca7f4: CDROM IRQ callback fn ptr
                    // DAT_801d6190: FUN_8015d4e0 sets this to 0x801FFF00 before calling (*(code*)0x80140400).
                    // If non-zero → title screen dynamic code has been loaded and is executing.
                    const uint32_t d6190 = rd32(0x1d6190u);
                    // Is CPU currently in the dynamically-loaded title screen code region (0x80140000-0x80141FFF)?
                    const bool in_dyn = (cpu_pc_ >= 0x80140000u && cpu_pc_ <= 0x80141FFFu);
                    // exe_remain: bytes left to load for current EXE chunk (0x1CB334)
                    const uint32_t exe_remain = rd32(0x1cb334u);
                    // exe_dst: target RAM address for current EXE write (0x1CB33C)
                    const uint32_t exe_dst = rd32(0x1cb33cu);
                    emu::logf(emu::LogLevel::debug, "STAGE67ST",
                        "vblank=%u pc=0x%08X sound=%d ldst=%d ldsub=%d loopctr=%d vsync_ctr=%u vblank_cb=0x%08X"
                        " cd_stat=%u game_imask=0x%04X cd_cb=0x%08X hw_istat=0x%04X hw_imask=0x%04X"
                        " d6190=0x%08X dyn=%d exe_remain=%u exe_dst=0x%08X",
                        vblank_total_count_, cpu_pc_,
                        b72cc, cb3d4, cb3dc, cb2ec, ca7c8, ca7ec,
                        ca680, ca818, ca7f4, i_stat_, i_mask_,
                        d6190, (int)in_dyn, exe_remain, exe_dst);
                }
            }

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

                // Re-arm every 200 stuck VBlanks so we get periodic CPU PC snapshots
                if (vblank_stuck_logged_ && (vblank_stuck_count_ % 200) == 0)
                    vblank_stuck_logged_ = 0;

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

                    // Log comprehensive state dump (warn level so it appears in normal runs)
                    emu::logf(emu::LogLevel::warn, "BUS", "===== VSYNC STUCK DETECTED =====");
                    emu::logf(emu::LogLevel::debug, "BUS", "VBlank #%u: stuck for %u VBlanks (no primitives)",
                        vblank_total_count_, vblank_stuck_count_);
                    emu::logf(emu::LogLevel::warn, "BUS", "Last real frame: VBlank #%u", vblank_last_frame_);
                    emu::logf(emu::LogLevel::warn, "BUS", "I_STAT=0x%04X I_MASK=0x%04X pending=0x%04X",
                        (unsigned)i_stat_, (unsigned)i_mask_, (unsigned)(i_stat_ & i_mask_));
                    emu::logf(emu::LogLevel::warn, "BUS", "CPU PC=0x%08X", cpu_pc_);
                    emu::logf(emu::LogLevel::warn, "BUS", "Event table ptr=0x%08X", evt_ptr);
                    emu::logf(emu::LogLevel::warn, "BUS", "SysEnqIntRP chains: [0]=0x%08X [1]=0x%08X [2]=0x%08X [3]=0x%08X",
                        chain_ptrs[0], chain_ptrs[1], chain_ptrs[2], chain_ptrs[3]);
                    emu::logf(emu::LogLevel::warn, "BUS", "PCB=0x%08X TCB=0x%08X", pcb_ptr, tcb_ptr);
                    emu::logf(emu::LogLevel::warn, "BUS", "Last EXC: EPC=0x%08X Cause=0x%08X (ExCode=%u)",
                        cpu_epc_, cpu_cause_, (cpu_cause_ >> 2) & 0x1Fu);

                    // Walk each interrupt chain to find bad handler pointers.
                    // HE struct: { HE* next; int(*func)(); long type; } (12 bytes)
                    auto rd32 = [&](uint32_t vaddr) -> uint32_t {
                        const uint32_t phys = vaddr & 0x1FFFFFFFu;
                        if (phys + 4u > ram_size_) return 0xDEADBEEFu;
                        return (uint32_t)ram_[phys]           |
                               ((uint32_t)ram_[phys + 1] << 8)  |
                               ((uint32_t)ram_[phys + 2] << 16) |
                               ((uint32_t)ram_[phys + 3] << 24);
                    };
                    for (int ci = 0; ci < 4; ++ci)
                    {
                        uint32_t head = chain_ptrs[ci];
                        if (head == 0) continue;
                        emu::logf(emu::LogLevel::warn, "BUS", "Chain[%d] entries (head=0x%08X):", ci, head);
                        for (int ei = 0; ei < 8 && head != 0; ++ei)
                        {
                            const uint32_t phys = head & 0x1FFFFFFFu;
                            if (phys + 12u > ram_size_)
                            {
                                emu::logf(emu::LogLevel::warn, "BUS", "  [%d] @0x%08X OUT_OF_RANGE", ei, head);
                                break;
                            }
                            const uint32_t next = rd32(head);
                            const uint32_t func = rd32(head + 4u);
                            const uint32_t type = rd32(head + 8u);
                            emu::logf(emu::LogLevel::warn, "BUS", "  [%d] @0x%08X next=0x%08X func=0x%08X type=0x%08X",
                                ei, head, next, func, type);
                            head = next;
                        }
                    }

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
                            // Also dump raw words at +0,+4,+8,+C for field layout verification
                            const uint32_t raw0 = (uint32_t)ram_[eoff+0] | ((uint32_t)ram_[eoff+1]<<8) | ((uint32_t)ram_[eoff+2]<<16) | ((uint32_t)ram_[eoff+3]<<24);
                            const uint32_t raw4 = (uint32_t)ram_[eoff+4] | ((uint32_t)ram_[eoff+5]<<8) | ((uint32_t)ram_[eoff+6]<<16) | ((uint32_t)ram_[eoff+7]<<24);
                            const uint32_t raw8 = (uint32_t)ram_[eoff+8] | ((uint32_t)ram_[eoff+9]<<8) | ((uint32_t)ram_[eoff+10]<<16) | ((uint32_t)ram_[eoff+11]<<24);
                            const uint32_t rawC = (uint32_t)ram_[eoff+12] | ((uint32_t)ram_[eoff+13]<<8) | ((uint32_t)ram_[eoff+14]<<16) | ((uint32_t)ram_[eoff+15]<<24);
                            emu::logf(emu::LogLevel::warn, "BUS", "  Event[%u]@0x%05X: cls=0x%08X spec=0x%04X status=0x%04X (%s) raw[+0..+C]=%08X %08X %08X %08X",
                                i, eoff, cls, spec, status, st_str, raw0, raw4, raw8, rawC);

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

            // NOTE: The "rescue" workaround that force-set BUSY events to READY after
            // 50 stuck VBlanks was REMOVED. It caused Tekken (Europe) to panic because
            // Tekken legitimately has 50+ VBlanks with no GPU output during CD loading,
            // causing the rescue to fire and mark all CDROM events READY (including error
            // events spec=0x80, 0x8000), which triggered BIOS ISO reader panic at 0xBFC07EE4.
            // Root cause: "no GPU output == stuck" is wrong during loading screens.

            // Workaround: some BIOS ROMs (e.g. SCPH-7502) never write I_MASK
            // to non-zero — all ROM write sites store r0. The kernel's
            // SysEnqIntRP dispatch chains ARE populated (VBlank at [0],
            // CDROM at [2]), so the exception handler works correctly once
            // IRQs are enabled. After 40 VBlanks (well past BIOS init),
            // we force I_MASK to enable the standard IRQ sources.
            //
            // Also handles the case where BIOS Exec() sets I_MASK=0x000C
            // (CDROM+DMA) during game loading but leaves VBlank (bit 0) disabled.
            // Without this, the second-phase game code (SCES_000.05) stalls in
            // WaitVSync because VBlank IRQ never fires to advance the ACFC counter.
            // We check !(i_mask_ & 0x01) so the counter also accumulates when
            // I_MASK is non-zero but VBlank is missing, and OR in 0x0075 to
            // preserve any bits the BIOS already set.
            if (!(i_mask_ & 0x01))
            {
                ++vblank_no_mask_count_;
                if (vblank_no_mask_count_ >= 40)
                {
                    // OR in standard IRQ sources so we don't clobber bits BIOS set
                    // (e.g. DMA bit 3 that was already present in 0x000C).
                    i_mask_ |= 0x0075; // VBlank(0) | CDROM(2) | TMR0(4) | TMR1(5) | TMR2(6)
                    emu::logf(emu::LogLevel::info, "BUS", "Auto-enable I_MASK=0x%04X after %u VBlanks (VBlank bit was missing)", (unsigned)i_mask_, (unsigned)vblank_no_mask_count_);
                }
            }
            else
            {
                vblank_no_mask_count_ = 0;
            }
#endif // R3000_NO_DIAG — end of VBlank diagnostics
    }

    // Tick CDROM
    if (cdrom_)
    {
        cdrom_->tick(cycles);

        // DMA3 DREQ gating: if DMA3 was deferred (FIFO was empty at trigger time),
        // check if the FIFO is now populated (INT1 fired and try_fill_data_fifo ran).
        // If so, execute the deferred transfer now.
        if (dma3_pending_ && !cdrom_->is_fifo_empty())
        {
            dma3_pending_ = 0;
            emu::logf(emu::LogLevel::warn, "BUS",
                "DMA3 deferred exec: madr=0x%08X bcr=0x%08X (FIFO now ready)",
                dma_[3].madr, dma_[3].bcr);
            exec_dma3_transfer();
        }

        // CDROM IRQ edge detection is now done in check_cdrom_irq_edge() which
        // is called after every CDROM register access. This ensures I_STAT bit 2
        // is set before the game can poll the CDROM for the IRQ type.
        // We still do a check here as a fallback for async IRQs (reads, etc.).
        check_cdrom_irq_edge();

        // Soul Reaver debug: dump game flags when CD advances past LBA 1022
        {
            static int sr_trace = 0;
            // Trace game PC during stall (after loading completes at ~14s)
            static uint64_t pc_trace_last = 0;
            if (vblank_total_count_ > 700 && vblank_total_count_ < 705
                && cpu_pc_ >= 0x80010000u && cpu_pc_ < 0x80100000u
                && vblank_total_count_ != pc_trace_last)
            {
                pc_trace_last = vblank_total_count_;
                const auto& t2 = timers_[2];
                emu::logf(emu::LogLevel::warn, "SR_PC",
                    "VBL=%u pc=0x%08X T2: cnt=%u mode=0x%04X target=%u enabled=%d extclk=%d",
                    vblank_total_count_, cpu_pc_,
                    t2.count, t2.mode, t2.target, (int)t2.counting_enabled, (int)t2.use_external_clock);
            }
            // Dump game VSync counter once after loading
            {
                static int vsync_trace = 0;
                if (vsync_trace < 3 && vblank_total_count_ > 700 && vblank_total_count_ < 710
                    && vblank_total_count_ != (uint32_t)vsync_trace)
                {
                    vsync_trace = vblank_total_count_;
                    const uint32_t game_vsync = *(uint32_t*)(ram_ + (0x800cda48u & (ram_size_ - 1)));
                    emu::logf(emu::LogLevel::warn, "SR_VSYNC",
                        "VBL=%u game_vsync=%u i_stat=0x%04X i_mask=0x%04X",
                        vblank_total_count_, game_vsync, i_stat_, i_mask_);
                }
            }
            if (sr_trace < 5 && cdrom_->read_lba_debug() >= 1023 && cdrom_->read_lba_debug() <= 1030)
            {
                ++sr_trace;
                const uint8_t cd_sync = *(uint8_t*)(ram_ + (0x800cd5bcu & (ram_size_ - 1)));
                const uint8_t cd_ready = *(uint8_t*)(ram_ + (0x800cd5bdu & (ram_size_ - 1)));
                // IRQ handler table: CDROM handler at index 2 (offset 8)
                const uint32_t cdrom_handler = *(uint32_t*)(ram_ + (0x800cb78Cu & (ram_size_ - 1)));
                // Game's own IRQ mask shadow
                const uint32_t irq_mask_game = *(uint32_t*)(ram_ + (0x800cb7b0u & (ram_size_ - 1)));
                emu::logf(emu::LogLevel::warn, "SR_DEBUG",
                    "LBA=%u sync=%u irq=0x%02X i_stat=0x%04X i_mask=0x%04X pc=0x%08X",
                    cdrom_->read_lba_debug(), cd_sync,
                    cdrom_->irq_flags_debug(),
                    i_stat_, i_mask_, cpu_pc_);
            }
        }
    }

    // If VBlank IRQ is not enabled in I_MASK, the game can stall indefinitely in
    // WaitVSync. This covers both the "BIOS never writes I_MASK" case (stays 0)
    // and the "BIOS Exec() set I_MASK=0x000C for loading but left VBlank off" case.
    // Force-enable a minimal mask after some time has passed, even if VBlank is not
    // reached due to host throttling (e.g. UE5 time budget).
    if (!(i_mask_ & 0x01))
    {
        if (no_mask_cycles_ < 0xFFFFFFFFu - cycles)
            no_mask_cycles_ += cycles;
        else
            no_mask_cycles_ = 0xFFFFFFFFu;

        if (no_mask_cycles_ >= kForceMaskAfterCycles)
        {
            i_mask_ |= 0x0075; // VBlank(0) | CDROM(2) | TMR0(4) | TMR1(5) | TMR2(6)
            emu::logf(emu::LogLevel::info, "BUS", "Auto-enable I_MASK=0x%04X after cycles=%u VBlanks=%u (VBlank bit was missing)",
                (unsigned)i_mask_, (unsigned)no_mask_cycles_, (unsigned)vblank_no_mask_count_);
        }
    }
    else
    {
        no_mask_cycles_ = 0;
    }
}

} // namespace r3000
