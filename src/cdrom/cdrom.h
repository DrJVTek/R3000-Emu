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
    uint8_t status_reg() const;
    void try_fill_data_fifo();

    uint32_t msf_to_lba(uint8_t m, uint8_t s, uint8_t f) const;
    bool read_user_data_2048(uint32_t lba, uint8_t out[2048]);

    rlog::Logger* logger_{nullptr};

    flog::Sink log_cd_{};
    flog::Sink log_io_{};
    flog::Clock clock_{};
    int has_clock_{0};

    Disc* disc_{nullptr};

    // Registres CDROM (modèle minimal, mais avec sémantique réelle).
    uint8_t index_{0};   // écrit via 0x1F801800
    uint8_t status_{0};  // lu via 0x1F801800
    uint8_t irq_enable_{0};
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

    // Command queue (quand IRQ flags non ack ou Busy=1).
    uint8_t queued_cmd_{0};
    uint8_t queued_cmd_valid_{0};
    uint8_t queued_params_[16]{};
    uint8_t queued_param_count_{0};

    // Etat CDROM simplifié
    uint8_t mode_{0};
    uint8_t filter_file_{0};
    uint8_t filter_chan_{0};

    // Audio volume registers (pas critique pour boot, mais présents dans l'I/O map).
    uint8_t vol_ll_{0x80}; // L-CD -> L-SPU
    uint8_t vol_lr_{0x00}; // L-CD -> R-SPU
    uint8_t vol_rr_{0x80}; // R-CD -> R-SPU
    uint8_t vol_rl_{0x00}; // R-CD -> L-SPU

    // Shell close interrupt tracking.
    // INT5 is sent when the BIOS enables it and a disc is present.
    uint8_t shell_close_sent_{0};
};

} // namespace cdrom

