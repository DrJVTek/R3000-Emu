#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <thread>

#include "../log/filelog.h"
#include "../log/logger.h"
#include "../audio/audio_ring_buffer.h"
#include "../audio/xa_decoder.h"

namespace audio { class Spu; }

namespace cdrom
{

// CD-ROM device (PS1).
//
// Objectif: implémenter la sémantique MMIO réelle du contrôleur CDROM (no$psx / PSX-SPX)
// suffisante pour permettre au BIOS d'initialiser le périphérique et lancer une lecture.
//
// NOTE: pas de "stubs" qui bypassent le flow BIOS; la logique doit rester alignée
// avec la spec (reg banking via Index, IRQ flags/enable, Data/Response FIFOs, Request bits).
class Cdrom
{
  public:
    enum class TimingMode : uint8_t
    {
        // Preferred long-term target: closest to real hardware behavior.
        // This uses realistic spin-up/seek delays and is the mode to validate
        // against reference emulators/hardware.
        Realistic = 0,

        // Compatibility fallback: preserves the historical "10x fast" seek/spin-up
        // behavior that currently lets some BIOS/game flows progress in this core.
        // This is NOT the target model for accuracy; it only exists so we can keep
        // booting/debugging while the event/timing model is being corrected.
        CompatibilityFast = 1,
    };

    enum SecondaryStatusBits : uint8_t
    {
        STAT_ERROR = 1u << 0,
        STAT_MOTOR_ON = 1u << 1,
        STAT_SEEK_ERROR = 1u << 2,
        STAT_ID_ERROR = 1u << 3,
        STAT_SHELL_OPEN = 1u << 4,
        STAT_READING = 1u << 5,
        STAT_SEEKING = 1u << 6,
        STAT_PLAYING_CDDA = 1u << 7,
    };

    explicit Cdrom(rlog::Logger* logger = nullptr);
    ~Cdrom();
    void set_timing_mode(TimingMode mode) { timing_mode_ = mode; }
    TimingMode timing_mode() const { return timing_mode_; }

    // Logs dédiés (optionnels).
    // - cd_only: logs CDROM uniquement
    // - combined: logs "IO" (CD + GPU + system)
    void set_log_sinks(const flog::Sink& cd_only, const flog::Sink& combined, const flog::Clock& clock);

    // Charge une image disque.
    //
    // Formats supportés:
    // - .iso : 2048 bytes/secteur (Mode1 user data)
    // - .bin/.img : 2352 bytes/secteur RAW (Mode1/Mode2/XA)
    // - .cue : BIN/CUE multi-tracks (parse minimal: FILE/TRACK/INDEX, audio ignoré côté "data")
    //
    // Limitations actuelles:
    // - parsing CUE minimal (pas de pregaps complexes / pas d’audio/subchannel)
    bool insert_disc(const char* path, char* err, size_t err_cap);
    // Insert an in-memory virtual disc built from a PS-EXE file.
    // The BIOS can boot it as if it were a real disc (SYSTEM.CNF → BOOT.EXE).
    // Region defaults to SCEE (EU). Call before init_from_image()/BIOS boot.
    bool insert_virtual_exe_disc(const char* exe_path, char* err, size_t err_cap);
    void eject_disc();

    // MMIO: adresses absolues (0x1F801800..803)
    uint8_t mmio_read8(uint32_t addr);
    void mmio_write8(uint32_t addr, uint8_t v);

    // Niveau IRQ CDROM (utilisé par le bus pour latch IRQ2 dans I_STAT sur front montant).
    int irq_line() const;
    bool is_reading_active() const { return reading_active_ != 0; }
    bool is_streaming_mode() const { return streaming_mode_ != 0; }
    void try_redeliver_sector();
    uint8_t irq_flags_raw() const { return irq_flags_; }
    uint8_t irq_enable_raw() const { return irq_enable_; }
    uint8_t debug_index_raw() const { return index_; }
    uint8_t debug_status_reg() const { return status_reg(); }
    uint32_t debug_read_lba() const { return read_lba_; }
    uint32_t debug_data_lba() const { return data_lba_; }
    uint8_t debug_last_cmd() const { return last_cmd_; }
    void clear_irq_flags() { irq_flags_ = 0; }
    void debug_log_bus_irq_latched(uint32_t i_stat, uint32_t i_mask);
    void debug_log_dma3_start(uint32_t madr, uint32_t bcr, uint32_t words);
    void debug_log_dma3_end(uint32_t madr, uint32_t words, int blocked);

