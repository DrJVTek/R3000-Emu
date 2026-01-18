#include "cpu.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

namespace r3000
{

static const char* reg_name(uint32_t idx)
{
    // Noms "ABI" MIPS pour que le trace soit lisible en live.
    // Exemple: t0/t1/t2 = temporaires, a0..a3 = arguments, sp = stack pointer, ra = return address.
    // NOTE: l'ABI est un "convention de nommage". Le CPU, lui, ne connait que 32 registres GPR.
    static const char* k[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return k[idx & 31u];
}

Cpu::Cpu(Bus& bus, rlog::Logger* logger) : bus_(bus), logger_(logger)
{
}

void Cpu::reset(uint32_t reset_pc)
{
    // Reset minimal: on met tout à zéro et on positionne PC sur l'adresse de reset.
    // Dans une vraie PS1, le reset vector et certains registres auraient une valeur spécifique.
    for (int i = 0; i < 32; ++i)
        gpr_[i] = 0;
    hi_ = 0;
    lo_ = 0;
    pc_ = reset_pc;
    branch_pending_ = false;
    branch_target_ = 0;
    branch_delay_slots_ = 0;
    branch_just_scheduled_ = false;

    // COP0: valeurs par défaut minimales.
    for (int i = 0; i < 32; ++i)
    {
        cop0_[i] = 0;
    }
    cop0_[COP0_STATUS] = 0; // IE=0 au début (simplifié)
    cop0_[COP0_CAUSE] = 0;
    cop0_[COP0_EPC] = 0;
    cop0_[COP0_BADVADDR] = 0;

    pending_load_.valid = 0;
    pending_load_.reg = 0;
    pending_load_.value = 0;

    gte_.reset();
}

void Cpu::set_reg(uint32_t idx, uint32_t v)
{
    // Sur MIPS, r0 vaut TOUJOURS 0, on ignore donc toute écriture vers r0.
    // C'est un invariant très pratique en assembleur (NOP, clear register, etc.).
    if ((idx & 31u) == 0u)
        return; // r0 = 0
    gpr_[idx & 31u] = v;
}

void Cpu::schedule_branch(uint32_t target_after_delay_slot)
{
    // Branch/jump en MIPS I ont un "delay slot" :
    // l'instruction SUIVANTE (celle qui est à PC+4) s'exécute toujours,
    // puis seulement après on applique la destination de branchement.
    //
    // Ici, on ne saute pas tout de suite. On "programme" le saut pour après 1 instruction.
    branch_pending_ = true;
    branch_target_ = target_after_delay_slot;
    branch_delay_slots_ = 1;
    branch_just_scheduled_ = true;
}

uint32_t Cpu::virt_to_phys(uint32_t vaddr) const
{
    // PS1/R3000A: pas de TLB utilisé dans la console.
    // KSEG0: 0x8000_0000..0x9FFF_FFFF -> phys = vaddr & 0x1FFF_FFFF
    // KSEG1: 0xA000_0000..0xBFFF_FFFF -> phys = vaddr & 0x1FFF_FFFF
    if ((vaddr & 0xE000'0000u) == 0x8000'0000u || (vaddr & 0xE000'0000u) == 0xA000'0000u)
    {
        return vaddr & 0x1FFF'FFFFu;
    }
    return vaddr;
}

void Cpu::raise_exception(uint32_t code, uint32_t badvaddr, uint32_t pc_of_fault)
{
    // Minimal COP0 exception handling:
    // - EPC = PC de l'instruction fautive (ici: pc_of_fault)
    // - Cause.ExcCode = code (bits 6..2)
    // - BadVAddr = adresse fautive (pour ADEL/ADES principalement)
    // - PC saute sur le vecteur 0x8000_0080 (common exception vector)
    cop0_[COP0_EPC] = pc_of_fault;
    cop0_[COP0_BADVADDR] = badvaddr;

    cop0_[COP0_CAUSE] &= ~((uint32_t)0x1Fu << 2);
    cop0_[COP0_CAUSE] |= ((code & 0x1Fu) << 2);

    // Sur une vraie machine, Status manipule le mode kernel/IE stack. Ici: simplifié.
    pc_ = 0x8000'0080u;

    // Les branches pending sont annulées quand on prend une exception.
    branch_pending_ = false;
    branch_delay_slots_ = 0;
    branch_just_scheduled_ = false;
}

void Cpu::commit_pending_load()
{
    // Commit du "load delay slot": l'écriture du registre (rt) arrive *après* l'instruction
    // suivante.
    if (!pending_load_.valid)
    {
        return;
    }

    // r0 ignore toujours les écritures.
    if ((pending_load_.reg & 31u) != 0u)
    {
        set_reg(pending_load_.reg, pending_load_.value);
    }

    pending_load_.valid = 0;
}

Cpu::StepResult Cpu::step()
{
    // Une "step" = exécuter EXACTEMENT 1 instruction MIPS (plus éventuellement appliquer un
    // branchement programmé par l'instruction précédente, à cause du delay slot).
    //
    // Schéma simplifié:
    // 1) Fetch   : instr = mem[PC]
    // 2) Decode  : on extrait opcode/rs/rt/rd/imm/funct
    // 3) Execute : on modifie registres/mémoire et on planifie branch/jump si besoin
    // 4) Commit  : r0=0, et si un branch "pending" arrive à échéance, on applique PC=target
    StepResult r;
    r.pc = pc_;

    // -----------------------------
    // 1) FETCH
    // -----------------------------
    // Le R3000 (PS1) est little-endian.
    // On lit un mot 32-bit aligné. Si l'adresse n'est pas alignée, c'est une exception ADEL.
    uint32_t instr = 0;
    Bus::MemFault fault{};
    const uint32_t pc_phys = virt_to_phys(pc_);
    if (!bus_.read_u32(pc_phys, instr, fault))
    {
        // Address error on instruction fetch (ADEL).
        if (logger_)
        {
            rlog::logger_logf(
                logger_,
                rlog::Level::error,
                rlog::Category::exc,
                "IFETCH fault kind=%d vaddr=0x%08X paddr=0x%08X",
                (int)fault.kind,
                pc_,
                pc_phys
            );
        }
        raise_exception(EXC_ADEL, pc_, r.pc);
        r.kind = StepResult::Kind::ok; // on continue à exécuter au handler si présent
        return r;
    }
    r.instr = instr;

    if (logger_ && rlog::logger_enabled(logger_, rlog::Level::trace, rlog::Category::fetch))
    {
        rlog::logger_logf(
            logger_, rlog::Level::trace, rlog::Category::fetch, "PC=0x%08X INSTR=0x%08X", pc_, instr
        );
    }

    // Pour le mode --pretty, on capture ce qui change:
    // - "writeback" registre: lequel, ancienne valeur, nouvelle valeur
    // - et les accès mémoire LW/SW (adresse/valeur)
    uint32_t wb_reg = 0xFFFFFFFFu;
    uint32_t wb_old = 0;
    uint32_t wb_new = 0;
    int wb_valid = 0;

    int mem_valid = 0;
    const char* mem_op = "";
    uint32_t mem_addr = 0;
    uint32_t mem_val = 0;

    // Pour rendre le load delay slot lisible:
    // - wb2 = commit du load de l'instruction précédente (s'il y en a un)
    uint32_t wb2_reg = 0xFFFFFFFFu;
    uint32_t wb2_old = 0;
    uint32_t wb2_new = 0;
    int wb2_valid = 0;

    // Pour rendre la programmation du load lisible:
    // - ld = load programmé par l'instruction courante (qui sera commité après la suivante).
    uint32_t ld_reg = 0xFFFFFFFFu;
    uint32_t ld_val = 0;
    const char* ld_op = "";
    int ld_valid = 0;

    // Load programmé par l'instruction courante (à commiter après la suivante).
    PendingLoad next_pending_load{};
    next_pending_load.valid = 0;
    next_pending_load.reg = 0;
    next_pending_load.value = 0;

    // Sur MIPS, le PC avance "naturellement" de 4 (instructions 32-bit).
    // Les branches/jumps ne changent pas PC immédiatement: ils programment un target après le delay
    // slot.
    pc_ += 4;

    // Reset "just scheduled" ici: il correspond uniquement à la branche posée pendant CETTE step().
    branch_just_scheduled_ = false;

    // -----------------------------
    // 2) DECODE (extraction champs)
    // -----------------------------
    // Formats MIPS:
    // - R-type: opcode=0, champs rs/rt/rd/shamt/funct
    // - I-type: opcode != 0, champs rs/rt/imm16 (imm est souvent sign-extended)
    // - J-type: opcode=2/3, champ index 26-bit (target = (PC+4 upper) | (index<<2))
    const uint32_t opcode = op(instr);

    // -----------------------------
    // 3) EXECUTE (interpréteur)
    // -----------------------------
    // Interpréteur "switch-case": simple, lisible en live, et facile à étendre instruction par
    // instruction. On commence volontairement avec un sous-ensemble suffisant pour une démo.

    // Helpers mémoire (virtuel -> physique + exceptions):
    // - On traduit KSEG0/KSEG1 via virt_to_phys().
    // - En cas de faute, on déclenche une exception (ADEL/ADES) et on renvoie 0 (l'instruction
    //   courante ne doit pas continuer).
    auto load_u8 = [&](uint32_t vaddr, uint8_t& out) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.read_u8(paddr, out, f))
        {
            raise_exception(EXC_ADEL, vaddr, r.pc);
            return 0;
        }
        return 1;
    };
    auto load_u16 = [&](uint32_t vaddr, uint16_t& out) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.read_u16(paddr, out, f))
        {
            raise_exception(EXC_ADEL, vaddr, r.pc);
            return 0;
        }
        return 1;
    };
    auto load_u32 = [&](uint32_t vaddr, uint32_t& out) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.read_u32(paddr, out, f))
        {
            raise_exception(EXC_ADEL, vaddr, r.pc);
            return 0;
        }
        return 1;
    };
    auto store_u8 = [&](uint32_t vaddr, uint8_t v) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.write_u8(paddr, v, f))
        {
            raise_exception(EXC_ADES, vaddr, r.pc);
            return 0;
        }
        return 1;
    };
    auto store_u16 = [&](uint32_t vaddr, uint16_t v) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.write_u16(paddr, v, f))
        {
            raise_exception(EXC_ADES, vaddr, r.pc);
            return 0;
        }
        return 1;
    };
    auto store_u32 = [&](uint32_t vaddr, uint32_t v) -> int
    {
        Bus::MemFault f{};
        const uint32_t paddr = virt_to_phys(vaddr);
        if (!bus_.write_u32(paddr, v, f))
        {
            raise_exception(EXC_ADES, vaddr, r.pc);
            return 0;
        }
        return 1;
    };

    switch (opcode)
    {
        case 0x00:
            { // SPECIAL
                const uint32_t f = funct(instr);
                switch (f)
                {
                    case 0x00:
                        { // SLL (NOP si rd=rt=0 et shamt=0)
                            // SLL rd, rt, shamt
                            // Démo pédagogique: le NOP canonique sur MIPS est: SLL r0, r0, 0
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = shamt(instr);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = (gpr_[t] << s);
                                wb_valid = 1;
                            }
                            set_reg(d, gpr_[t] << s);
                            break;
                        }
                    case 0x02:
                        { // SRL
                            // SRL rd, rt, shamt (logical shift right, zero-fill)
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = shamt(instr);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = (gpr_[t] >> s);
                                wb_valid = 1;
                            }
                            set_reg(d, gpr_[t] >> s);
                            break;
                        }
                    case 0x03:
                        { // SRA
                            // SRA rd, rt, shamt (arithmetic shift right, sign-fill)
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = shamt(instr);
                            const uint32_t v = (uint32_t)(((int32_t)gpr_[t]) >> s);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x04:
                        { // SLLV
                            // SLLV rd, rt, rs (shift amount = rs & 31)
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = rs(instr);
                            const uint32_t sh = gpr_[s] & 31u;
                            const uint32_t v = gpr_[t] << sh;
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x06:
                        { // SRLV
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = rs(instr);
                            const uint32_t sh = gpr_[s] & 31u;
                            const uint32_t v = gpr_[t] >> sh;
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x07:
                        { // SRAV
                            const uint32_t d = rd(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t s = rs(instr);
                            const uint32_t sh = gpr_[s] & 31u;
                            const uint32_t v = (uint32_t)(((int32_t)gpr_[t]) >> sh);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x08:
                        { // JR
                            // JR rs : jump register (utile pour retours de fonctions via ra)
                            // Delay slot: l'instruction suivante s'exécute quand même.
                            const uint32_t s = rs(instr);
                            schedule_branch(gpr_[s]);
                            break;
                        }
                    case 0x09:
                        { // JALR
                            // JALR rd, rs (si rd=0 dans l'encodage, c'est souvent ra=31 en
                            // pratique) Link = adresse de retour = (PC de l'instruction suivante
                            // après delay slot) = old_pc + 8. Ici, pc_ vaut déjà old_pc+4, donc
                            // return = pc_ + 4.
                            const uint32_t s = rs(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t ra = pc_ + 4;
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = ra;
                                wb_valid = 1;
                            }
                            set_reg(d ? d : 31u, ra);
                            schedule_branch(gpr_[s]);
                            break;
                        }
                    case 0x0C:
                        { // SYSCALL
                            // SYSCALL
                            //
                            // Sur PS1, beaucoup d'outils/devcalls passent par des appels BIOS
                            // (tables A0/B0/C0) ou des mécanismes de debug.
                            //
                            // Pour notre émulateur "pédago live", on implémente aussi quelques
                            // "host syscalls" *optionnels* pour faire du printf/debug facilement,
                            // sans GPU/BIOS:
                            // - v0 = 0xFF00 : print_u32(a0)
                            // - v0 = 0xFF02 : putc(a0 & 0xFF)
                            // - v0 = 0xFF03 : print_cstr(a0)  (a0 = adresse virtuelle)
                            //
                            // Convention registres MIPS:
                            // - v0 = r2, a0 = r4
                            // Note: certains loaders utilisent ADDIU pour charger des constantes
                            // > 0x7FFF, ce qui sign-extend en 0xFFFFxxxx.
                            // Pour être tolérant (et pédagogique), on compare sur 16 bits.
                            const uint32_t svc = gpr_[2] & 0xFFFFu;
                            if (svc == 0xFF00u)
                            {
                                const uint32_t v = gpr_[4];
                                std::printf("[SYSCALL] %u (0x%08X)\n", v, v);
                            }
                            else if (svc == 0xFF02u)
                            {
                                const uint8_t ch = (uint8_t)(gpr_[4] & 0xFFu);
                                std::printf("%c", (char)ch);
                                std::fflush(stdout);
                            }
                            else if (svc == 0xFF03u)
                            {
                                // Lecture d'une C-string depuis la mémoire émulée.
                                // On cappe pour éviter les boucles infinies.
                                const uint32_t addr0 = gpr_[4];
                                uint32_t addr = addr0;
                                for (uint32_t i = 0; i < 1024; ++i)
                                {
                                    uint8_t b = 0;
                                    if (!load_u8(addr, b))
                                        break;
                                    if (b == 0)
                                        break;
                                    std::printf("%c", (char)b);
                                    addr++;
                                }
                                std::fflush(stdout);
                            }
                            else
                            {
                                // Sinon, comportement "réaliste": exception SYSCALL.
                                raise_exception(EXC_SYS, 0, r.pc);
                            }
                            break;
                        }
                    case 0x0D:
                        { // BREAK (démo: halt)
                            // BREAK: en vrai, déclenche une exception "Breakpoint".
                            // Ici on s'en sert comme stop propre pour la mini-ROM.
                            r.kind = StepResult::Kind::halted;
                            break;
                        }
                    case 0x10:
                        { // MFHI
                            const uint32_t d = rd(instr);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = hi_;
                                wb_valid = 1;
                            }
                            set_reg(d, hi_);
                            break;
                        }
                    case 0x11:
                        { // MTHI
                            const uint32_t s = rs(instr);
                            hi_ = gpr_[s];
                            break;
                        }
                    case 0x12:
                        { // MFLO
                            const uint32_t d = rd(instr);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = lo_;
                                wb_valid = 1;
                            }
                            set_reg(d, lo_);
                            break;
                        }
                    case 0x13:
                        { // MTLO
                            const uint32_t s = rs(instr);
                            lo_ = gpr_[s];
                            break;
                        }
                    case 0x18:
                        { // MULT (signed)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const int64_t a = (int32_t)gpr_[s];
                            const int64_t b = (int32_t)gpr_[t];
                            const int64_t p = a * b;
                            lo_ = (uint32_t)(p & 0xFFFF'FFFFll);
                            hi_ = (uint32_t)((uint64_t)p >> 32);
                            break;
                        }
                    case 0x19:
                        { // MULTU (unsigned)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint64_t a = (uint32_t)gpr_[s];
                            const uint64_t b = (uint32_t)gpr_[t];
                            const uint64_t p = a * b;
                            lo_ = (uint32_t)(p & 0xFFFF'FFFFull);
                            hi_ = (uint32_t)(p >> 32);
                            break;
                        }
                    case 0x1A:
                        { // DIV (signed)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const int32_t num = (int32_t)gpr_[s];
                            const int32_t den = (int32_t)gpr_[t];
                            if (den == 0)
                            {
                                // Résultat "unpredictable" sur MIPS. On laisse hi/lo inchangés et
                                // log.
                                if (logger_)
                                    rlog::logger_logf(
                                        logger_,
                                        rlog::Level::warn,
                                        rlog::Category::exec,
                                        "DIV by zero (HI/LO unchanged)"
                                    );
                                break;
                            }
                            lo_ = (uint32_t)(num / den);
                            hi_ = (uint32_t)(num % den);
                            break;
                        }
                    case 0x1B:
                        { // DIVU (unsigned)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t num = gpr_[s];
                            const uint32_t den = gpr_[t];
                            if (den == 0)
                            {
                                if (logger_)
                                    rlog::logger_logf(
                                        logger_,
                                        rlog::Level::warn,
                                        rlog::Category::exec,
                                        "DIVU by zero (HI/LO unchanged)"
                                    );
                                break;
                            }
                            lo_ = num / den;
                            hi_ = num % den;
                            break;
                        }
                    case 0x20:
                        { // ADD (signed overflow)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const int32_t a = (int32_t)gpr_[s];
                            const int32_t b = (int32_t)gpr_[t];
                            const int32_t res = a + b;
                            if (((a ^ b) >= 0) && ((a ^ res) < 0))
                            {
                                raise_exception(EXC_OV, 0, r.pc);
                                break;
                            }
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = (uint32_t)res;
                                wb_valid = 1;
                            }
                            set_reg(d, (uint32_t)res);
                            break;
                        }
                    case 0x21:
                        { // ADDU (no overflow)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = gpr_[s] + gpr_[t];
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x22:
                        { // SUB (signed overflow)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const int32_t a = (int32_t)gpr_[s];
                            const int32_t b = (int32_t)gpr_[t];
                            const int32_t res = a - b;
                            if (((a ^ b) < 0) && ((a ^ res) < 0))
                            {
                                raise_exception(EXC_OV, 0, r.pc);
                                break;
                            }
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = (uint32_t)res;
                                wb_valid = 1;
                            }
                            set_reg(d, (uint32_t)res);
                            break;
                        }
                    case 0x23:
                        { // SUBU
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = gpr_[s] - gpr_[t];
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x24:
                        { // AND
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = gpr_[s] & gpr_[t];
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x25:
                        { // OR
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = gpr_[s] | gpr_[t];
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x26:
                        { // XOR
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = gpr_[s] ^ gpr_[t];
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x27:
                        { // NOR
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = ~(gpr_[s] | gpr_[t]);
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x2A:
                        { // SLT (signed)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = ((int32_t)gpr_[s] < (int32_t)gpr_[t]) ? 1u : 0u;
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    case 0x2B:
                        { // SLTU (unsigned)
                            const uint32_t s = rs(instr);
                            const uint32_t t = rt(instr);
                            const uint32_t d = rd(instr);
                            const uint32_t v = (gpr_[s] < gpr_[t]) ? 1u : 0u;
                            if ((d & 31u) != 0u)
                            {
                                wb_reg = d;
                                wb_old = gpr_[d];
                                wb_new = v;
                                wb_valid = 1;
                            }
                            set_reg(d, v);
                            break;
                        }
                    default:
                        // Reserved instruction => exception RI
                        raise_exception(EXC_RI, 0, r.pc);
                        break;
                }
                break;
            }
        case 0x08:
            { // ADDI
                // ADDI rt, rs, imm
                // imm est SIGN-EXTENDED (16 -> 32).
                // ADDI déclenche une exception en cas d'overflow signé.
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t imm = (int16_t)imm_s(instr);
                const int32_t a = (int32_t)gpr_[s];
                const int32_t res = a + imm;
                // Overflow check:
                // si a et imm ont le même signe, mais res a un signe différent => overflow.
                // R3000: ADDI déclenche une exception "Overflow".
                if (((a ^ imm) >= 0) && ((a ^ res) < 0))
                {
                    if (logger_)
                        rlog::logger_logf(
                            logger_, rlog::Level::error, rlog::Category::exc, "ADDI overflow"
                        );
                    raise_exception(EXC_OV, 0, r.pc);
                    break;
                }
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = (uint32_t)res;
                    wb_valid = 1;
                }
                set_reg(t, (uint32_t)res);
                break;
            }
        case 0x09:
            { // ADDIU
                // ADDIU rt, rs, imm
                // Comme ADDI mais SANS exception overflow (c'est "unsigned" au sens MIPS).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t imm = (int16_t)imm_s(instr);
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = (uint32_t)((int32_t)gpr_[s] + imm);
                    wb_valid = 1;
                }
                set_reg(t, (uint32_t)((int32_t)gpr_[s] + imm));
                break;
            }
        case 0x0D:
            { // ORI
                // ORI rt, rs, imm
                // imm est ZERO-EXTENDED (16 -> 32).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = (gpr_[s] | (uint32_t)imm_u(instr));
                    wb_valid = 1;
                }
                set_reg(t, gpr_[s] | (uint32_t)imm_u(instr));
                break;
            }
        case 0x0F:
            { // LUI
                // LUI rt, imm : charge imm dans les 16 bits hauts (imm << 16)
                // Très utilisé pour former des adresses/constantes 32-bit avec ORI ensuite.
                const uint32_t t = rt(instr);
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = ((uint32_t)imm_u(instr) << 16);
                    wb_valid = 1;
                }
                set_reg(t, (uint32_t)imm_u(instr) << 16);
                break;
            }
        case 0x23:
            { // LW
                // LW rt, off(rs)
                // Adresse = rs + signext(off).
                // LW exige un alignement 4 octets (sinon exception Address Error).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint32_t v = 0;
                if (!load_u32(addr, v))
                {
                    // Exception déjà déclenchée (ADEL). On sort.
                    break;
                }
                mem_valid = 1;
                mem_op = "LW";
                mem_addr = addr;
                mem_val = v;
                // Load delay slot: pas de writeback immédiat.
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LW";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x2B:
            { // SW
                // SW rt, off(rs)
                // Comme LW mais écriture.
                // Dans notre bus, écrire à 0x1F000000 déclenche un "printf" côté hôte (démo live).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                if (!store_u32(addr, gpr_[t]))
                {
                    // Exception déjà déclenchée (ADES). On sort.
                    break;
                }
                mem_valid = 1;
                mem_op = "SW";
                mem_addr = addr;
                mem_val = gpr_[t];
                break;
            }
        case 0x05:
            { // BNE
                // BNE rs, rt, off
                // Si rs != rt, on branche à: (PC+4) + (signext(off) << 2)
                // MAIS: le branchement est appliqué après le delay slot.
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                if (gpr_[s] != gpr_[t])
                {
                    // target = PC+4 + (signext(off) << 2)
                    // Ici, pc_ a déjà été avancé à (old_pc + 4), donc target = pc_ + (off<<2).
                    const uint32_t target = pc_ + ((uint32_t)((int32_t)off << 2));
                    schedule_branch(target);
                }
                break;
            }
        case 0x04:
            { // BEQ
                // BEQ rs, rt, off (mêmes règles que BNE)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                if (gpr_[s] == gpr_[t])
                {
                    const uint32_t target = pc_ + ((uint32_t)((int32_t)off << 2));
                    schedule_branch(target);
                }
                break;
            }
        case 0x06:
            { // BLEZ
                // BLEZ rs, off : branch si rs <= 0 (signed)
                const uint32_t s = rs(instr);
                const int32_t off = (int16_t)imm_s(instr);
                if ((int32_t)gpr_[s] <= 0)
                {
                    const uint32_t target = pc_ + ((uint32_t)((int32_t)off << 2));
                    schedule_branch(target);
                }
                break;
            }
        case 0x07:
            { // BGTZ
                // BGTZ rs, off : branch si rs > 0 (signed)
                const uint32_t s = rs(instr);
                const int32_t off = (int16_t)imm_s(instr);
                if ((int32_t)gpr_[s] > 0)
                {
                    const uint32_t target = pc_ + ((uint32_t)((int32_t)off << 2));
                    schedule_branch(target);
                }
                break;
            }
        case 0x01:
            { // REGIMM (BLTZ/BGEZ + link variants)
                const uint32_t s = rs(instr);
                const uint32_t rt_field = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t target = pc_ + ((uint32_t)((int32_t)off << 2));

                const int32_t sv = (int32_t)gpr_[s];
                int take = 0;
                int link = 0;
                switch (rt_field)
                {
                    case 0x00: // BLTZ
                        take = (sv < 0);
                        break;
                    case 0x01: // BGEZ
                        take = (sv >= 0);
                        break;
                    case 0x10: // BLTZAL
                        take = (sv < 0);
                        link = 1;
                        break;
                    case 0x11: // BGEZAL
                        take = (sv >= 0);
                        link = 1;
                        break;
                    default:
                        raise_exception(EXC_RI, 0, r.pc);
                        break;
                }

                if (link && take)
                {
                    const uint32_t ra = pc_ + 4;
                    wb_reg = 31;
                    wb_old = gpr_[31];
                    wb_new = ra;
                    wb_valid = 1;
                    set_reg(31, ra);
                }

                if (take)
                {
                    schedule_branch(target);
                }
                break;
            }
        case 0x02:
            { // J
                // J index (J-type)
                // target = (PC+4 upper 4 bits) | (index << 2)
                // Delay slot aussi.
                const uint32_t target = (pc_ & 0xF000'0000u) | (jidx(instr) << 2);
                schedule_branch(target);
                break;
            }
        case 0x03:
            { // JAL
                // JAL index : jump + link (ra = old_pc + 8)
                const uint32_t target = (pc_ & 0xF000'0000u) | (jidx(instr) << 2);
                const uint32_t ra = pc_ + 4;
                wb_reg = 31;
                wb_old = gpr_[31];
                wb_new = ra;
                wb_valid = 1;
                set_reg(31, ra);
                schedule_branch(target);
                break;
            }
        case 0x0A:
            { // SLTI
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t imm = (int16_t)imm_s(instr);
                const uint32_t v = ((int32_t)gpr_[s] < imm) ? 1u : 0u;
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = v;
                    wb_valid = 1;
                }
                set_reg(t, v);
                break;
            }
        case 0x0B:
            { // SLTIU
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const uint32_t imm = (uint32_t)(int32_t)(int16_t)imm_s(instr);
                const uint32_t v = (gpr_[s] < imm) ? 1u : 0u;
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = v;
                    wb_valid = 1;
                }
                set_reg(t, v);
                break;
            }
        case 0x0C:
            { // ANDI (zero-extend)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const uint32_t imm = (uint32_t)imm_u(instr);
                const uint32_t v = gpr_[s] & imm;
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = v;
                    wb_valid = 1;
                }
                set_reg(t, v);
                break;
            }
        case 0x0E:
            { // XORI (zero-extend)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const uint32_t imm = (uint32_t)imm_u(instr);
                const uint32_t v = gpr_[s] ^ imm;
                if ((t & 31u) != 0u)
                {
                    wb_reg = t;
                    wb_old = gpr_[t];
                    wb_new = v;
                    wb_valid = 1;
                }
                set_reg(t, v);
                break;
            }
        case 0x20:
            { // LB
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint8_t b = 0;
                if (!load_u8(addr, b))
                    break;
                const uint32_t v = (uint32_t)(int32_t)(int8_t)b;
                mem_valid = 1;
                mem_op = "LB";
                mem_addr = addr;
                mem_val = b;
                // Load delay slot: pas de writeback immédiat.
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LB";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x24:
            { // LBU
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint8_t b = 0;
                if (!load_u8(addr, b))
                    break;
                const uint32_t v = (uint32_t)b;
                mem_valid = 1;
                mem_op = "LBU";
                mem_addr = addr;
                mem_val = b;
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LBU";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x21:
            { // LH
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint16_t h = 0;
                if (!load_u16(addr, h))
                    break;
                const uint32_t v = (uint32_t)(int32_t)(int16_t)h;
                mem_valid = 1;
                mem_op = "LH";
                mem_addr = addr;
                mem_val = h;
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LH";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x25:
            { // LHU
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint16_t h = 0;
                if (!load_u16(addr, h))
                    break;
                const uint32_t v = (uint32_t)h;
                mem_valid = 1;
                mem_op = "LHU";
                mem_addr = addr;
                mem_val = h;
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LHU";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x28:
            { // SB
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                if (!store_u8(addr, (uint8_t)(gpr_[t] & 0xFFu)))
                    break;
                mem_valid = 1;
                mem_op = "SB";
                mem_addr = addr;
                mem_val = gpr_[t] & 0xFFu;
                break;
            }
        case 0x29:
            { // SH
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                if (!store_u16(addr, (uint16_t)(gpr_[t] & 0xFFFFu)))
                    break;
                mem_valid = 1;
                mem_op = "SH";
                mem_addr = addr;
                mem_val = gpr_[t] & 0xFFFFu;
                break;
            }
        case 0x22:
            { // LWL (little-endian merge)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                const uint32_t base = addr & ~3u;
                uint32_t w = 0;
                if (!load_u32(base, w))
                    break;
                const uint32_t k = addr & 3u;
                uint32_t v = gpr_[t];
                switch (k)
                {
                    case 0:
                        v = (v & 0xFFFFFF00u) | (w >> 24);
                        break;
                    case 1:
                        v = (v & 0xFFFF0000u) | (w >> 16);
                        break;
                    case 2:
                        v = (v & 0xFF000000u) | (w >> 8);
                        break;
                    case 3:
                        v = w;
                        break;
                }
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LWL";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x26:
            { // LWR (little-endian merge)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                const uint32_t base = addr & ~3u;
                uint32_t w = 0;
                if (!load_u32(base, w))
                    break;
                const uint32_t k = addr & 3u;
                uint32_t v = gpr_[t];
                switch (k)
                {
                    case 0:
                        v = w;
                        break;
                    case 1:
                        v = (v & 0x000000FFu) | (w << 8);
                        break;
                    case 2:
                        v = (v & 0x0000FFFFu) | (w << 16);
                        break;
                    case 3:
                        v = (v & 0x00FFFFFFu) | (w << 24);
                        break;
                }
                next_pending_load.valid = 1;
                next_pending_load.reg = t;
                next_pending_load.value = v;
                ld_valid = 1;
                ld_op = "LWR";
                ld_reg = t;
                ld_val = v;
                break;
            }
        case 0x2A:
            { // SWL (little-endian merge)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                const uint32_t base = addr & ~3u;
                uint32_t w = 0;
                if (!load_u32(base, w))
                    break;
                const uint32_t k = addr & 3u;
                const uint32_t v = gpr_[t];
                switch (k)
                {
                    case 0:
                        w = (w & 0xFFFFFF00u) | (v >> 24);
                        break;
                    case 1:
                        w = (w & 0xFFFF0000u) | (v >> 16);
                        break;
                    case 2:
                        w = (w & 0xFF000000u) | (v >> 8);
                        break;
                    case 3:
                        w = v;
                        break;
                }
                store_u32(base, w);
                break;
            }
        case 0x2E:
            { // SWR (little-endian merge)
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr);
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                const uint32_t base = addr & ~3u;
                uint32_t w = 0;
                if (!load_u32(base, w))
                    break;
                const uint32_t k = addr & 3u;
                const uint32_t v = gpr_[t];
                switch (k)
                {
                    case 0:
                        w = v;
                        break;
                    case 1:
                        w = (w & 0x000000FFu) | (v << 8);
                        break;
                    case 2:
                        w = (w & 0x0000FFFFu) | (v << 16);
                        break;
                    case 3:
                        w = (w & 0x00FFFFFFu) | (v << 24);
                        break;
                }
                store_u32(base, w);
                break;
            }
        case 0x10:
            { // COP0
                const uint32_t rs_field = rs(instr);
                const uint32_t t = rt(instr);
                const uint32_t d = rd(instr);
                if (rs_field == 0x00)
                {
                    // MFC0 rt, rd
                    const uint32_t v = cop0_[d & 31u];

                    // Le R3000 a aussi une latence sur les moves depuis coprocessor.
                    // Pour rester simple/pédago: on réutilise le mécanisme de load delay slot.
                    next_pending_load.valid = 1;
                    next_pending_load.reg = t;
                    next_pending_load.value = v;
                    ld_valid = 1;
                    ld_op = "MFC0";
                    ld_reg = t;
                    ld_val = v;
                }
                else if (rs_field == 0x04)
                {
                    // MTC0 rt, rd
                    cop0_[d & 31u] = gpr_[t];
                }
                else if (rs_field == 0x10)
                {
                    // CO (RFE)
                    if ((instr & 0x3Fu) == 0x10u)
                    {
                        // RFE: restore mode/IE stack (simplifié).
                        // status[5:0] = status[5:0] >> 2
                        uint32_t st = cop0_[COP0_STATUS];
                        st = (st & ~0x3Fu) | ((st >> 2) & 0x3Fu);
                        cop0_[COP0_STATUS] = st;
                    }
                    else
                    {
                        raise_exception(EXC_RI, 0, r.pc);
                    }
                }
                else
                {
                    raise_exception(EXC_RI, 0, r.pc);
                }
                break;
            }
        case 0x12:
            { // COP2 (GTE)
                // COP2 = GTE.
                //
                // On sépare complètement le GTE du CPU:
                // - Ici, le CPU ne fait que décoder l'instruction COP2 (rs/rt/rd/funct)
                // - et délègue au module gte::Gte pour lire/écrire les registres GTE.
                //
                // Encodage des transferts (convention MIPS):
                // - MFC2 rt, rd : rs=0  (read GTE data reg -> CPU reg)
                // - CFC2 rt, rd : rs=2  (read GTE ctrl reg -> CPU reg)
                // - MTC2 rt, rd : rs=4  (write CPU reg -> GTE data reg)
                // - CTC2 rt, rd : rs=6  (write CPU reg -> GTE ctrl reg)
                const uint32_t rs_field = rs(instr);
                const uint32_t t = rt(instr);
                const uint32_t d = rd(instr);

                if (rs_field == 0x00)
                {
                    // MFC2: lecture data reg GTE -> CPU (avec load delay slot)
                    const uint32_t v = gte_.read_data(d);
                    next_pending_load.valid = 1;
                    next_pending_load.reg = t;
                    next_pending_load.value = v;
                    ld_valid = 1;
                    ld_op = "MFC2";
                    ld_reg = t;
                    ld_val = v;
                }
                else if (rs_field == 0x02)
                {
                    // CFC2: lecture ctrl reg GTE -> CPU (avec load delay slot)
                    const uint32_t v = gte_.read_ctrl(d);
                    next_pending_load.valid = 1;
                    next_pending_load.reg = t;
                    next_pending_load.value = v;
                    ld_valid = 1;
                    ld_op = "CFC2";
                    ld_reg = t;
                    ld_val = v;
                }
                else if (rs_field == 0x04)
                {
                    // MTC2: écriture CPU -> data reg GTE
                    gte_.write_data(d, gpr_[t]);
                }
                else if (rs_field == 0x06)
                {
                    // CTC2: écriture CPU -> ctrl reg GTE
                    gte_.write_ctrl(d, gpr_[t]);
                }
                else if (rs_field == 0x10)
                {
                    // CO: commande GTE (RTPS/MVMVA/NCLIP/...)
                    if (!gte_.execute(instr))
                    {
                        raise_exception(EXC_RI, 0, r.pc);
                    }
                }
                else
                {
                    // Commandes GTE (RTPS/MVMVA/...) viendront ici (rs=0x10/0x12 selon forme).
                    raise_exception(EXC_RI, 0, r.pc);
                }
                break;
            }
        case 0x11: // COP1 (absent sur PS1)
        case 0x13: // COP3
            {
                raise_exception(EXC_RI, 0, r.pc);
                break;
            }
        case 0x30: // LWC0
        case 0x31: // LWC1
        case 0x33: // LWC3
        case 0x38: // SWC0
        case 0x39: // SWC1
        case 0x3B: // SWC3
            {
                // Les transferts coprocessor seront implémentés quand le GTE sera ajouté.
                raise_exception(EXC_RI, 0, r.pc);
                break;
            }
        case 0x32:
            { // LWC2 (GTE load)
                // LWC2 rt, off(rs)
                // Equivalent à LW, mais la destination est un registre "data" du GTE (COP2).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr); // numéro de registre GTE (0..31)
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                uint32_t v = 0;
                if (!load_u32(addr, v))
                {
                    break;
                }
                mem_valid = 1;
                mem_op = "LWC2";
                mem_addr = addr;
                mem_val = v;

                // Pour l'instant: écriture immédiate dans le GTE (pédago).
                // Si on veut coller plus finement au timing hardware, on pourra ajouter une
                // latence.
                gte_.lwc2(t, v);
                break;
            }
        case 0x3A:
            { // SWC2 (GTE store)
                // SWC2 rt, off(rs)
                // Equivalent à SW, mais la source est un registre "data" du GTE (COP2).
                const uint32_t s = rs(instr);
                const uint32_t t = rt(instr); // numéro de registre GTE (0..31)
                const int32_t off = (int16_t)imm_s(instr);
                const uint32_t addr = (uint32_t)((int32_t)gpr_[s] + off);
                const uint32_t v = gte_.swc2(t);
                if (!store_u32(addr, v))
                {
                    break;
                }
                mem_valid = 1;
                mem_op = "SWC2";
                mem_addr = addr;
                mem_val = v;
                break;
            }
        case 0x2F:
            { // CACHE (ignoré pour l'instant)
                // Sur R3000A, l'opcode CACHE existe selon certaines docs.
                // Pour une émulation éducative simple: on l'ignore (NOP).
                break;
            }
        default:
            raise_exception(EXC_RI, 0, r.pc);
            break;
    }

    // -----------------------------
    // 4) COMMIT / INVARIANTS
    // -----------------------------
    // 4a) Commit du load en attente (instruction précédente).
    // Important: sur R3000, la valeur du load arrive après *une* instruction.
    if (pending_load_.valid)
    {
        wb2_reg = pending_load_.reg;
        wb2_old = gpr_[pending_load_.reg & 31u];
        wb2_new = pending_load_.value;
        wb2_valid = ((pending_load_.reg & 31u) != 0u) ? 1 : 0;
    }
    commit_pending_load();

    // 4b) Programme le pending load issu de l'instruction courante.
    pending_load_ = next_pending_load;

    // 4c) On force r0=0 (au cas où un bug écrirait dessus).
    gpr_[0] = 0;

    // Application du delay slot:
    // - Quand une instruction "branche" (BEQ/BNE/J/JR) s'exécute, elle programme
    // branch_pending_=true
    //   et branch_delay_slots_=1.
    // - L'instruction suivante (delay slot) s'exécute normalement.
    // - A la fin de la step du delay slot, on décrémente branch_delay_slots_ et quand il atteint 0,
    //   on applique pc_=branch_target_.
    //
    // Le flag branch_just_scheduled_ nous évite d'appliquer/décrémenter le delay slot dans la même
    // step qui vient juste de programmer le branchement.
    if (branch_pending_)
    {
        // Si la branche a été planifiée pendant CETTE step, on ne touche pas au compteur (le delay
        // slot est la prochaine instr).
        if (!branch_just_scheduled_)
        {
            if (branch_delay_slots_ > 0)
            {
                branch_delay_slots_--;
                if (branch_delay_slots_ == 0)
                {
                    pc_ = branch_target_;
                    branch_pending_ = false;
                }
            }
            else
            {
                pc_ = branch_target_;
                branch_pending_ = false;
            }
        }
    }

    if (pretty_)
    {
        // Mode lisible "désassemblage":
        // On reconstruit une string à la volée pour l'affichage live.
        // Important: ce n'est PAS un désassembleur complet, juste les instructions qu'on supporte.
        char line[256];
        line[0] = '\0';

        // Désassemblage minimal pour les opcodes supportés.
        const uint32_t o = opcode;
        if (o == 0x0F)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  LUI  %s, 0x%04X",
                r.pc,
                reg_name(rt(instr)),
                imm_u(instr)
            );
        }
        else if (o == 0x0D)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  ORI  %s, %s, 0x%04X",
                r.pc,
                reg_name(rt(instr)),
                reg_name(rs(instr)),
                imm_u(instr)
            );
        }
        else if (o == 0x09)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  ADDIU %s, %s, %d",
                r.pc,
                reg_name(rt(instr)),
                reg_name(rs(instr)),
                (int)(int16_t)imm_s(instr)
            );
        }
        else if (o == 0x08)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  ADDI %s, %s, %d",
                r.pc,
                reg_name(rt(instr)),
                reg_name(rs(instr)),
                (int)(int16_t)imm_s(instr)
            );
        }
        else if (o == 0x2B)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  SW   %s, %d(%s)",
                r.pc,
                reg_name(rt(instr)),
                (int)(int16_t)imm_s(instr),
                reg_name(rs(instr))
            );
        }
        else if (o == 0x23)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  LW   %s, %d(%s)",
                r.pc,
                reg_name(rt(instr)),
                (int)(int16_t)imm_s(instr),
                reg_name(rs(instr))
            );
        }
        else if (o == 0x05)
        {
            const int16_t off = (int16_t)imm_s(instr);
            const uint32_t target = (r.pc + 4) + ((uint32_t)((int32_t)off << 2));
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  BNE  %s, %s, 0x%08X",
                r.pc,
                reg_name(rs(instr)),
                reg_name(rt(instr)),
                target
            );
        }
        else if (o == 0x04)
        {
            const int16_t off = (int16_t)imm_s(instr);
            const uint32_t target = (r.pc + 4) + ((uint32_t)((int32_t)off << 2));
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  BEQ  %s, %s, 0x%08X",
                r.pc,
                reg_name(rs(instr)),
                reg_name(rt(instr)),
                target
            );
        }
        else if (o == 0x02)
        {
            const uint32_t target = ((r.pc + 4) & 0xF000'0000u) | (jidx(instr) << 2);
            std::snprintf(line, sizeof(line), "PC=%08X  J    0x%08X", r.pc, target);
        }
        else if (o == 0x00 && funct(instr) == 0x08)
        {
            std::snprintf(line, sizeof(line), "PC=%08X  JR   %s", r.pc, reg_name(rs(instr)));
        }
        else if (o == 0x00 && funct(instr) == 0x00)
        {
            std::snprintf(
                line,
                sizeof(line),
                "PC=%08X  SLL  %s, %s, %u",
                r.pc,
                reg_name(rd(instr)),
                reg_name(rt(instr)),
                shamt(instr)
            );
        }
        else if (o == 0x00 && funct(instr) == 0x0D)
        {
            std::snprintf(line, sizeof(line), "PC=%08X  BREAK", r.pc);
        }
        else
        {
            std::snprintf(line, sizeof(line), "PC=%08X  INSTR 0x%08X", r.pc, instr);
        }

        if (wb_valid)
        {
            char tmp[128];
            std::snprintf(
                tmp, sizeof(tmp), "  ; %s:0x%08X->0x%08X", reg_name(wb_reg), wb_old, wb_new
            );
            ::strncat(line, tmp, sizeof(line) - ::strlen(line) - 1);
        }

        if (mem_valid)
        {
            char tmp[128];
            std::snprintf(tmp, sizeof(tmp), "  ; %s [0x%08X]=0x%08X", mem_op, mem_addr, mem_val);
            ::strncat(line, tmp, sizeof(line) - ::strlen(line) - 1);
        }

        if (ld_valid)
        {
            char tmp[128];
            std::snprintf(
                tmp, sizeof(tmp), "  ; (LD sched) %s -> %s=0x%08X", ld_op, reg_name(ld_reg), ld_val
            );
            ::strncat(line, tmp, sizeof(line) - ::strlen(line) - 1);
        }

        if (wb2_valid)
        {
            char tmp[128];
            std::snprintf(
                tmp,
                sizeof(tmp),
                "  ; (LD commit) %s:0x%08X->0x%08X",
                reg_name(wb2_reg),
                wb2_old,
                wb2_new
            );
            ::strncat(line, tmp, sizeof(line) - ::strlen(line) - 1);
        }

        std::printf("%s\n", line);
    }

    // Debug log "exec"
    if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exec))
    {
        rlog::logger_logf(
            logger_,
            rlog::Level::debug,
            rlog::Category::exec,
            "PC->0x%08X r1=%" PRIu32 " r2=%" PRIu32 " r3=%" PRIu32,
            pc_,
            gpr_[1],
            gpr_[2],
            gpr_[3]
        );
    }

    return r;
}

} // namespace r3000
