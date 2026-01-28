#pragma once

#include <cstdint>
#include <cstdio>
#include <array>

#include "../log/filelog.h"
#include "../gte/gte.h"
#include "../log/logger.h"
#include "bus.h"

namespace r3000
{

// CPU R3000 minimal (MIPS I) pour démo éducative.
// Objectif: un coeur interprété clair, avec logs et delay slot.
class Cpu
{
  public:
    struct StepResult
    {
        enum class Kind
        {
            ok,
            halted,        // BREAK/SYSCALL (demo) ou arrêt demandé
            mem_fault,     // accès mémoire invalide / non-aligné
            illegal_instr, // opcode/funct non supporté
        };
        Kind kind{Kind::ok};
        uint32_t pc{0};
        uint32_t instr{0};
        Bus::MemFault mem_fault{};
    };

    Cpu(Bus& bus, rlog::Logger* logger = nullptr);

    void reset(uint32_t reset_pc);

    uint32_t pc() const
    {
        return pc_;
    }
    uint32_t reg(unsigned idx) const
    {
        return gpr_[idx & 31u];
    }

    void set_pretty(int enabled)
    {
        pretty_ = enabled ? 1 : 0;
    }

    void set_stop_on_high_ram(int enabled)
    {
        stop_on_high_ram_ = enabled ? 1 : 0;
    }

    void set_stop_on_bios_to_ram_nop(int enabled)
    {
        stop_on_bios_to_ram_nop_ = enabled ? 1 : 0;
    }

    // Debug: stop dès qu'on ENTRE dans une zone de NOPs en RAM (transition depuis une instr non-NOP).
    // C'est utile quand le BIOS part exécuter du vide vers 0x801FFxxx, mais qu'on veut voir
    // l'instruction qui a provoqué le saut (avant d'avoir 1000 lignes de 0x00000000).
    void set_stop_on_ram_nop(int enabled)
    {
        stop_on_ram_nop_ = enabled ? 1 : 0;
    }

    // Debug: stop when PC reaches an exact value (virtual address).
    void set_stop_on_pc(uint32_t pc, int enabled)
    {
        stop_on_pc_ = enabled ? 1 : 0;
        stop_pc_ = pc;
        stopped_on_pc_ = 0;
    }

    // Debug: log verbeux des accès MMIO (I/O PS1) avec nom de registre si connu.
    void set_trace_io(int enabled)
    {
        trace_io_ = enabled ? 1 : 0;
    }

    // Debug/HLE: active des trampolines sécurisés (vecteurs A0/B0/C0 et exception vector RAM).
    void set_hle_vectors(int enabled);

    // Debug: fichier de sortie texte (BIOS putc / syscalls "write-like" / etc).
    // Objectif: avoir un "console.log" séparé et facile à relire pendant le live.
    void set_text_out(std::FILE* f)
    {
        text_out_ = f;
    }

    // Duplique le texte (BIOS putc / write-like) vers un log combiné (ex: logs/io.log).
    // On bufferise par ligne pour éviter le spam par caractère.
    void set_text_io_sink(const flog::Sink& s, const flog::Clock& c)
    {
        text_io_ = s;
        text_clock_ = c;
        text_has_clock_ = 1;
    }

    // Logs "système" pour événements CPU à fort signal (ex: boucle d'exception).
    // Objectif: pas de trace CPU/GTE complète ici, seulement quelques lignes clés.
    void set_sys_log_sinks(const flog::Sink& sys, const flog::Sink& combined, const flog::Clock& c)
    {
        sys_log_ = sys;
        sys_io_ = combined;
        sys_clock_ = c;
        sys_has_clock_ = 1;
    }

    // Compare-with-DuckStation: write parseable key=0x... blocks at debug-loop PCs.
    void set_compare_file(std::FILE* f)
    {
        compare_file_ = f;
    }

    // Petites APIs "outillage" (loader) pour initialiser un programme chargé depuis un fichier.
    // On reste minimal: pas d'OS/BIOS, donc le loader doit pouvoir positionner PC/GP/SP.
    void set_pc(uint32_t pc)
    {
        pc_ = pc;
    }

    void set_gpr(uint32_t idx, uint32_t v)
    {
        set_reg(idx, v);
    }

    StepResult step();

  private:
    // COP0 minimal (suffisant pour exceptions et quelques move).
    // On reste volontairement simple pour une démo: pas de TLB, pas de timing cycle-accurate.
    // NOTE: on supporte néanmoins un minimum d'IRQ (EXC_INT) pour permettre au BIOS d'avancer.
    enum Cop0Reg : uint32_t
    {
        COP0_INDEX = 0,
        COP0_RANDOM = 1,
        COP0_ENTRYLO0 = 2,
        COP0_ENTRYLO1 = 3,
        COP0_CONTEXT = 4,
        COP0_PAGEMASK = 5,
        COP0_WIRED = 6,
        COP0_BADVADDR = 8,
        COP0_COUNT = 9,
        COP0_ENTRYHI = 10,
        COP0_COMPARE = 11,
        COP0_STATUS = 12,
        COP0_CAUSE = 13,
        COP0_EPC = 14,
        COP0_PRID = 15,
    };

    enum ExceptionCode : uint32_t
    {
        EXC_INT = 0,
        EXC_ADEL = 4, // Address error load/fetch
        EXC_ADES = 5, // Address error store
        EXC_SYS = 8,  // Syscall
        EXC_BP = 9,   // Breakpoint
        EXC_RI = 10,  // Reserved instruction
        EXC_OV = 12,  // Overflow
    };