    // Tick (called from bus). Handles async IRQ delivery (INT5, INT1 for reads).
    void tick(uint32_t cycles);

    // Advance clock only — no IRQ delivery. Used by DMA handlers to give the
    // CD time without cascading INT1 deliveries that advance read_lba_ too fast.
    void tick_clock_only(uint32_t cycles) { now_cycles_ += cycles; }

    // Audio output for SPU: get next stereo sample pair from CDDA/XA playback.
    // Returns true if audio is available, false if FIFO is empty.
    bool get_audio_frame(int16_t* left, int16_t* right);

    // Check if CDDA is currently playing
    bool is_playing_cdda() const { return playing_cdda_ != 0; }

    // Debug callback: called when garbage SetLoc is detected.
    // Signature: void(uint32_t lba, uint32_t disc_end, void* user)
    using GarbageSetLocCallback = void(*)(uint32_t lba, uint32_t disc_end, void* user);
    void set_garbage_setloc_callback(GarbageSetLocCallback cb, void* user)
    {
        garbage_setloc_cb_ = cb;
        garbage_setloc_user_ = user;
    }

    // IRQ callback: called when CDROM IRQ state changes (like DuckStation's push model).
    // The callback receives the new irq_line() state (0 or 1).
    // This allows immediate notification to the bus for edge detection.
    using IrqCallback = void(*)(int irq_state, void* user);
    void set_irq_callback(IrqCallback cb, void* user)
    {
        irq_callback_ = cb;
        irq_callback_user_ = user;
    }

    // Set SPU for XA-ADPCM audio output
    void set_spu(audio::Spu* spu) { spu_ = spu; }

    // XA audio ring buffer: decoded XA-ADPCM goes here for UE5 consumption.
    // The CDROM decodes XA sectors and pushes directly — no SPU involvement.
    audio::AudioRingBuffer& xa_output_ring() { return xa_output_ring_; }

    // Returns true if the data FIFO is empty (DMA3 DREQ not yet asserted).
    // Used by Bus to defer DMA3 until the sector is ready.
    bool is_fifo_empty() const { return sb_[sb_r_].pos >= sb_[sb_r_].sz; }

    // Sector delivery thread: fires at real-time intervals matching disc speed.
    // The thread only signals "sector ready" via atomic flag. The CPU thread
    // (in tick()) does the actual sector processing (read disc, fill FIFO, IRQ).
    void start_sector_thread();
    void stop_sector_thread();
    bool has_pending_sector_signal() const { return sector_thread_signal_.load(std::memory_order_acquire) != 0; }
    void consume_sector_signal() { sector_thread_signal_.store(0, std::memory_order_release); }
    uint32_t read_lba_debug() const { return read_lba_; }
    uint8_t irq_flags_debug() const { return irq_flags_; }
    uint8_t pending_irq_type_debug() const { return pending_irq_type_; }
    uint64_t pending_irq_due_debug() const { return pending_irq_due_cycle_; }
    uint64_t next_irq_ready_debug() const { return next_irq_ready_cycle_; }
    uint64_t next_read_due_debug() const { return next_read_due_cycle_; }
    uint8_t want_data_debug() const { return want_data_; }
    uint8_t data_ready_pending_debug() const { return data_ready_pending_; }
    uint32_t int1_deliver_count_debug() const { return int1_deliver_count_; }
    uint32_t sector_fire_count_debug() const { return sector_thread_fire_count_.load(std::memory_order_relaxed); }
    uint32_t sector_consumed_count_debug() const { return sector_signal_consumed_count_.load(std::memory_order_relaxed); }
    uint32_t resp_count_debug() const { return (resp_w_ >= resp_r_) ? (resp_w_ - resp_r_) : (32 - resp_r_ + resp_w_); }
    void log_external(const char* fmt, ...);  // Log via cdrom file logger (for UE5)
    uint64_t now_cycles_debug() const { return now_cycles_; }
    void check_sector_read_complete();

    // Lecture d'un secteur "user data" 2048 bytes (ISO9660).
    // Retourne false si pas de disque ou secteur illisible.
    bool read_sector_2048(uint32_t lba, uint8_t out[2048]);

    // Recherche un fichier ISO9660 (device "cdrom:") et renvoie (LBA, size bytes).
    // Le path accepte par ex:
    // - "cdrom:\\SYSTEM.CNF;1"
    // - "cdrom:\\PSX.EXE"
    // - "\\SYSTEM.CNF"
    // Comparaison case-insensitive, ignore ";1".
    bool iso9660_find_file(const char* path, uint32_t* out_lba, uint32_t* out_size);

