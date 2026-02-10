#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "../log/filelog.h"
#include "../log/logger.h"

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
    explicit Cdrom(rlog::Logger* logger = nullptr);

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
    void eject_disc();

    // MMIO: adresses absolues (0x1F801800..803)
    uint8_t mmio_read8(uint32_t addr);
    void mmio_write8(uint32_t addr, uint8_t v);

    // Niveau IRQ CDROM (utilisé par le bus pour latch IRQ2 dans I_STAT sur front montant).
    int irq_line() const;
    uint8_t irq_flags_raw() const { return irq_flags_; }
    uint8_t irq_enable_raw() const { return irq_enable_; }
    void clear_irq_flags() { irq_flags_ = 0; }

    // Tick (called from bus). Handles async IRQ delivery (INT5, INT1 for reads).
    void tick(uint32_t cycles);

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

  private:
    struct Disc;
    struct DiscRegion
    {
        // PS1 region letters: 'I'=Japan, 'A'=America, 'E'=Europe. 0 = unknown.
        char letter{0};
        char scex[4]{0, 0, 0, 0}; // "SCEI"/"SCEA"/"SCEE" (no NUL).
    };

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
    const char* cmd_name(uint8_t cmd) const;
    uint8_t cmd_expected_params(uint8_t cmd) const;

    void set_irq(uint8_t flags);
    void queue_cmd_irq(uint8_t flags);
    void stop_reading_with_error(uint8_t reason);
    uint8_t status_reg() const;
    void try_fill_data_fifo();

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
    uint32_t calc_seek_time(uint32_t from_lba, uint32_t to_lba, bool include_spinup) const;

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

    // Registres CDROM (modèle minimal, mais avec sémantique réelle).
    uint8_t index_{0};   // écrit via 0x1F801800
    uint8_t status_{0};  // lu via 0x1F801800
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

    uint8_t data_fifo_[4096]{};
    uint16_t data_r_{0};
    uint16_t data_w_{0};

    // Etat lecture
    uint8_t loc_msf_[3]{};
    uint32_t loc_lba_{0};
    uint8_t want_data_{0};
    uint8_t read_pending_irq1_{0};   // second response INT1 pending (ReadN/ReadS)
    uint8_t data_ready_pending_{0};  // data can be loaded when want_data=1
    uint8_t async_stat_pending_{0};  // async status INT1 pending after certain commands
    uint8_t reading_active_{0};      // ReadN/ReadS continuous reading in progress

    // Command queue (quand IRQ flags non ack ou Busy=1).
    uint8_t queued_cmd_{0};
    uint8_t queued_cmd_valid_{0};
    uint8_t queued_params_[16]{};
    uint8_t queued_param_count_{0};

    // Etat CDROM simplifié
    uint8_t mode_{0};
    uint8_t filter_file_{0};
    uint8_t filter_chan_{0};
    uint8_t seek_pending_{0};  // SetLoc was called, next read needs seek delay

    // Motor and head position tracking for realistic seek/spin-up timing.
    // Real PS1: motor spins down after Stop/Pause, spin-up takes ~600ms.
    // Seek time depends on LBA distance (logarithmic model like DuckStation).
    uint8_t motor_spinning_{0};      // 0=idle (needs spin-up), 1=spinning
    uint32_t head_lba_{0};           // Physical head position (for seek distance calc)
    uint32_t motor_idle_countdown_{0}; // Cycles until motor spins down after Pause

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
    uint32_t pending_irq_delay_{0};  // cycles until pending IRQ fires
    uint8_t pending_irq_type_{0};    // IRQ type to deliver (1-5), 0=none
    uint8_t pending_irq_resp_{0};    // response byte 0 (stat)
    uint8_t pending_irq_reason_{0};  // response byte 1 (reason code, 0=none)
    uint8_t pending_irq_extra_[16]{};// extra response bytes (for GetID etc.)
    uint8_t pending_irq_extra_len_{0};

    // Command response delay: irq_flags set after this delay elapses.
    // Response data is already in the FIFO (BIOS can poll), but the IRQ
    // line isn't raised until the delay expires, preventing VBlank handler
    // from seeing CDROM irq_flags during the probing phase.
    uint32_t cmd_irq_delay_{0};      // cycles until irq_flags are set
    uint8_t cmd_irq_pending_{0};     // IRQ type to set when delay expires

    uint8_t last_cmd_{0};             // last command executed (for debug)

    // DuckStation-style MINIMUM_INTERRUPT_DELAY: cycles since last IRQ ack.
    // New IRQs cannot be delivered until at least 1000 cycles after ack.
    // This prevents rapid-fire IRQ sequences that confuse the BIOS state machine.
    static constexpr uint32_t kMinInterruptDelay = 1000;
    uint32_t cycles_since_irq_ack_{kMinInterruptDelay}; // start ready to deliver

    // Trace counters (per-instance, not static, so they reset between PIE sessions).
    int mmio_rd_trace_{0};
    uint32_t data_read_count_{0};
    int mmio_wr_trace_{0};
};

} // namespace cdrom

