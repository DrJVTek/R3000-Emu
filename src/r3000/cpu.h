#pragma once

#include <cstdint>

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

    StepResult step();

  private:
    // COP0 minimal (suffisant pour exceptions et quelques move).
    // On reste volontairement simple pour une démo: pas de TLB, pas de timing, pas d'irq.
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