    // ISO9660 file map entry — public so static helpers in cdrom.cpp can use it.
    struct FileMapEntry
    {
        uint32_t lba_start;
        uint32_t lba_end;   // exclusive: lba_start + ceil(size/2048)
        char path[128];     // e.g. "/TEKKEN.EXE"
    };

  private:
    struct Disc;
    struct DiscRegion
    {
        // PS1 region letters: 'I'=Japan, 'A'=America, 'E'=Europe. 0 = unknown.
        char letter{0};
        char scex[4]{0, 0, 0, 0}; // "SCEI"/"SCEA"/"SCEE" (no NUL).
    };

    // ISO9660 file map: built once at insert_disc() for LBA→filename annotation.
    static constexpr int kMaxFileMapEntries = 512;
    FileMapEntry file_map_[kMaxFileMapEntries]{};
    int file_map_count_{0};

    // Build file_map_ by recursively scanning the ISO9660 directory tree.
    void build_file_map();
    // Returns a static description string "NAME+0xOFFSET" for a given LBA, or nullptr if not mapped.
    const char* lba_to_filename(uint32_t lba) const;

    // Helpers BCD (le BIOS parle en BCD).
    static uint8_t bcd_to_u8(uint8_t bcd);
    static uint8_t u8_to_bcd(uint8_t v);

    void push_resp(uint8_t v);
    uint8_t pop_resp();
    void clear_resp();

    void push_data(const uint8_t* p, size_t n);
    uint8_t pop_data();
    void clear_data();

    void clear_params();

    void exec_command(uint8_t cmd);
    void schedule_command_execution(uint8_t cmd, const uint8_t* params, uint8_t param_count);
    const char* cmd_name(uint8_t cmd) const;
    uint8_t cmd_expected_params(uint8_t cmd) const;

    void set_irq(uint8_t flags);
    void queue_cmd_irq(uint8_t flags);
    void stop_reading_with_error(uint8_t reason);
    uint8_t status_reg() const;
    void try_fill_data_fifo();
    void clear_secondary_active_bits();
    void set_secondary_idle(bool motor_on);
    void set_secondary_seeking();
    void set_secondary_reading();
    void set_secondary_playing();
    void cancel_pending_read_advance();
    void arm_pending_irq_after(uint32_t delay_cycles);
    void arm_command_execution_after(uint32_t delay_cycles);
    void arm_motor_idle_after(uint32_t delay_cycles);

    // CDDA audio processing
    void start_cdda_playback();
    void stop_cdda_playback();
    void process_cdda_sector();
    void add_cdda_frame(int16_t left, int16_t right);
    void tick_cdda(uint32_t cycles);
    bool read_raw_sector(uint32_t lba, uint8_t out[2352]);

    uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) const;
    bool read_user_data_2048(uint32_t lba, uint8_t out[2048]);
    DiscRegion infer_disc_region();

    // Seek timing: calculate delay in CPU cycles based on LBA distance.
    // Uses logarithmic model like DuckStation for realistic timing.
    //
    // IMPORTANT:
    // - `TimingMode::Realistic` is the fidelity target.
    // - `TimingMode::CompatibilityFast` keeps the old 10x-fast seek/spin-up path
    //   as an explicit compatibility mode.
    // - The user preference is to move toward real timing, not to normalize on
    //   the 10x-fast behavior forever.
    uint32_t calc_seek_time(uint32_t from_lba, uint32_t to_lba, bool include_spinup) const;
    uint32_t read_sector_ticks() const;

    rlog::Logger* logger_{nullptr};

    flog::Sink log_cd_{};
    flog::Sink log_io_{};
    flog::Clock clock_{};
    int has_clock_{0};

    Disc* disc_{nullptr};
    DiscRegion disc_region_{};

    // Debug callback for garbage SetLoc
    GarbageSetLocCallback garbage_setloc_cb_{nullptr};
    void* garbage_setloc_user_{nullptr};

    // IRQ callback for push-model notification
    IrqCallback irq_callback_{nullptr};
    void* irq_callback_user_{nullptr};

    // Sector delivery thread
    std::thread sector_thread_;
    std::atomic<bool> sector_thread_running_{false};
    std::atomic<uint8_t> sector_thread_signal_{0}; // 1 = sector timer fired, CPU should deliver
    std::atomic<uint32_t> sector_thread_fire_count_{0}; // debug: signals sent by thread
    std::atomic<uint32_t> sector_signal_consumed_count_{0}; // debug: signals consumed by tick