    // Helpers de décodage (format MIPS)
    static uint32_t op(uint32_t i)
    {
        return (i >> 26) & 0x3Fu;
    }
    static uint32_t rs(uint32_t i)
    {
        return (i >> 21) & 0x1Fu;
    }
    static uint32_t rt(uint32_t i)
    {
        return (i >> 16) & 0x1Fu;
    }
    static uint32_t rd(uint32_t i)
    {
        return (i >> 11) & 0x1Fu;
    }
    static uint32_t shamt(uint32_t i)
    {
        return (i >> 6) & 0x1Fu;
    }
    static uint32_t funct(uint32_t i)
    {
        return i & 0x3Fu;
    }
    static uint16_t imm_u(uint32_t i)
    {
        return static_cast<uint16_t>(i & 0xFFFFu);
    }
    static int16_t imm_s(uint32_t i)
    {
        return static_cast<int16_t>(i & 0xFFFFu);
    }
    static uint32_t jidx(uint32_t i)
    {
        return i & 0x03FF'FFFFu;
    }

    void set_reg(uint32_t idx, uint32_t v);
    void schedule_branch(uint32_t target_after_delay_slot);

    // Traduction virtuelle->physique (simplifiée PS1): KSEG0/KSEG1 = alias sur
    // 0x0000_0000..0x1FFF_FFFF.
    uint32_t virt_to_phys(uint32_t vaddr) const;

    // Exceptions (mode minimal): set COP0(Cause/EPC/BadVAddr) et saute sur vector.
    void raise_exception(uint32_t code, uint32_t badvaddr, uint32_t pc_of_fault);

    // R3000A (MIPS I) n'a pas d'interlocks complets: les loads ont un "load delay slot".
    // Concrètement: une instruction de load met à jour rt *après* l'instruction suivante.
    struct PendingLoad
    {
        int valid;
        uint32_t reg;
        uint32_t value;
    };

    void commit_pending_load();

    Bus& bus_;
    rlog::Logger* logger_{nullptr};

    uint32_t gpr_[32]{};
    uint32_t hi_{0};
    uint32_t lo_{0};
    uint32_t pc_{0};
    uint32_t cop0_[32]{};

    PendingLoad pending_load_{};

    // R3000A I-cache (4KB) - utilisé comme "data store" quand COP0.Status.Isc=1 (cache isolated).
    // Pour le bring-up BIOS, l'essentiel est que les boucles d'init cache n'écrasent pas la RAM.
    std::array<uint8_t, 4u * 1024u> icache_data_{};

    // Trace ring-buffer (dernieres instructions fetchées) pour debug BIOS.
    // Utile quand on veut comprendre un IFETCH fault sans activer --pretty.
    uint32_t recent_pc_[256]{};
    uint32_t recent_instr_[256]{};
    uint32_t recent_pos_{0};
    int stop_on_high_ram_{0};
    int stopped_on_high_ram_{0};
    int stop_on_bios_to_ram_nop_{0};
    int stop_on_ram_nop_{0};
    int stop_on_pc_{0};
    int stopped_on_pc_{0};
    uint32_t stop_pc_{0};
    int trace_io_{0};
    int hle_vectors_{0};
    std::FILE* text_out_{nullptr};
    flog::Sink text_io_{};
    flog::Clock text_clock_{};
    int text_has_clock_{0};
    char text_line_[512]{};
    uint32_t text_pos_{0};

    flog::Sink sys_log_{};
    flog::Sink sys_io_{};
    flog::Clock sys_clock_{};
    int sys_has_clock_{0};

    std::FILE* compare_file_{nullptr};

    uint64_t exc_vec_hits_{0};

    // HLE BIOS vectors (bring-up): petit état pour quelques services kernel.
    // NOTE: ce n'est pas "PS1-accurate"; objectif = permettre au BIOS d'avancer,
    // tout en gardant le code lisible pour le live.
    uint32_t kalloc_ptr_{0};
    uint32_t kalloc_end_{0};
    uint32_t entryint_struct_addr_{0};
    uint32_t entryint_hook_addr_{0};

    struct HleEvent
    {
        uint32_t cls;
        uint32_t spec;
        uint32_t mode;
        uint32_t func;
        uint32_t status;
    };

    HleEvent hle_events_[32]{};
    uint32_t hle_vblank_div_{0};
    int hle_pseudo_vblank_{0};

    // HLE BIOS File I/O (cdrom:) - minimal pour permettre le boot CD.
    struct HleFile
    {
        int used;
        uint32_t lba;   // LBA ISO9660 (secteurs 2048)
        uint32_t size;  // taille fichier en bytes
        uint32_t pos;   // position courante en bytes
    };

    HleFile hle_files_[16]{};
    uint32_t hle_last_error_{0};
    uint64_t hle_wait_event_calls_{0};
    uint64_t hle_mark_ready_calls_{0};
    uint32_t dbg_loop_dumped_{0};
    uint32_t dbg_loop_patched_{0};
    uint32_t dbg_ef30_dumped_{0};
    uint32_t dbg_de24_dumped_{0};
    uint32_t dbg_e520_dumped_{0};
    uint32_t dbg_6797c_dumped_{0};
    uint32_t dbg_67938_dumped_{0};
    uint32_t spin_pc_{0};
    uint32_t spin_count_{0};
    // NOTE: pas de "skip loop" ici: on préfère corriger l'émulation plutôt que patcher le flow du BIOS.

    // COP2 = GTE (PS1). Séparé du CPU pour garder le code propre.
    gte::Gte gte_;

    // MIPS: branchements/jumps ont un delay slot.
    // On stocke le PC à appliquer *après* l'instruction suivante.
    bool branch_pending_{false};
    uint32_t branch_target_{0};
    uint32_t branch_delay_slots_{0}; // 1 = un delay slot restant
    bool branch_just_scheduled_{false};

    int pretty_{0};
};

} // namespace r3000