    // XA-ADPCM decoder + SPU output
    audio::Spu* spu_{nullptr};
    audio::AudioRingBuffer xa_output_ring_;
    audio::XaDecoder xa_decoder_;

    TimingMode timing_mode_{TimingMode::CompatibilityFast};

    // Registres CDROM (modèle minimal, mais avec sémantique réelle).
    uint8_t index_{0};   // écrit via 0x1F801800
    uint8_t status_{0};  // CDROM secondary status returned by GetStat/command responses
    uint8_t irq_enable_{0x1Fu}; // PSX-SPX: defaults to 1Fh (all INT1-INT5 enabled)
    uint8_t irq_flags_{0};
    uint8_t request_{0}; // 1F801803.Index0 (SMEN/BFRD)
    uint8_t busy_{0};

    // FIFOs
    uint8_t param_fifo_[16]{};
    uint8_t param_count_{0};

    uint8_t resp_fifo_[32]{};
    uint8_t resp_r_{0};
    uint8_t resp_w_{0};

    // 8 sector buffers (like DuckStation). write_idx advances on each
    // new sector. read_idx advances only on INT1 delivery.
    static constexpr int kNumSB = 8;
    struct SB { uint8_t data[2352]{}; uint16_t pos{0}; uint16_t sz{0}; };
    SB sb_[kNumSB]{};
    uint8_t sb_w_{0}; // next buffer to write
    uint8_t sb_r_{0}; // buffer the game reads from

    // Last sector header + subheader (captured on each sector read, used by GetLocL)
    // header: mm, ss, ff, mode (4 bytes from raw sector offset 12)
    // subheader: file, channel, submode, coding (4 bytes from raw sector offset 16)
    uint8_t last_sector_header_[4]{};
    uint8_t last_sector_subheader_[4]{};
    uint8_t last_sector_header_valid_{0};

    // Etat lecture
    uint8_t loc_msf_[3]{};
    uint32_t loc_lba_{0};            // SetLoc target LBA requested by software
    uint32_t read_lba_{0};           // Sector currently exposed/read by ReadN/ReadS
    uint32_t data_lba_{0};           // Sector latched by the last delivered INT1/DataReady
    uint8_t want_data_{0};
    uint8_t read_pending_irq1_{0};   // second response INT1 pending (ReadN/ReadS)
    uint8_t data_ready_pending_{0};  // data can be loaded when want_data=1
    uint8_t async_stat_pending_{0};  // async status INT1 pending after certain commands

    // Dual-channel IRQ (like real PS1 hardware / DuckStation):
    // Sync channel: command responses (INT3/INT5) via set_irq() → irq_flags_ directly
    // Async channel: sector data ready (INT1) via this pending + separate resp
    // The async fires AFTER the sync is acked, with minimum delay.
    uint8_t async_irq_type_{0};        // 0=none, 0x01=DataReady
    uint8_t async_resp_{0};            // status byte for async delivery
    uint8_t async_resp_valid_{0};      // 1 if async has response to deliver
    void set_async_irq(uint8_t type, uint8_t resp);
    void deliver_async_irq();

    // Deferred command INT3: set when a command INT3 would overwrite an
    // unACK'd sector INT1 in irq_flags. Delivered after the INT1 is ACK'd.
    uint8_t deferred_cmd_irq_{0};       // 0=none, 0x03=INT3
    uint8_t deferred_cmd_resp_valid_{0}; // 1 if deferred INT3 waiting
    uint8_t reading_active_{0};      // ReadN/ReadS continuous reading in progress
    uint32_t int1_deliver_count_{0}; // debug: total INT1 deliveries
    uint8_t streaming_mode_{0};      // 1 if ReadS (streaming), 0 if ReadN (normal)

    // Command queue (quand IRQ flags non ack ou Busy=1).
    uint8_t queued_cmd_{0};
    uint8_t queued_cmd_valid_{0};
    uint8_t queued_params_[16]{};
    uint8_t queued_param_count_{0};

    // Etat CDROM simplifié
    uint8_t mode_{0};
    uint8_t filter_file_{0};
    uint8_t filter_chan_{0};
    uint8_t seek_pending_{0};          // SetLoc target differs from committed head position
    uint8_t seek_in_progress_{0};      // explicit SeekL/SeekP mechanical motion in flight
    uint8_t pending_read_seek_commit_{0}; // first INT1 completes an implicit seek
    uint32_t seek_target_lba_{0};      // target that will be committed on seek completion

    // Motor and head position tracking for realistic seek/spin-up timing.
    // Real PS1: motor spins down after Stop/Pause, spin-up takes ~600ms.
    // Seek time depends on LBA distance (logarithmic model like DuckStation).
    uint8_t motor_spinning_{0};      // 0=idle (needs spin-up), 1=spinning
    uint32_t head_lba_{0};           // Physical head position (for seek distance calc)
    uint64_t motor_idle_deadline_{0}; // absolute emulated cycle when motor spins down

    // Audio volume registers (pas critique pour boot, mais présents dans l'I/O map).
    uint8_t vol_ll_{0x80}; // L-CD -> L-SPU
    uint8_t vol_lr_{0x00}; // L-CD -> R-SPU
    uint8_t vol_rr_{0x80}; // R-CD -> R-SPU
    uint8_t vol_rl_{0x00}; // R-CD -> L-SPU

    // CDDA playback state
    uint8_t playing_cdda_{0};        // CDDA playback active
    uint32_t cdda_lba_{0};           // Current CDDA sector LBA
    uint32_t cdda_sector_samples_{0}; // Samples remaining in current sector
    uint32_t cdda_cycle_accum_{0};   // Cycle accumulator for CDDA timing

    // CDDA audio FIFO (ring buffer for stereo samples)
    static constexpr int kAudioFifoSize = 4096;  // sample pairs (L+R)
    int16_t audio_fifo_l_[kAudioFifoSize]{};
    int16_t audio_fifo_r_[kAudioFifoSize]{};
    int audio_fifo_read_{0};
    int audio_fifo_write_{0};
    int audio_fifo_count_{0};

    // Current sector buffer for CDDA (2352 bytes raw)
    uint8_t cdda_sector_buf_[2352]{};
    int cdda_sector_pos_{0};  // Current sample position in sector (0..587 for 1x, 0..293 for 2x)

    // Shell close interrupt tracking.
    // INT5 is sent when the BIOS enables it and a disc is present.
    uint8_t shell_close_sent_{0};

    // Async IRQ delivery delays (in CPU cycles).
    // On real hardware, IRQs are delivered asynchronously by the drive.
    // We queue them and deliver after a short delay so the CPU has time
    // to return to its polling loop with interrupts enabled.
    uint64_t pending_irq_due_cycle_{0}; // absolute emulated cycle when pending IRQ may fire
    uint8_t pending_irq_type_{0};    // IRQ type to deliver (1-5), 0=none
    uint8_t pending_irq_resp_{0};    // response byte 0 (stat)
    uint8_t pending_irq_live_status_{0}; // if set, response byte 0 is read from current status_ on delivery
    uint8_t pending_irq_reason_{0};  // response byte 1 (reason code, 0=none)
    uint8_t pending_irq_extra_[16]{};// extra response bytes (for GetID etc.)
    uint8_t pending_irq_extra_len_{0};

    // Command response delay: irq_flags set after this delay elapses.
    // Real hardware schedules command execution/ACK, it does not expose the
    // response bytes immediately on command write.
    uint64_t cmd_exec_due_cycle_{0};   // absolute emulated cycle when command executes
    uint8_t cmd_exec_valid_{0};       // delayed command pending
    uint8_t cmd_exec_cmd_{0};         // command byte to execute
    uint8_t cmd_exec_params_[16]{};   // latched parameter bytes
    uint8_t cmd_exec_param_count_{0}; // number of latched parameters

    uint8_t last_cmd_{0};             // last command executed (for debug)

    // Emulated timebase for CDROM. All asynchronous command/IRQ timing is based
    // on absolute cycle deadlines so behavior stays invariant across host tick
    // granularity (CLI vs UE5 worker thread).
    uint64_t now_cycles_{0};

    // DuckStation-style MINIMUM_INTERRUPT_DELAY.
    // New IRQs cannot be delivered until at least 1000 cycles after ACK.
    static constexpr uint32_t kMinInterruptDelay = 1000;
    uint64_t next_irq_ready_cycle_{0};
    uint64_t next_read_due_cycle_{0}; // DuckStation-style: next sector arrives at this cycle (timer-driven)
    uint8_t prev_sector_partial_{0}; // Previous sector was partially consumed (<50%)

    // Trace counters (per-instance, not static, so they reset between PIE sessions).
    int mmio_rd_trace_{0};
    uint32_t data_read_count_{0};
    int mmio_wr_trace_{0};
};

} // namespace cdrom

