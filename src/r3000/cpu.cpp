#include "cpu.h"

#include <cerrno>
#include <cinttypes>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "../cdrom/cdrom.h"

#if defined(_WIN32)
#include <direct.h>
#endif

namespace r3000
{

static uint64_t agent_now_ms()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

static void agent_log_json_begin(std::FILE*& f)
{
    // Ensure debug directory exists (best-effort) and surface fopen failures.
#if defined(_WIN32)
    // _mkdir returns -1 if already exists; ignore.
    _mkdir("e:\\Projects\\github\\Live\\R3000-Emu\\.cursor");
#endif
    f = std::fopen("e:\\Projects\\github\\Live\\R3000-Emu\\.cursor\\debug.log", "ab");
    if (!f)
    {
        static int s_reported = 0;
        if (!s_reported)
        {
            s_reported = 1;
            const int e = errno;
            std::fprintf(
                stderr,
                "[agent] failed to open debug log '%s' (errno=%d)\n",
                "e:\\Projects\\github\\Live\\R3000-Emu\\.cursor\\debug.log",
                e
            );
            std::fflush(stderr);
        }
    }
}

static void agent_log_json_end(std::FILE* f)
{
    if (f)
        std::fclose(f);
}

static int is_printable_ascii(uint8_t b)
{
    if (b == '\t' || b == '\r' || b == '\n')
        return 1;
    return (b >= 0x20 && b <= 0x7Eu) ? 1 : 0;
}

static void text_flush_line(flog::Sink& s, const flog::Clock& c, int has_clock, char* buf, uint32_t& pos)
{
    if (!has_clock || !s.f || !buf || pos == 0)
    {
        pos = 0;
        if (buf)
            buf[0] = '\0';
        return;
    }

    buf[pos] = '\0';
    flog::logf(s, c, flog::Level::info, "TEXT", "%s", buf);
    pos = 0;
    buf[0] = '\0';
}

static void text_push_char(flog::Sink& s, const flog::Clock& c, int has_clock, char* buf, uint32_t cap, uint32_t& pos, uint8_t ch)
{
    if (!has_clock || !buf || cap < 2)
        return;

    if (ch == '\n')
    {
        text_flush_line(s, c, has_clock, buf, pos);
        return;
    }

    // Remplacer les bytes non imprimables pour garder le log lisible.
    if (!is_printable_ascii(ch))
        ch = '.';

    if (pos + 1 >= cap)
    {
        text_flush_line(s, c, has_clock, buf, pos);
    }

    buf[pos++] = (char)ch;
}

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

static const char* psx_mmio_name(uint32_t phys_addr)
{
    // Petits alias pour rendre la trace BIOS lisible (pas un mapping complet).
    // On nomme surtout ce que le BIOS touche très tôt (mem ctrl, IRQ, DMA, CDROM, GPU).
    switch (phys_addr)
    {
        // MEMCTRL
        case 0x1F801000u:
            return "MEMCTRL1_EXP1_BASE";
        case 0x1F801004u:
            return "MEMCTRL1_EXP2_BASE";
        case 0x1F801008u:
            return "MEMCTRL1_EXP1_DELAY";
        case 0x1F80100Cu:
            return "MEMCTRL1_EXP3_DELAY";
        case 0x1F801010u:
            return "MEMCTRL2_BIOS_CFG";
        case 0x1F801014u:
            return "MEMCTRL2_SPU_DELAY";
        case 0x1F801018u:
            return "MEMCTRL2_CDROM_DELAY";
        case 0x1F80101Cu:
            return "MEMCTRL2_EXP2_DELAY";
        case 0x1F801060u:
            return "RAM_SIZE";

        // IRQ
        case 0x1F801070u:
            return "I_STAT";
        case 0x1F801074u:
            return "I_MASK";

        // Timers (base)
        case 0x1F801100u:
            return "TMR0_COUNT";
        case 0x1F801104u:
            return "TMR0_MODE";
        case 0x1F801108u:
            return "TMR0_TARGET";
        case 0x1F801110u:
            return "TMR1_COUNT";
        case 0x1F801114u:
            return "TMR1_MODE";
        case 0x1F801118u:
            return "TMR1_TARGET";
        case 0x1F801120u:
            return "TMR2_COUNT";
        case 0x1F801124u:
            return "TMR2_MODE";
        case 0x1F801128u:
            return "TMR2_TARGET";

        // DMA (juste quelques registres clés)
        case 0x1F8010F0u:
            return "DPCR";
        case 0x1F8010F4u:
            return "DICR";

        // CDROM
        case 0x1F801800u:
            return "CDROM_IDX/STAT";
        case 0x1F801801u:
            return "CDROM_CMD";
        case 0x1F801802u:
            return "CDROM_PARAM";
        case 0x1F801803u:
            return "CDROM_RESP/DATA";

        // GPU
        case 0x1F801810u:
            return "GPU_GP0";
        case 0x1F801814u:
            return "GPU_GP1";

        // Debug/demo: print MMIO (not PS1-accurate, just for live)
        case 0x1F000000u:
            return "HOST_MMIO_PRINT";

        // Cache control (KSEG2)
        case 0xFFFE0130u:
            return "CACHE_CTRL";

        default:
            break;
    }
    return nullptr;
}

static int psx_is_mmio(uint32_t phys_addr)
{
    // I/O space principal + quelques blocs communs.
    if (phys_addr >= 0x1F801000u && phys_addr < 0x1F803000u)
        return 1;
    if (phys_addr >= 0x1F000000u && phys_addr < 0x1F010000u)
        return 1; // EXP1 expansion port region
    if (phys_addr == 0xFFFE0130u)
        return 1;
    return 0;
}

Cpu::Cpu(Bus& bus, rlog::Logger* logger) : bus_(bus), logger_(logger)
{
}

void Cpu::set_hle_vectors(int enabled)
{
    hle_vectors_ = enabled ? 1 : 0;
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
    // COP0 Status reset:
    // Sur PS1, le BIOS démarre avec BEV=1 (boot exception vectors en ROM @ 0xBFC00180).
    // On reste minimal, mais BEV=1 est important sinon la 1ère exception saute en RAM vide @ 0x80000080.
    cop0_[COP0_STATUS] = (1u << 22); // BEV=1, IE=0
    // DuckStation (and real PSX) expose non-zero reserved/CE bits here; keep them stable.
    // Observed via GDB: Cause=0x30000000 during BIOS bring-up.
    cop0_[COP0_CAUSE] = 0x3000'0000u;
    cop0_[COP0_EPC] = 0;
    cop0_[COP0_BADVADDR] = 0;

    pending_load_.valid = 0;
    pending_load_.reg = 0;
    pending_load_.value = 0;

    for (uint32_t i = 0; i < (uint32_t)icache_data_.size(); ++i)
    {
        icache_data_[i] = 0;
    }

    // HLE (bring-up) init: allocator kernel + structures.
    kalloc_ptr_ = 0xA000'E000u;
    kalloc_end_ = 0xA000'E000u + 0x2000u; // 8KB
    entryint_struct_addr_ = 0x0000'00D0u; // zone "unused/reserved" d'après la RAM map BIOS
    entryint_hook_addr_ = 0;
    for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
    {
        hle_events_[i] = {};
    }
    hle_vblank_div_ = 0;
    hle_pseudo_vblank_ = 0;

    // HLE File I/O init.
    for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_files_) / sizeof(hle_files_[0])); ++i)
    {
        hle_files_[i] = {};
        hle_files_[i].used = 0;
    }
    hle_last_error_ = 0;
    hle_wait_event_calls_ = 0;
    hle_mark_ready_calls_ = 0;
    dbg_loop_dumped_ = 0;
    dbg_loop_patched_ = 0;
    dbg_ef30_dumped_ = 0;
    spin_pc_ = 0;
    spin_count_ = 0;

    for (int i = 0; i < 256; ++i)
    {
        recent_pc_[i] = 0;
        recent_instr_[i] = 0;
    }
    recent_pos_ = 0;
    stopped_on_high_ram_ = 0;

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
    // MIPS: si l'exception arrive dans un delay slot, on met BD=1 et EPC = adresse de la branch (PC-4).
    // Dans notre modèle:
    // - pendant l'instruction du delay slot: branch_pending_=true, branch_delay_slots_=1, branch_just_scheduled_=false
    int in_delay_slot = (branch_pending_ && !branch_just_scheduled_ && (branch_delay_slots_ == 1)) ? 1 : 0;

    // IRQ (EXC_INT) est pris "entre" instructions.
    // Pour le bring-up BIOS on force BD=0 et EPC=PC courant (pas de PC-4).
    if ((code & 0x1Fu) == EXC_INT)
    {
        in_delay_slot = 0;
    }

    const uint32_t epc = in_delay_slot ? (pc_of_fault - 4u) : pc_of_fault;

    cop0_[COP0_EPC] = epc;
    cop0_[COP0_BADVADDR] = badvaddr;

    cop0_[COP0_CAUSE] &= ~((uint32_t)0x1Fu << 2);
    cop0_[COP0_CAUSE] |= ((code & 0x1Fu) << 2);
    // BD bit (bit31): Branch Delay.
    if (in_delay_slot)
        cop0_[COP0_CAUSE] |= (1u << 31);
    else
        cop0_[COP0_CAUSE] &= ~(1u << 31);

    // Status "stack" (KU/IE):
    // R3000: bits 5..0 = {KUo,IEo,KUp,IEp,KUc,IEc}
    // Exception entry: on pousse (old<-prev, prev<-cur, cur<-0).
    // L'instruction RFE fera l'opération inverse (>>2).
    {
        uint32_t st = cop0_[COP0_STATUS];
        const uint32_t low = st & 0x3Fu;
        const uint32_t pushed = ((low & 0x0Fu) << 2) & 0x3Fu;
        st = (st & ~0x3Fu) | pushed;
        cop0_[COP0_STATUS] = st;
    }

    // Exception vector:
    // - BEV=1 => boot exception vectors en ROM (BIOS): 0xBFC00180
    // - BEV=0 => common exception vector en RAM:       0x80000080
    const uint32_t st = cop0_[COP0_STATUS];
    const int bev = (st & (1u << 22)) ? 1 : 0;
    pc_ = bev ? 0xBFC0'0180u : 0x8000'0080u;

    // Debug: log ALL exceptions (not just INT)
    {
        static int all_exc_count = 0;
        static int exc_by_type[16] = {};
        static const char* exc_names[] = {"INT", "MOD", "TLBL", "TLBS", "ADEL", "ADES", "IBE", "DBE",
                                          "SYS", "BP", "RI", "CpU", "OV"};
        const char* name = (code < 13) ? exc_names[code] : "???";
        ++all_exc_count;
        if (code < 16) exc_by_type[code]++;
        if (all_exc_count <= 50 || (all_exc_count % 10000 == 0))
        {
            std::fprintf(stderr, "[CPU] EXC %s (code=%u) #%d: EPC=0x%08X vector=0x%08X status=0x%08X (INT=%d SYS=%d BP=%d)\n",
                name, code, all_exc_count, epc, pc_, st, exc_by_type[0], exc_by_type[8], exc_by_type[9]);
            std::fflush(stderr);
        }
    }

    if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
    {
        rlog::logger_logf(
            logger_,
            rlog::Level::debug,
            rlog::Category::exc,
            "EXC code=%u EPC=0x%08X BadVAddr=0x%08X BEV=%d BD=%d -> vector=0x%08X",
            code,
            epc,
            badvaddr,
            bev,
            in_delay_slot,
            pc_
        );
    }

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

    // Détection simple de boucle (même PC répété).
    if (pc_ == spin_pc_)
        spin_count_++;
    else
    {
        spin_pc_ = pc_;
        spin_count_ = 1;
    }

    // Debug: trace key addresses to understand exception handler flow
    {
        static int exc_vec_trace_count = 0;
        static int kernel_trace_count = 0;

        // Trace exception vector entry (0x80000080)
        if (pc_ == 0x8000'0080u)
        {
            ++exc_vec_trace_count;
            if (exc_vec_trace_count <= 3)
            {
                const uint32_t cause = cop0_[COP0_CAUSE];
                const uint32_t epc = cop0_[COP0_EPC];
                // Read all 4 instructions at 0x80000080
                uint32_t instr0 = 0, instr1 = 0, instr2 = 0, instr3 = 0;
                Bus::MemFault f{};
                (void)bus_.read_u32(0x00000080u, instr0, f);
                (void)bus_.read_u32(0x00000084u, instr1, f);
                (void)bus_.read_u32(0x00000088u, instr2, f);
                (void)bus_.read_u32(0x0000008Cu, instr3, f);
                std::fprintf(stderr, "[CPU] EXC_VEC entry #%d: PC=0x80000080 cause=0x%08X epc=0x%08X ra=0x%08X instr=[0x%08X,0x%08X,0x%08X,0x%08X]\n",
                    exc_vec_trace_count, cause, epc, gpr_[31], instr0, instr1, instr2, instr3);
                std::fflush(stderr);
            }
        }

        // Trace low kernel area (0x00001000-0x00001100) where handler lives
        if (pc_ >= 0x0000'1000u && pc_ < 0x0000'1100u)
        {
            ++kernel_trace_count;
            if (kernel_trace_count <= 5 || (kernel_trace_count % 100000 == 0))
            {
                std::fprintf(stderr, "[CPU] KERNEL PC=0x%08X #%d ra=0x%08X v0=0x%08X\n",
                    pc_, kernel_trace_count, gpr_[31], gpr_[2]);
                std::fflush(stderr);
            }
        }

        // Trace the exception vector jump target area (0x80000080-0x800000C0)
        static int vec_area_trace = 0;
        if (pc_ >= 0x8000'0080u && pc_ < 0x8000'00C0u)
        {
            ++vec_area_trace;
            if (vec_area_trace <= 5)
            {
                std::fprintf(stderr, "[CPU] VEC_AREA PC=0x%08X #%d status=0x%08X\n",
                    pc_, vec_area_trace, cop0_[COP0_STATUS]);
                std::fflush(stderr);
            }
        }

        // Trace the exception handler target 0x00000C80 area (expanded to 0x0D80 for return paths)
        static int exc_handler_trace = 0;
        if (pc_ >= 0x0000'0C80u && pc_ < 0x0000'0E00u)
        {
            ++exc_handler_trace;
            if (exc_handler_trace <= 5)
            {
                std::fprintf(stderr, "[CPU] EXC_HANDLER PC=0x%08X #%d status=0x%08X ra=0x%08X k0=0x%08X\n",
                    pc_, exc_handler_trace, cop0_[COP0_STATUS], gpr_[31], gpr_[26]);
                std::fflush(stderr);
            }
            // When entering the loop area (0xDE8-0xDFC), dump instructions once
            static int loop_dump_done = 0;
            if (pc_ == 0x0000'0DE8u && loop_dump_done == 0)
            {
                loop_dump_done = 1;
                Bus::MemFault f{};
                uint32_t ins[8];
                for (int i = 0; i < 8; i++)
                    (void)bus_.read_u32(0x00000DE8u + i * 4, ins[i], f);
                std::fprintf(stderr, "[CPU] LOOP DUMP at 0xDE8: %08X %08X %08X %08X %08X %08X %08X %08X\n",
                    ins[0], ins[1], ins[2], ins[3], ins[4], ins[5], ins[6], ins[7]);
                std::fflush(stderr);
            }
        }

        // Trace when status register transitions from IEc=0 to IEc=1 (outside RFE)
        static uint32_t prev_status = 0;
        static int status_trans_count = 0;
        if ((prev_status & 1u) == 0 && (cop0_[COP0_STATUS] & 1u) == 1)
        {
            ++status_trans_count;
            if (status_trans_count <= 3)
            {
                std::fprintf(stderr, "[CPU] STATUS IEc 0->1 #%d: PC=0x%08X status=0x%08X->0x%08X\n",
                    status_trans_count, pc_, prev_status, cop0_[COP0_STATUS]);
                std::fflush(stderr);
            }
        }
        prev_status = cop0_[COP0_STATUS];
    }

    // Debug bring-up: dump une fois quand on tombe dans la boucle connue.
    // (Aucune trace CPU continue: juste un snapshot pour comprendre ce qui est attendu.)
    // Si on tombe dans la boucle connue, on dump le contexte pour investiguer la cause réelle
    // (pas de patch: on corrige l'émulation).
    if (!dbg_loop_patched_ && pc_ == 0x8005EE80u)
    {
        dbg_loop_patched_ = 1;
    }

    if (!dbg_loop_dumped_ && sys_has_clock_ && pc_ == 0x8005EE80u)
    {
        dbg_loop_dumped_ = 1;
        uint32_t ins[32]{};
        for (uint32_t i = 0; i < 32; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        // Peek quelques globals utilisés par cette routine (valeurs typiquement attendues par le BIOS).
        uint32_t gptr = 0;
        uint32_t gval = 0;
        {
            Bus::MemFault f{};
            (void)bus_.read_u32(virt_to_phys(0x8009A204u), gptr, f);
        }
        if (gptr)
        {
            Bus::MemFault f{};
            (void)bus_.read_u32(virt_to_phys(gptr), gval, f);
        }

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop PC=0x%08X v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X sp=0x%08X ra=0x%08X "
            "t6=0x%08X t7=0x%08X t8=0x%08X t9=0x%08X at=0x%08X gp=0x%08X status=0x%08X cause=0x%08X pend=0x%08X",
            pc_,
            gpr_[2],
            gpr_[4],
            gpr_[5],
            gpr_[6],
            gpr_[7],
            gpr_[29],
            gpr_[31],
            gpr_[14],
            gpr_[15],
            gpr_[24],
            gpr_[25],
            gpr_[1],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE],
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG globals: [0x8009A204]=0x%08X *(that)=0x%08X",
            gptr,
            gval
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG ins16-23: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[16],
            ins[17],
            ins[18],
            ins[19],
            ins[20],
            ins[21],
            ins[22],
            ins[23]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG ins24-31: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[24],
            ins[25],
            ins[26],
            ins[27],
            ins[28],
            ins[29],
            ins[30],
            ins[31]
        );
        // #region agent log H1
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H1_IRQ_OR_WAIT\",\"location\":\"cpu.cpp:dbg_loop_8005EE80\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"v0\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"t6\":\"0x%08X\",\"t7\":\"0x%08X\",\"t8\":\"0x%08X\",\"t9\":\"0x%08X\",\"at\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"pend\":\"0x%08X\",\"global_8009A204\":\"0x%08X\",\"global_ptr_val\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[2],
                    gpr_[4],
                    gpr_[5],
                    gpr_[6],
                    gpr_[7],
                    gpr_[29],
                    gpr_[31],
                    gpr_[14],
                    gpr_[15],
                    gpr_[24],
                    gpr_[25],
                    gpr_[1],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    bus_.irq_pending_masked(),
                    gptr,
                    gval
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nv0=0x%08X\na0=0x%08X\na1=0x%08X\na2=0x%08X\na3=0x%08X\nsp=0x%08X\nra=0x%08X\n", pc_, gpr_[2], gpr_[4], gpr_[5], gpr_[6], gpr_[7], gpr_[29], gpr_[31]);
            std::fprintf(compare_file_, "t6=0x%08X\nt7=0x%08X\nt8=0x%08X\nt9=0x%08X\nat=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\npend=0x%08X\n", gpr_[14], gpr_[15], gpr_[24], gpr_[25], gpr_[1], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE], bus_.irq_pending_masked());
            std::fprintf(compare_file_, "global_8009A204=0x%08X\nglobal_ptr_val=0x%08X\n", gptr, gval);
            for (int i = 0; i < 32; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // Debug bring-up (sans patch): dump une fois quand on tombe dans la boucle suivante observée.
    // Objectif: identifier quel périphérique/IRQ/flag manque avant l'accès CDROM.
    if (!dbg_ef30_dumped_ && sys_has_clock_ && pc_ == 0x8005EF30u)
    {
        dbg_ef30_dumped_ = 1;

        uint32_t ins[16]{};
        for (uint32_t i = 0; i < 16; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        auto mmio_peek32 = [&](uint32_t phys, uint32_t& out) -> int {
            Bus::MemFault f{};
            return bus_.read_u32(phys, out, f) ? 1 : 0;
        };

        uint32_t i_stat = 0, i_mask = 0;
        (void)mmio_peek32(0x1F801070u, i_stat);
        (void)mmio_peek32(0x1F801074u, i_mask);

        // DMA2 (GPU) regs
        uint32_t dma2_madr = 0, dma2_bcr = 0, dma2_chcr = 0;
        uint32_t dpcr = 0, dicr = 0;
        (void)mmio_peek32(0x1F8010A0u, dma2_madr);
        (void)mmio_peek32(0x1F8010A4u, dma2_bcr);
        (void)mmio_peek32(0x1F8010A8u, dma2_chcr);
        (void)mmio_peek32(0x1F8010F0u, dpcr);
        (void)mmio_peek32(0x1F8010F4u, dicr);

        // Timers snapshot
        uint32_t t0c = 0, t0m = 0, t0t = 0;
        uint32_t t1c = 0, t1m = 0, t1t = 0;
        uint32_t t2c = 0, t2m = 0, t2t = 0;
        (void)mmio_peek32(0x1F801100u, t0c);
        (void)mmio_peek32(0x1F801104u, t0m);
        (void)mmio_peek32(0x1F801108u, t0t);
        (void)mmio_peek32(0x1F801110u, t1c);
        (void)mmio_peek32(0x1F801114u, t1m);
        (void)mmio_peek32(0x1F801118u, t1t);
        (void)mmio_peek32(0x1F801120u, t2c);
        (void)mmio_peek32(0x1F801124u, t2m);
        (void)mmio_peek32(0x1F801128u, t2t);

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 PC=0x%08X sp=0x%08X ra=0x%08X gp=0x%08X status=0x%08X cause=0x%08X",
            pc_,
            gpr_[29],
            gpr_[31],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 IRQ: I_STAT=0x%08X I_MASK=0x%08X pend=0x%08X",
            i_stat,
            i_mask,
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 DMA2: MADR=0x%08X BCR=0x%08X CHCR=0x%08X DPCR=0x%08X DICR=0x%08X",
            dma2_madr,
            dma2_bcr,
            dma2_chcr,
            dpcr,
            dicr
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 TMR: t0(c=%08X m=%08X t=%08X) t1(c=%08X m=%08X t=%08X) t2(c=%08X m=%08X t=%08X)",
            t0c,
            t0m,
            t0t,
            t1c,
            t1m,
            t1t,
            t2c,
            t2m,
            t2t
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop2 ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        // #region agent log H2
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H2_TIMERS_OR_DMA\",\"location\":\"cpu.cpp:dbg_loop2_8005EF30\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\",\"pend\":\"0x%08X\",\"dma2_madr\":\"0x%08X\",\"dma2_bcr\":\"0x%08X\",\"dma2_chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\",\"t0c\":\"0x%08X\",\"t0m\":\"0x%08X\",\"t0t\":\"0x%08X\",\"t1c\":\"0x%08X\",\"t1m\":\"0x%08X\",\"t1t\":\"0x%08X\",\"t2c\":\"0x%08X\",\"t2m\":\"0x%08X\",\"t2t\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[29],
                    gpr_[31],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    i_stat,
                    i_mask,
                    bus_.irq_pending_masked(),
                    dma2_madr,
                    dma2_bcr,
                    dma2_chcr,
                    dpcr,
                    dicr,
                    t0c,
                    t0m,
                    t0t,
                    t1c,
                    t1m,
                    t1t,
                    t2c,
                    t2m,
                    t2t
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nsp=0x%08X\nra=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\n", pc_, gpr_[29], gpr_[31], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE]);
            std::fprintf(compare_file_, "i_stat=0x%08X\ni_mask=0x%08X\npend=0x%08X\n", i_stat, i_mask, bus_.irq_pending_masked());
            std::fprintf(compare_file_, "dma2_madr=0x%08X\ndma2_bcr=0x%08X\ndma2_chcr=0x%08X\ndpcr=0x%08X\ndicr=0x%08X\n", dma2_madr, dma2_bcr, dma2_chcr, dpcr, dicr);
            std::fprintf(compare_file_, "t0_count=0x%08X\nt0_mode=0x%08X\nt0_target=0x%08X\nt1_count=0x%08X\nt1_mode=0x%08X\nt1_target=0x%08X\nt2_count=0x%08X\nt2_mode=0x%08X\nt2_target=0x%08X\n", t0c, t0m, t0t, t1c, t1m, t1t, t2c, t2m, t2t);
            for (int i = 0; i < 16; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // Debug bring-up (sans patch): nouvelle boucle observée après avoir implémenté DMA2 request/manual.
    // Objectif: identifier le registre/bit MMIO exact que le BIOS attend (souvent GPUSTAT / IRQ / timer).
    if (!dbg_de24_dumped_ && sys_has_clock_ && pc_ == 0x8005DE24u)
    {
        dbg_de24_dumped_ = 1;

        uint32_t ins[32]{};
        for (uint32_t i = 0; i < 32; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        auto mmio_peek32 = [&](uint32_t phys, uint32_t& out) -> int {
            Bus::MemFault f{};
            return bus_.read_u32(phys, out, f) ? 1 : 0;
        };

        const uint32_t t7 = gpr_[15];
        const uint32_t t8 = gpr_[24];
        const uint32_t t7_phys = virt_to_phys(t7) & ~3u;
        uint32_t t7_val = 0;
        (void)mmio_peek32(t7_phys, t7_val);

        uint32_t i_stat = 0, i_mask = 0;
        (void)mmio_peek32(0x1F801070u, i_stat);
        (void)mmio_peek32(0x1F801074u, i_mask);

        uint32_t gpustat = 0, gpuread = 0;
        (void)mmio_peek32(0x1F801814u, gpustat);
        (void)mmio_peek32(0x1F801810u, gpuread);

        uint32_t dma2_madr = 0, dma2_bcr = 0, dma2_chcr = 0;
        uint32_t dpcr = 0, dicr = 0;
        (void)mmio_peek32(0x1F8010A0u, dma2_madr);
        (void)mmio_peek32(0x1F8010A4u, dma2_bcr);
        (void)mmio_peek32(0x1F8010A8u, dma2_chcr);
        (void)mmio_peek32(0x1F8010F0u, dpcr);
        (void)mmio_peek32(0x1F8010F4u, dicr);

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 PC=0x%08X sp=0x%08X ra=0x%08X gp=0x%08X status=0x%08X cause=0x%08X",
            pc_,
            gpr_[29],
            gpr_[31],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 T7=0x%08X (phys=0x%08X) [T7]=0x%08X T8=0x%08X",
            t7,
            t7_phys,
            t7_val,
            t8
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 IRQ: I_STAT=0x%08X I_MASK=0x%08X pend=0x%08X",
            i_stat,
            i_mask,
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 GPU: GPUSTAT=0x%08X GPUREAD=0x%08X",
            gpustat,
            gpuread
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 DMA2: MADR=0x%08X BCR=0x%08X CHCR=0x%08X DPCR=0x%08X DICR=0x%08X",
            dma2_madr,
            dma2_bcr,
            dma2_chcr,
            dpcr,
            dicr
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 ins16-23: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[16],
            ins[17],
            ins[18],
            ins[19],
            ins[20],
            ins[21],
            ins[22],
            ins[23]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop3 ins24-31: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[24],
            ins[25],
            ins[26],
            ins[27],
            ins[28],
            ins[29],
            ins[30],
            ins[31]
        );
        // #region agent log H3
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H3_GPUSTAT_OR_DMA\",\"location\":\"cpu.cpp:dbg_loop3_8005DE24\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"t7\":\"0x%08X\",\"t7_phys\":\"0x%08X\",\"t7_val\":\"0x%08X\",\"t8\":\"0x%08X\",\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\",\"pend\":\"0x%08X\",\"gpustat\":\"0x%08X\",\"gpuread\":\"0x%08X\",\"dma2_madr\":\"0x%08X\",\"dma2_bcr\":\"0x%08X\",\"dma2_chcr\":\"0x%08X\",\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[29],
                    gpr_[31],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    t7,
                    t7_phys,
                    t7_val,
                    t8,
                    i_stat,
                    i_mask,
                    bus_.irq_pending_masked(),
                    gpustat,
                    gpuread,
                    dma2_madr,
                    dma2_bcr,
                    dma2_chcr,
                    dpcr,
                    dicr
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nsp=0x%08X\nra=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\n", pc_, gpr_[29], gpr_[31], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE]);
            std::fprintf(compare_file_, "t7=0x%08X\nt7_phys=0x%08X\nt7_val=0x%08X\nt8=0x%08X\n", t7, t7_phys, t7_val, t8);
            std::fprintf(compare_file_, "i_stat=0x%08X\ni_mask=0x%08X\npend=0x%08X\n", i_stat, i_mask, bus_.irq_pending_masked());
            std::fprintf(compare_file_, "gpustat=0x%08X\ngpuread=0x%08X\n", gpustat, gpuread);
            std::fprintf(compare_file_, "dma2_madr=0x%08X\ndma2_bcr=0x%08X\ndma2_chcr=0x%08X\ndpcr=0x%08X\ndicr=0x%08X\n", dma2_madr, dma2_bcr, dma2_chcr, dpcr, dicr);
            for (int i = 0; i < 32; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // Debug bring-up (sans patch): nouvelle boucle observée après DMA6 OTC.
    // Objectif: comprendre ce que le BIOS attend (MMIO/IRQ/etc).
    if (!dbg_e520_dumped_ && sys_has_clock_ && pc_ == 0x8005E520u)
    {
        dbg_e520_dumped_ = 1;

        uint32_t ins[32]{};
        for (uint32_t i = 0; i < 32; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        auto mmio_peek32 = [&](uint32_t phys, uint32_t& out) -> int {
            Bus::MemFault f{};
            return bus_.read_u32(phys, out, f) ? 1 : 0;
        };

        uint32_t i_stat = 0, i_mask = 0;
        (void)mmio_peek32(0x1F801070u, i_stat);
        (void)mmio_peek32(0x1F801074u, i_mask);

        uint32_t gpustat = 0;
        (void)mmio_peek32(0x1F801814u, gpustat);

        uint32_t dpcr = 0, dicr = 0;
        (void)mmio_peek32(0x1F8010F0u, dpcr);
        (void)mmio_peek32(0x1F8010F4u, dicr);

        // DMA2 + DMA6 snapshot
        uint32_t dma2_madr = 0, dma2_bcr = 0, dma2_chcr = 0;
        uint32_t dma6_madr = 0, dma6_bcr = 0, dma6_chcr = 0;
        (void)mmio_peek32(0x1F8010A0u, dma2_madr);
        (void)mmio_peek32(0x1F8010A4u, dma2_bcr);
        (void)mmio_peek32(0x1F8010A8u, dma2_chcr);
        (void)mmio_peek32(0x1F8010E0u, dma6_madr);
        (void)mmio_peek32(0x1F8010E4u, dma6_bcr);
        (void)mmio_peek32(0x1F8010E8u, dma6_chcr);

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 PC=0x%08X sp=0x%08X ra=0x%08X gp=0x%08X status=0x%08X cause=0x%08X "
            "v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X t6=0x%08X t7=0x%08X",
            pc_,
            gpr_[29],
            gpr_[31],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE],
            gpr_[2],
            gpr_[4],
            gpr_[5],
            gpr_[6],
            gpr_[7],
            gpr_[14],
            gpr_[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 IRQ: I_STAT=0x%08X I_MASK=0x%08X pend=0x%08X",
            i_stat,
            i_mask,
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 GPU: GPUSTAT=0x%08X",
            gpustat
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 DMA: DPCR=0x%08X DICR=0x%08X DMA2(chcr=%08X bcr=%08X madr=%08X) DMA6(chcr=%08X bcr=%08X madr=%08X)",
            dpcr,
            dicr,
            dma2_chcr,
            dma2_bcr,
            dma2_madr,
            dma6_chcr,
            dma6_bcr,
            dma6_madr
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 ins16-23: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[16],
            ins[17],
            ins[18],
            ins[19],
            ins[20],
            ins[21],
            ins[22],
            ins[23]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop4 ins24-31: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[24],
            ins[25],
            ins[26],
            ins[27],
            ins[28],
            ins[29],
            ins[30],
            ins[31]
        );
        // #region agent log H4
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H4_DMA_IRQ\",\"location\":\"cpu.cpp:dbg_loop4_8005E520\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"v0\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"t6\":\"0x%08X\",\"t7\":\"0x%08X\",\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\",\"pend\":\"0x%08X\",\"gpustat\":\"0x%08X\",\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\",\"dma2_madr\":\"0x%08X\",\"dma2_bcr\":\"0x%08X\",\"dma2_chcr\":\"0x%08X\",\"dma6_madr\":\"0x%08X\",\"dma6_bcr\":\"0x%08X\",\"dma6_chcr\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[29],
                    gpr_[31],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    gpr_[2],
                    gpr_[4],
                    gpr_[5],
                    gpr_[6],
                    gpr_[7],
                    gpr_[14],
                    gpr_[15],
                    i_stat,
                    i_mask,
                    bus_.irq_pending_masked(),
                    gpustat,
                    dpcr,
                    dicr,
                    dma2_madr,
                    dma2_bcr,
                    dma2_chcr,
                    dma6_madr,
                    dma6_bcr,
                    dma6_chcr
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nsp=0x%08X\nra=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\n", pc_, gpr_[29], gpr_[31], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE]);
            std::fprintf(compare_file_, "v0=0x%08X\na0=0x%08X\na1=0x%08X\na2=0x%08X\na3=0x%08X\nt6=0x%08X\nt7=0x%08X\n", gpr_[2], gpr_[4], gpr_[5], gpr_[6], gpr_[7], gpr_[14], gpr_[15]);
            std::fprintf(compare_file_, "i_stat=0x%08X\ni_mask=0x%08X\npend=0x%08X\n", i_stat, i_mask, bus_.irq_pending_masked());
            std::fprintf(compare_file_, "gpustat=0x%08X\ndpcr=0x%08X\ndicr=0x%08X\n", gpustat, dpcr, dicr);
            std::fprintf(compare_file_, "dma2_madr=0x%08X\ndma2_bcr=0x%08X\ndma2_chcr=0x%08X\ndma6_madr=0x%08X\ndma6_bcr=0x%08X\ndma6_chcr=0x%08X\n", dma2_madr, dma2_bcr, dma2_chcr, dma6_madr, dma6_bcr, dma6_chcr);
            for (int i = 0; i < 32; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // Debug bring-up (sans patch): boucle observée après avoir débloqué GPUSTAT bit27.
    // Objectif: comprendre quel périphérique/flag manque avant l'accès CDROM.
    if (!dbg_6797c_dumped_ && sys_has_clock_ && pc_ == 0x8006797Cu)
    {
        dbg_6797c_dumped_ = 1;

        uint32_t ins[32]{};
        for (uint32_t i = 0; i < 32; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        auto mmio_peek32 = [&](uint32_t phys, uint32_t& out) -> int {
            Bus::MemFault f{};
            return bus_.read_u32(phys, out, f) ? 1 : 0;
        };

        uint32_t i_stat = 0, i_mask = 0;
        (void)mmio_peek32(0x1F801070u, i_stat);
        (void)mmio_peek32(0x1F801074u, i_mask);

        uint32_t gpustat = 0;
        (void)mmio_peek32(0x1F801814u, gpustat);

        uint32_t dpcr = 0, dicr = 0;
        (void)mmio_peek32(0x1F8010F0u, dpcr);
        (void)mmio_peek32(0x1F8010F4u, dicr);

        uint32_t t0c = 0, t0m = 0, t0t = 0;
        uint32_t t1c = 0, t1m = 0, t1t = 0;
        uint32_t t2c = 0, t2m = 0, t2t = 0;
        (void)mmio_peek32(0x1F801100u, t0c);
        (void)mmio_peek32(0x1F801104u, t0m);
        (void)mmio_peek32(0x1F801108u, t0t);
        (void)mmio_peek32(0x1F801110u, t1c);
        (void)mmio_peek32(0x1F801114u, t1m);
        (void)mmio_peek32(0x1F801118u, t1t);
        (void)mmio_peek32(0x1F801120u, t2c);
        (void)mmio_peek32(0x1F801124u, t2m);
        (void)mmio_peek32(0x1F801128u, t2t);

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 PC=0x%08X sp=0x%08X ra=0x%08X gp=0x%08X status=0x%08X cause=0x%08X "
            "v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X t0=0x%08X t1=0x%08X t2=0x%08X",
            pc_,
            gpr_[29],
            gpr_[31],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE],
            gpr_[2],
            gpr_[4],
            gpr_[5],
            gpr_[6],
            gpr_[7],
            gpr_[8],
            gpr_[9],
            gpr_[10]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 IRQ: I_STAT=0x%08X I_MASK=0x%08X pend=0x%08X",
            i_stat,
            i_mask,
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 GPUSTAT=0x%08X DPCR=0x%08X DICR=0x%08X",
            gpustat,
            dpcr,
            dicr
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 TMR: t0(c=%08X m=%08X t=%08X) t1(c=%08X m=%08X t=%08X) t2(c=%08X m=%08X t=%08X)",
            t0c,
            t0m,
            t0t,
            t1c,
            t1m,
            t1t,
            t2c,
            t2m,
            t2t
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 ins16-23: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[16],
            ins[17],
            ins[18],
            ins[19],
            ins[20],
            ins[21],
            ins[22],
            ins[23]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop5 ins24-31: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[24],
            ins[25],
            ins[26],
            ins[27],
            ins[28],
            ins[29],
            ins[30],
            ins[31]
        );
        // #region agent log H5
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H5_IRQ_TIMER_GPUSTAT\",\"location\":\"cpu.cpp:dbg_loop5_8006797C\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"v0\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"t0\":\"0x%08X\",\"t1\":\"0x%08X\",\"t2\":\"0x%08X\",\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\",\"pend\":\"0x%08X\",\"gpustat\":\"0x%08X\",\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\",\"t0c\":\"0x%08X\",\"t0m\":\"0x%08X\",\"t0t\":\"0x%08X\",\"t1c\":\"0x%08X\",\"t1m\":\"0x%08X\",\"t1t\":\"0x%08X\",\"t2c\":\"0x%08X\",\"t2m\":\"0x%08X\",\"t2t\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[29],
                    gpr_[31],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    gpr_[2],
                    gpr_[4],
                    gpr_[5],
                    gpr_[6],
                    gpr_[7],
                    gpr_[8],
                    gpr_[9],
                    gpr_[10],
                    i_stat,
                    i_mask,
                    bus_.irq_pending_masked(),
                    gpustat,
                    dpcr,
                    dicr,
                    t0c,
                    t0m,
                    t0t,
                    t1c,
                    t1m,
                    t1t,
                    t2c,
                    t2m,
                    t2t
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nsp=0x%08X\nra=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\n", pc_, gpr_[29], gpr_[31], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE]);
            std::fprintf(compare_file_, "v0=0x%08X\na0=0x%08X\na1=0x%08X\na2=0x%08X\na3=0x%08X\nt0=0x%08X\nt1=0x%08X\nt2=0x%08X\n", gpr_[2], gpr_[4], gpr_[5], gpr_[6], gpr_[7], gpr_[8], gpr_[9], gpr_[10]);
            std::fprintf(compare_file_, "i_stat=0x%08X\ni_mask=0x%08X\npend=0x%08X\n", i_stat, i_mask, bus_.irq_pending_masked());
            std::fprintf(compare_file_, "gpustat=0x%08X\ndpcr=0x%08X\ndicr=0x%08X\n", gpustat, dpcr, dicr);
            std::fprintf(compare_file_, "t0_count=0x%08X\nt0_mode=0x%08X\nt0_target=0x%08X\nt1_count=0x%08X\nt1_mode=0x%08X\nt1_target=0x%08X\nt2_count=0x%08X\nt2_mode=0x%08X\nt2_target=0x%08X\n", t0c, t0m, t0t, t1c, t1m, t1t, t2c, t2m, t2t);
            for (int i = 0; i < 32; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // Même boucle, mais dump au début du "hot path" observé (PC=0x80067938 dans les samples).
    if (!dbg_67938_dumped_ && sys_has_clock_ && pc_ == 0x80067938u)
    {
        dbg_67938_dumped_ = 1;

        uint32_t ins[32]{};
        for (uint32_t i = 0; i < 32; ++i)
        {
            Bus::MemFault f{};
            const uint32_t phys = virt_to_phys(pc_ + i * 4u);
            (void)bus_.read_u32(phys, ins[i], f);
        }

        auto mmio_peek32 = [&](uint32_t phys, uint32_t& out) -> int {
            Bus::MemFault f{};
            return bus_.read_u32(phys, out, f) ? 1 : 0;
        };

        uint32_t i_stat = 0, i_mask = 0;
        (void)mmio_peek32(0x1F801070u, i_stat);
        (void)mmio_peek32(0x1F801074u, i_mask);

        uint32_t gpustat = 0;
        (void)mmio_peek32(0x1F801814u, gpustat);

        uint32_t dpcr = 0, dicr = 0;
        (void)mmio_peek32(0x1F8010F0u, dpcr);
        (void)mmio_peek32(0x1F8010F4u, dicr);

        // Peek a few likely MMIO words in this region (DMA / GPU / CDROM / IRQ / SPU / PAD).
        uint32_t mmio_10f8 = 0, mmio_10fc = 0, mmio_1d80 = 0, mmio_1d84 = 0;
        (void)mmio_peek32(0x1F8010F8u, mmio_10f8);
        (void)mmio_peek32(0x1F8010FCu, mmio_10fc);
        (void)mmio_peek32(0x1F801D80u, mmio_1d80);
        (void)mmio_peek32(0x1F801D84u, mmio_1d84);

        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 PC=0x%08X sp=0x%08X ra=0x%08X gp=0x%08X status=0x%08X cause=0x%08X "
            "v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X t0=0x%08X t1=0x%08X t2=0x%08X",
            pc_,
            gpr_[29],
            gpr_[31],
            gpr_[28],
            cop0_[COP0_STATUS],
            cop0_[COP0_CAUSE],
            gpr_[2],
            gpr_[4],
            gpr_[5],
            gpr_[6],
            gpr_[7],
            gpr_[8],
            gpr_[9],
            gpr_[10]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 IRQ: I_STAT=0x%08X I_MASK=0x%08X pend=0x%08X",
            i_stat,
            i_mask,
            bus_.irq_pending_masked()
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 GPUSTAT=0x%08X DPCR=0x%08X DICR=0x%08X MMIO10F8=%08X MMIO10FC=%08X MMIO1D80=%08X MMIO1D84=%08X",
            gpustat,
            dpcr,
            dicr,
            mmio_10f8,
            mmio_10fc,
            mmio_1d80,
            mmio_1d84
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 ins0-7 : %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[0],
            ins[1],
            ins[2],
            ins[3],
            ins[4],
            ins[5],
            ins[6],
            ins[7]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 ins8-15: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[8],
            ins[9],
            ins[10],
            ins[11],
            ins[12],
            ins[13],
            ins[14],
            ins[15]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 ins16-23: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[16],
            ins[17],
            ins[18],
            ins[19],
            ins[20],
            ins[21],
            ins[22],
            ins[23]
        );
        flog::logf(
            sys_io_,
            sys_clock_,
            flog::Level::info,
            "CPU",
            "DBG loop6 ins24-31: %08X %08X %08X %08X %08X %08X %08X %08X",
            ins[24],
            ins[25],
            ins[26],
            ins[27],
            ins[28],
            ins[29],
            ins[30],
            ins[31]
        );
        // #region agent log H3
        {
            std::FILE* f = nullptr;
            agent_log_json_begin(f);
            if (f)
            {
                const uint64_t ts = agent_now_ms();
                std::fprintf(
                    f,
                    "{\"timestamp\":%" PRIu64 ",\"sessionId\":\"debug-session\",\"runId\":\"pre-fix\",\"hypothesisId\":\"H3_GPUSTAT_OR_MMIO\",\"location\":\"cpu.cpp:dbg_loop6_80067938\",\"message\":\"snapshot\",\"data\":{\"pc\":\"0x%08X\",\"sp\":\"0x%08X\",\"ra\":\"0x%08X\",\"gp\":\"0x%08X\",\"status\":\"0x%08X\",\"cause\":\"0x%08X\",\"v0\":\"0x%08X\",\"a0\":\"0x%08X\",\"a1\":\"0x%08X\",\"a2\":\"0x%08X\",\"a3\":\"0x%08X\",\"t0\":\"0x%08X\",\"t1\":\"0x%08X\",\"t2\":\"0x%08X\",\"i_stat\":\"0x%08X\",\"i_mask\":\"0x%08X\",\"pend\":\"0x%08X\",\"gpustat\":\"0x%08X\",\"dpcr\":\"0x%08X\",\"dicr\":\"0x%08X\",\"mmio_10f8\":\"0x%08X\",\"mmio_10fc\":\"0x%08X\",\"mmio_1d80\":\"0x%08X\",\"mmio_1d84\":\"0x%08X\"}}\n",
                    ts,
                    pc_,
                    gpr_[29],
                    gpr_[31],
                    gpr_[28],
                    cop0_[COP0_STATUS],
                    cop0_[COP0_CAUSE],
                    gpr_[2],
                    gpr_[4],
                    gpr_[5],
                    gpr_[6],
                    gpr_[7],
                    gpr_[8],
                    gpr_[9],
                    gpr_[10],
                    i_stat,
                    i_mask,
                    bus_.irq_pending_masked(),
                    gpustat,
                    dpcr,
                    dicr,
                    mmio_10f8,
                    mmio_10fc,
                    mmio_1d80,
                    mmio_1d84
                );
            }
            agent_log_json_end(f);
        }
        // #endregion
        if (compare_file_)
        {
            std::fprintf(compare_file_, "[PC=0x%08X]\n", pc_);
            std::fprintf(compare_file_, "pc=0x%08X\nsp=0x%08X\nra=0x%08X\ngp=0x%08X\nstatus=0x%08X\ncause=0x%08X\n", pc_, gpr_[29], gpr_[31], gpr_[28], cop0_[COP0_STATUS], cop0_[COP0_CAUSE]);
            std::fprintf(compare_file_, "v0=0x%08X\na0=0x%08X\na1=0x%08X\na2=0x%08X\na3=0x%08X\nt0=0x%08X\nt1=0x%08X\nt2=0x%08X\n", gpr_[2], gpr_[4], gpr_[5], gpr_[6], gpr_[7], gpr_[8], gpr_[9], gpr_[10]);
            std::fprintf(compare_file_, "i_stat=0x%08X\ni_mask=0x%08X\npend=0x%08X\n", i_stat, i_mask, bus_.irq_pending_masked());
            std::fprintf(compare_file_, "gpustat=0x%08X\ndpcr=0x%08X\ndicr=0x%08X\nmmio_10f8=0x%08X\nmmio_10fc=0x%08X\nmmio_1d80=0x%08X\nmmio_1d84=0x%08X\n", gpustat, dpcr, dicr, mmio_10f8, mmio_10fc, mmio_1d80, mmio_1d84);
            for (int i = 0; i < 32; ++i)
                std::fprintf(compare_file_, "ins%d=0x%08X\n", i, ins[i]);
            std::fprintf(compare_file_, "\n");
        }
    }

    // -----------------------------
    // 0) IRQ (PS1) - check between instructions
    // -----------------------------
    // Sur PS1, l'IRQ controller (I_STAT/I_MASK) drive une ligne d'interruption R3000.
    // Modèle minimal:
    // - on mappe (I_STAT & I_MASK) -> COP0.Cause.IP2 (bit10)
    // - si Status.IEc=1 et Status.IM2=1, on prend une exception EXC_INT.
    {
        const uint32_t pending = bus_.irq_pending_masked();
        uint32_t cause = cop0_[COP0_CAUSE];
        if (pending)
            cause |= (1u << 10); // IP2
        else
            cause &= ~(1u << 10);
        cop0_[COP0_CAUSE] = cause;

        const uint32_t status = cop0_[COP0_STATUS];
        const uint32_t ip = cause & 0xFF00u;
        const uint32_t im = status & 0xFF00u;
        const int iec = (status & 0x1u) ? 1 : 0;

        // Periodic debug: log interrupt state every 10M cycles
        {
            static uint64_t irq_sample_step = 0;
            if (++irq_sample_step % 10000000 == 0)
            {
                const uint32_t i_stat = bus_.irq_stat_raw();
                const uint32_t i_mask = bus_.irq_mask_raw();
                std::fprintf(stderr, "[CPU] IRQ state sample: step=%" PRIu64 " PC=0x%08X i_stat=0x%08X i_mask=0x%08X pending=0x%08X status=0x%08X iec=%d im=0x%04X ip=0x%04X\n",
                    irq_sample_step, pc_, i_stat, i_mask, pending, status, iec, (im >> 8), (ip >> 8));
                std::fflush(stderr);
            }
        }

        if (iec && (ip & im) != 0u)
        {
            // Debug: log interrupt exceptions
            static int irq_exc_count = 0;
            if (++irq_exc_count <= 50 || (irq_exc_count % 100 == 0 && irq_exc_count <= 500))
            {
                std::fprintf(stderr, "[CPU] INT exception #%d: PC=0x%08X pending=0x%08X status=0x%08X cause=0x%08X\n",
                    irq_exc_count, pc_, pending, status, cause);
                std::fflush(stderr);
            }
            raise_exception(EXC_INT, 0, pc_);
            r.kind = StepResult::Kind::ok;
            r.instr = 0;
            return r;
        }
    }

    // HLE (bring-up) : vecteurs BIOS A0/B0/C0.
    //
    // Le BIOS (et les jeux) appellent des fonctions Kernel via:
    //   - PC=0xA0 / 0xB0 / 0xC0
    //   - numéro de fonction dans t1 (r9), ex: A(45h), B(18h), C(07h)...
    //
    // Sur vrai hardware, le BIOS installe de petits stubs en RAM à ces adresses.
    // Dans notre émulateur, la RAM est initialisée à 0 => sans HLE, ces appels tombent sur NOP.
    //
    // Ici on HLE une petite partie des fonctions les plus utiles pour le boot, en restant lisible.
    //
    // IMPORTANT "c'est le BIOS qui décide":
    // On ne doit HLE ces vecteurs QUE tant qu'ils sont vides (RAM=0). Dès que le BIOS installe
    // de vrais stubs à 0xA0/0xB0/0xC0, on doit exécuter le code RAM normal, sinon on bloque des
    // init (dont le boot CD).
    int hle_vec_gate = 0;
    if (hle_vectors_ && (pc_ == 0x0000'00A0u || pc_ == 0x0000'00B0u || pc_ == 0x0000'00C0u))
    {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        Bus::MemFault f{};
        const uint32_t p0 = pc_ + 0u;
        const uint32_t p1 = pc_ + 4u;
        const int ok0 = bus_.read_u32(p0, w0, f) ? 1 : 0;
        const int ok1 = bus_.read_u32(p1, w1, f) ? 1 : 0;
        const int empty = (ok0 && ok1 && w0 == 0 && w1 == 0) ? 1 : 0;
        hle_vec_gate = empty;

        // Debug: trace BIOS vector calls
        static int vec_call_count = 0;
        ++vec_call_count;
        const uint32_t fn_dbg = gpr_[9] & 0xFFu; // t1
        const char* vec_name = (pc_ == 0xA0u) ? "A0" : (pc_ == 0xB0u) ? "B0" : "C0";
        if (vec_call_count <= 100 || (vec_call_count % 10000 == 0))
        {
            std::fprintf(stderr, "[CPU] %s(0x%02X) call #%d: ra=0x%08X a0=0x%08X a1=0x%08X hle=%s stub=[0x%08X,0x%08X]\n",
                vec_name, fn_dbg, vec_call_count, gpr_[31], gpr_[4], gpr_[5],
                empty ? "yes" : "no(BIOS)", w0, w1);
            std::fflush(stderr);
        }
    }

    if (hle_vec_gate)
    {
        const uint32_t fn = gpr_[9] & 0xFFu; // t1
        const uint32_t a0 = gpr_[4];
        const uint32_t a1 = gpr_[5];
        const uint32_t a2 = gpr_[6];
        const uint32_t a3 = gpr_[7];

        auto read_u8_guest = [&](uint32_t vaddr, uint8_t& out) -> int
        {
            Bus::MemFault f{};
            const uint32_t paddr = virt_to_phys(vaddr);
            return bus_.read_u8(paddr, out, f) ? 1 : 0;
        };
        auto write_u8_guest = [&](uint32_t vaddr, uint8_t v) -> int
        {
            Bus::MemFault f{};
            const uint32_t paddr = virt_to_phys(vaddr);
            return bus_.write_u8(paddr, v, f) ? 1 : 0;
        };
        auto write_u32_guest = [&](uint32_t vaddr, uint32_t v) -> int
        {
            // Little-endian write.
            return write_u8_guest(vaddr + 0, (uint8_t)(v & 0xFFu)) && write_u8_guest(vaddr + 1, (uint8_t)((v >> 8) & 0xFFu)) &&
                   write_u8_guest(vaddr + 2, (uint8_t)((v >> 16) & 0xFFu)) && write_u8_guest(vaddr + 3, (uint8_t)((v >> 24) & 0xFFu));
        };
        auto read_cstr_guest = [&](uint32_t vaddr, char* dst, uint32_t cap) -> uint32_t
        {
            if (!dst || cap == 0)
                return 0;
            uint32_t n = 0;
            while (n + 1 < cap)
            {
                uint8_t ch = 0;
                if (!read_u8_guest(vaddr + n, ch))
                    break;
                dst[n++] = (char)ch;
                if (ch == 0)
                    break;
            }
            dst[(n < cap) ? n : (cap - 1)] = 0;
            return n;
        };

        // MMIO helpers (phys addresses).
        auto mmio_read_u16 = [&](uint32_t phys_addr, uint16_t& out) -> int
        {
            Bus::MemFault f{};
            return bus_.read_u16(phys_addr, out, f) ? 1 : 0;
        };
        auto mmio_write_u16 = [&](uint32_t phys_addr, uint16_t v) -> int
        {
            Bus::MemFault f{};
            return bus_.write_u16(phys_addr, v, f) ? 1 : 0;
        };
        auto mmio_read_u32 = [&](uint32_t phys_addr, uint32_t& out) -> int
        {
            Bus::MemFault f{};
            return bus_.read_u32(phys_addr, out, f) ? 1 : 0;
        };
        auto mmio_write_u32 = [&](uint32_t phys_addr, uint32_t v) -> int
        {
            Bus::MemFault f{};
            return bus_.write_u32(phys_addr, v, f) ? 1 : 0;
        };

        auto hle_memcpy = [&](uint32_t dst, uint32_t src, uint32_t len) -> int
        {
            // Très lent (byte-by-byte), mais suffisant pour bring-up.
            for (uint32_t i = 0; i < len; ++i)
            {
                uint8_t b = 0;
                if (!read_u8_guest(src + i, b))
                    return 0;
                if (!write_u8_guest(dst + i, b))
                    return 0;
            }
            return 1;
        };
        auto hle_memset = [&](uint32_t dst, uint8_t fill, uint32_t len) -> int
        {
            for (uint32_t i = 0; i < len; ++i)
            {
                if (!write_u8_guest(dst + i, fill))
                    return 0;
            }
            return 1;
        };
        auto hle_strlen = [&](uint32_t src, uint32_t max_scan) -> uint32_t
        {
            uint32_t n = 0;
            for (; n < max_scan; ++n)
            {
                uint8_t ch = 0;
                if (!read_u8_guest(src + n, ch))
                    break;
                if (ch == 0)
                    break;
            }
            return n;
        };

        // -----------------------------
        // HLE File I/O (cdrom:) minimal
        // -----------------------------
        auto hle_set_last_err = [&](uint32_t e) -> void
        {
            hle_last_error_ = e;
        };

        auto hle_alloc_fd = [&]() -> int
        {
            // PSX: 0..15, mais 0/1 sont souvent stdio réservés.
            for (int fd = 2; fd < 16; ++fd)
            {
                if (!hle_files_[fd].used)
                {
                    hle_files_[fd].used = 1;
                    hle_files_[fd].lba = 0;
                    hle_files_[fd].size = 0;
                    hle_files_[fd].pos = 0;
                    return fd;
                }
            }
            return -1;
        };

        auto hle_free_fd = [&](int fd) -> void
        {
            if (fd < 0 || fd >= 16)
                return;
            hle_files_[fd].used = 0;
            hle_files_[fd].lba = 0;
            hle_files_[fd].size = 0;
            hle_files_[fd].pos = 0;
        };

        auto hle_write_guest = [&](uint32_t dst, const uint8_t* src, uint32_t n) -> int
        {
            if (!src || n == 0)
                return 1;
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!write_u8_guest(dst + i, src[i]))
                    return 0;
            }
            return 1;
        };

        auto hle_file_open = [&](uint32_t filename_ptr, uint32_t accessmode) -> int
        {
            (void)accessmode;
            cdrom::Cdrom* cd = bus_.cdrom();
            if (!cd)
            {
                hle_set_last_err(0x13u); // unknown device
                return -1;
            }

            char name[256];
            read_cstr_guest(filename_ptr, name, (uint32_t)sizeof(name));

            uint32_t lba = 0;
            uint32_t size = 0;
            if (!cd->iso9660_find_file(name, &lba, &size))
            {
                hle_set_last_err(0x02u); // file not found
                return -1;
            }

            const int fd = hle_alloc_fd();
            if (fd < 0)
            {
                hle_set_last_err(0x18u); // not enough handles
                return -1;
            }

            hle_files_[fd].lba = lba;
            hle_files_[fd].size = size;
            hle_files_[fd].pos = 0;
            hle_set_last_err(0);
            return fd;
        };

        auto hle_file_seek = [&](int fd, int32_t offset, uint32_t seektype) -> int32_t
        {
            if (fd < 0 || fd >= 16 || !hle_files_[fd].used)
            {
                hle_set_last_err(0x09u); // invalid handle
                return -1;
            }

            int64_t base = 0;
            if (seektype == 0u)
                base = 0;
            else if (seektype == 1u)
                base = (int64_t)hle_files_[fd].pos;
            else
            {
                hle_set_last_err(0x16u); // bad seek type
                return -1;
            }

            int64_t np = base + (int64_t)offset;
            if (np < 0)
                np = 0;
            if (np > (int64_t)hle_files_[fd].size)
                np = (int64_t)hle_files_[fd].size;
            hle_files_[fd].pos = (uint32_t)np;
            hle_set_last_err(0);
            return (int32_t)hle_files_[fd].pos;
        };

        auto hle_file_read = [&](int fd, uint32_t dst, uint32_t len) -> int32_t
        {
            cdrom::Cdrom* cd = bus_.cdrom();
            if (!cd || fd < 0 || fd >= 16 || !hle_files_[fd].used)
            {
                hle_set_last_err(0x09u); // invalid handle
                return -1;
            }
            if (len == 0)
            {
                hle_set_last_err(0x16u); // invalid length
                return -1;
            }

            const uint32_t pos = hle_files_[fd].pos;
            const uint32_t size = hle_files_[fd].size;
            if (pos >= size)
            {
                hle_set_last_err(0);
                return 0;
            }

            uint32_t todo = len;
            if (todo > (size - pos))
                todo = (size - pos);

            uint32_t done = 0;
            uint8_t sec[2048];
            while (done < todo)
            {
                const uint32_t fpos = pos + done;
                const uint32_t sec_idx = fpos / 2048u;
                const uint32_t sec_off = fpos % 2048u;
                const uint32_t lba = hle_files_[fd].lba + sec_idx;

                if (!cd->read_sector_2048(lba, sec))
                {
                    hle_set_last_err(0x10u); // general error
                    break;
                }

                uint32_t n = todo - done;
                const uint32_t avail = 2048u - sec_off;
                if (n > avail)
                    n = avail;

                if (!hle_write_guest(dst + done, sec + sec_off, n))
                {
                    hle_set_last_err(0x10u);
                    break;
                }

                done += n;
            }

            hle_files_[fd].pos = pos + done;
            if (done != 0)
                hle_set_last_err(0);
            return (int32_t)done;
        };

        auto hle_file_close = [&](int fd) -> int
        {
            if (fd < 0 || fd >= 16 || !hle_files_[fd].used)
            {
                hle_set_last_err(0x09u);
                return -1;
            }
            hle_free_fd(fd);
            hle_set_last_err(0);
            return fd;
        };

        // Default: succès "neutre".
        uint32_t ret_v0 = 0;
        int handled = 1;

        if (pc_ == 0x0000'00A0u)
        {
            switch (fn)
            {
                case 0x00u: // A(00h) FileOpen(filename, accessmode)
                    ret_v0 = (uint32_t)hle_file_open(a0, a1);
                    break;
                case 0x01u: // A(01h) FileSeek(fd, offset, seektype)
                    ret_v0 = (uint32_t)hle_file_seek((int)a0, (int32_t)a1, a2);
                    break;
                case 0x02u: // A(02h) FileRead(fd, dst, length)
                    ret_v0 = (uint32_t)hle_file_read((int)a0, a1, a2);
                    break;
                case 0x03u: // A(03h) FileWrite(fd, src, length) (CDROM: returns 0)
                    ret_v0 = 0;
                    break;
                case 0x04u: // A(04h) FileClose(fd)
                    ret_v0 = (uint32_t)hle_file_close((int)a0);
                    break;
                case 0x1Bu: // A(1Bh) strlen(src)
                    ret_v0 = hle_strlen(a0, 1024u * 1024u);
                    break;
                case 0x28u: // A(28h) bzero(dst,len)
                    (void)hle_memset(a0, 0, a1);
                    ret_v0 = a0;
                    break;
                case 0x2Au: // A(2Ah) memcpy(dst,src,len)
                    (void)hle_memcpy(a0, a1, a2);
                    ret_v0 = a0;
                    break;
                case 0x2Bu: // A(2Bh) memset(dst,fillbyte,len)
                    (void)hle_memset(a0, (uint8_t)(a1 & 0xFFu), a2);
                    ret_v0 = a0;
                    break;
                case 0x3Fu: // A(3Fh) printf(txt, ...)
                    {
                        char buf[512];
                        read_cstr_guest(a0, buf, (uint32_t)sizeof(buf));
                        for (uint32_t i = 0; buf[i] != 0; ++i)
                        {
                            const uint8_t ch = (uint8_t)buf[i];
                            std::fputc((int)ch, stderr);
                            if (text_out_)
                                std::fputc((int)ch, text_out_);
                            text_push_char(
                                text_io_, text_clock_, text_has_clock_, text_line_, (uint32_t)sizeof(text_line_), text_pos_, ch
                            );
                        }
                        std::fflush(stderr);
                        if (text_out_)
                            std::fflush(text_out_);
                        ret_v0 = 0;
                    }
                    break;
                case 0x44u: // A(44h) FlushCache()
                    ret_v0 = 0;
                    break;
                case 0x45u: // A(45h) init_a0_b0_c0_vectors
                    // On laisse le BIOS continuer; ici, on dépend surtout du HLE pour ne pas tomber sur NOP.
                    ret_v0 = 0;
                    break;
                default:
                    handled = 0;
                    break;
            }
        }
        else if (pc_ == 0x0000'00B0u)
        {
            // B0:0x3D = putchar(char) (souvent utilisé pendant le boot)
            if (fn == 0x3Du)
            {
                const uint8_t ch = (uint8_t)(a0 & 0xFFu);
                std::fputc((int)ch, stderr);
                std::fflush(stderr);
                if (text_out_)
                {
                    std::fputc((int)ch, text_out_);
                    std::fflush(text_out_);
                }
                text_push_char(text_io_, text_clock_, text_has_clock_, text_line_, (uint32_t)sizeof(text_line_), text_pos_, ch);
                ret_v0 = 1;
            }
            else
            {
                switch (fn)
                {
                    case 0x32u: // B(32h) FileOpen(filename, accessmode)
                        ret_v0 = (uint32_t)hle_file_open(a0, a1);
                        break;
                    case 0x33u: // B(33h) FileSeek(fd, offset, seektype)
                        ret_v0 = (uint32_t)hle_file_seek((int)a0, (int32_t)a1, a2);
                        break;
                    case 0x34u: // B(34h) FileRead(fd, dst, length)
                        ret_v0 = (uint32_t)hle_file_read((int)a0, a1, a2);
                        break;
                    case 0x35u: // B(35h) FileWrite(fd, src, length)
                        ret_v0 = 0;
                        break;
                    case 0x36u: // B(36h) FileClose(fd)
                        ret_v0 = (uint32_t)hle_file_close((int)a0);
                        break;
                    case 0x00u: // B(00h) alloc_kernel_memory(size)
                        {
                            const uint32_t size = a0;
                            uint32_t p = kalloc_ptr_;
                            p = (p + 3u) & ~3u;
                            if (size == 0 || p > kalloc_end_ || (kalloc_end_ - p) < size)
                            {
                                ret_v0 = 0;
                            }
                            else
                            {
                                ret_v0 = p;
                                kalloc_ptr_ = p + ((size + 3u) & ~3u);
                            }
                        }
                        break;
                    case 0x01u: // B(01h) free_kernel_memory(buf)
                        ret_v0 = 1;
                        break;
                    case 0x02u: // B(02h) init_timer(t,reload,flags)
                        {
                            const uint32_t t = a0;
                            const uint32_t reload = a1;
                            const uint32_t flags = a2;
                            if (t <= 2u)
                            {
                                const uint32_t base = 0x1F80'1100u + t * 0x10u;

                                // Reset old mode, set target (reload), set new mode.
                                (void)mmio_write_u16(base + 0x04u, 0);
                                (void)mmio_write_u16(base + 0x08u, (uint16_t)(reload & 0xFFFFu));

                                uint16_t mode = (flags & (1u << 4)) ? (uint16_t)0x0049u : (uint16_t)0x0048u;
                                if ((flags & 1u) == 0u)
                                    mode = (uint16_t)(mode | 0x0100u);
                                if (flags & (1u << 12))
                                    mode = (uint16_t)(mode | 0x0010u);

                                (void)mmio_write_u16(base + 0x04u, mode);
                                ret_v0 = 1;
                            }
                            else
                            {
                                ret_v0 = 0;
                            }
                        }
                        break;
                    case 0x03u: // B(03h) get_timer(t)
                        {
                            const uint32_t t = a0;
                            if (t <= 2u)
                            {
                                const uint32_t base = 0x1F80'1100u + t * 0x10u;
                                uint16_t cur = 0;
                                (void)mmio_read_u16(base + 0x00u, cur);
                                ret_v0 = (uint32_t)cur;
                            }
                            else
                            {
                                ret_v0 = 0;
                            }
                        }
                        break;
                    case 0x04u: // B(04h) enable_timer_irq(t)
                        {
                            const uint32_t t = a0;
                            if (t <= 2u)
                            {
                                uint32_t mask = 0;
                                (void)mmio_read_u32(0x1F80'1074u, mask);
                                mask |= (1u << (4u + t));
                                (void)mmio_write_u32(0x1F80'1074u, mask);
                                ret_v0 = 1;
                            }
                            else
                            {
                                // t=3 (vblank) => return 0 per docs; other => garbage.
                                ret_v0 = 0;
                            }
                        }
                        break;
                    case 0x05u: // B(05h) disable_timer_irq(t)
                        {
                            const uint32_t t = a0;
                            if (t <= 2u)
                            {
                                uint32_t mask = 0;
                                (void)mmio_read_u32(0x1F80'1074u, mask);
                                mask &= ~(1u << (4u + t));
                                (void)mmio_write_u32(0x1F80'1074u, mask);
                            }
                            ret_v0 = 1;
                        }
                        break;
                    case 0x06u: // B(06h) restart_timer(t)
                        {
                            const uint32_t t = a0;
                            if (t <= 2u)
                            {
                                const uint32_t base = 0x1F80'1100u + t * 0x10u;
                                (void)mmio_write_u16(base + 0x00u, 0);
                                ret_v0 = 1;
                            }
                            else
                            {
                                ret_v0 = 0;
                            }
                        }
                        break;
                    case 0x07u: // B(07h) DeliverEvent(class,spec)
                        {
                            const uint32_t cls = a0;
                            const uint32_t spec = a1;
                            for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                            {
                                HleEvent& e = hle_events_[i];
                                if ((e.status & 0x2000u) && e.cls == cls && e.spec == spec)
                                {
                                    // mode=2000h => mark ready.
                                    // mode=1000h => callback (non implémenté ici), on marque ready aussi pour le bring-up.
                                    e.status &= ~0x2000u;
                                    e.status |= 0x4000u;
                                }
                            }
                            ret_v0 = 1;
                        }
                        break;
                    case 0x08u: // B(08h) OpenEvent(class,spec,mode,func)
                        {
                            uint32_t idx = 0xFFFFFFFFu;
                            for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                            {
                                if (hle_events_[i].status == 0)
                                {
                                    idx = i;
                                    break;
                                }
                            }
                            if (idx == 0xFFFFFFFFu)
                            {
                                ret_v0 = 0xFFFFFFFFu;
                            }
                            else
                            {
                                HleEvent& e = hle_events_[idx];
                                e.cls = a0;
                                e.spec = a1;
                                e.mode = a2;
                                e.func = a3;
                                // mode (très simplifié):
                                // - 0x2000: enabled/busy
                                // - 0x1000: callback (non implémenté ici)
                                // Beaucoup de BIOS passent 0x2000 et s'attendent à ce que l'event soit actif.
                                e.status = (a2 & 0x2000u) ? 0x2000u : 0x1000u;
                                ret_v0 = 0xF100'0000u | (idx & 0xFFFFu);

                                if (sys_has_clock_)
                                {
                                    flog::logf(
                                        sys_io_,
                                        sys_clock_,
                                        flog::Level::info,
                                        "CPU",
                                        "HLE OpenEvent cls=0x%08X spec=0x%08X mode=0x%08X func=0x%08X -> handle=0x%08X",
                                        a0,
                                        a1,
                                        a2,
                                        a3,
                                        ret_v0
                                    );
                                }
                            }
                        }
                        break;
                    case 0x09u: // B(09h) CloseEvent(event)
                        {
                            const uint32_t h = a0;
                            if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                            {
                                const uint32_t idx = h & 0xFFFFu;
                                if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                                {
                                    hle_events_[idx] = {};
                                }
                            }
                            ret_v0 = 1;
                        }
                        break;
                    case 0x0Au: // B(0Ah) WaitEvent(event)
                        {
                            const uint32_t h = a0;
                            ret_v0 = 0;
                            if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                            {
                                const uint32_t idx = h & 0xFFFFu;
                                if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                                {
                                    HleEvent& e = hle_events_[idx];
                                    if (e.status & 0x4000u)
                                    {
                                        e.status &= ~0x4000u;
                                        e.status |= 0x2000u;
                                        ret_v0 = 1;
                                    }

                                    // Log minimal (throttled) pour diagnostiquer les boucles de WaitEvent.
                                    hle_wait_event_calls_++;
                                    const uint64_t n = hle_wait_event_calls_;
                                    const int log_it = (n <= 8u) || ((n & (n - 1u)) == 0u);
                                    if (log_it && sys_has_clock_)
                                    {
                                        flog::logf(
                                            sys_io_,
                                            sys_clock_,
                                            flog::Level::info,
                                            "CPU",
                                            "HLE WaitEvent handle=0x%08X idx=%u status=0x%08X -> v0=%u (call=%" PRIu64 ")",
                                            h,
                                            idx,
                                            e.status,
                                            ret_v0,
                                            n
                                        );
                                    }
                                }
                            }
                        }
                        break;
                    case 0x0Bu: // B(0Bh) TestEvent(event)
                        {
                            const uint32_t h = a0;
                            ret_v0 = 0;
                            if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                            {
                                const uint32_t idx = h & 0xFFFFu;
                                if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                                {
                                    HleEvent& e = hle_events_[idx];
                                    if (e.status & 0x4000u)
                                    {
                                        e.status &= ~0x4000u;
                                        e.status |= 0x2000u;
                                        ret_v0 = 1;
                                    }
                                }
                            }
                        }
                        break;
                    case 0x0Cu: // B(0Ch) EnableEvent(event)
                        {
                            const uint32_t h = a0;
                            ret_v0 = 0;
                            if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                            {
                                const uint32_t idx = h & 0xFFFFu;
                                if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                                {
                                    HleEvent& e = hle_events_[idx];
                                    if (e.status != 0)
                                    {
                                        // enabled/busy
                                        e.status &= ~0x4000u;
                                        e.status |= 0x2000u;
                                        ret_v0 = 1;
                                    }
                                }
                            }
                        }
                        break;
                    case 0x0Du: // B(0Dh) DisableEvent(event)
                        {
                            const uint32_t h = a0;
                            if ((h & 0xFFFF'0000u) == 0xF100'0000u)
                            {
                                const uint32_t idx = h & 0xFFFFu;
                                if (idx < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])))
                                {
                                    HleEvent& e = hle_events_[idx];
                                    if (e.status != 0)
                                        e.status = 0x1000u;
                                }
                            }
                            ret_v0 = 1;
                        }
                        break;
                    case 0x18u: // B(18h) ResetEntryInt()
                        {
                            // Retourne un pointeur vers une structure "savestate" type setjmp (30h bytes).
                            // On met une structure minimale dans une zone réservée.
                            const uint32_t base = entryint_struct_addr_;
                            // ra/pc, sp, fp, r16..r23, gp
                            (void)write_u32_guest(base + 0x00u, 0);
                            (void)write_u32_guest(base + 0x04u, 0x801F'FFF0u);
                            (void)write_u32_guest(base + 0x08u, 0);
                            for (uint32_t i = 0; i < 8; ++i)
                            {
                                (void)write_u32_guest(base + 0x0Cu + i * 4u, 0);
                            }
                            (void)write_u32_guest(base + 0x2Cu, 0);
                            ret_v0 = base;
                            entryint_hook_addr_ = 0;
                        }
                        break;
                    case 0x19u: // B(19h) HookEntryInt(addr)
                        entryint_hook_addr_ = a0;
                        ret_v0 = 1;
                        break;
                    case 0x20u: // B(20h) UnDeliverEvent(class,spec)
                        {
                            const uint32_t cls = a0;
                            const uint32_t spec = a1;
                            for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
                            {
                                HleEvent& e = hle_events_[i];
                                if ((e.status & 0x4000u) && e.cls == cls && e.spec == spec)
                                {
                                    // enabled/ready -> enabled/busy
                                    e.status &= ~0x4000u;
                                    e.status |= 0x2000u;
                                }
                            }
                            ret_v0 = 1;
                        }
                        break;
                    default:
                        handled = 0;
                        break;
                }
            }
        }
        else
        {
            // C0
            switch (fn)
            {
                case 0x00u: // C(00h) EnqueueTimerAndVblankIrqs(priority)
                    // Active un "tick" minimal pour débloquer WaitEvent sur la vblank software timer.
                    hle_pseudo_vblank_ = 1;
                    if (sys_has_clock_)
                    {
                        flog::logf(sys_io_, sys_clock_, flog::Level::info, "CPU", "HLE EnqueueTimerAndVblankIrqs (pseudo vblank ON)");
                    }
                    ret_v0 = 0;
                    break;
                case 0x01u: // C(01h) EnqueueSyscallHandler(priority)
                    ret_v0 = 0;
                    break;
                case 0x07u: // C(07h) InstallExceptionHandlers()
                    ret_v0 = 0;
                    break;
                case 0x08u: // C(08h) SysInitMemory(addr,size)
                    kalloc_ptr_ = a0;
                    kalloc_end_ = a0 + a1;
                    ret_v0 = 0;
                    break;
                case 0x0Au: // C(0Ah) ChangeClearRCnt(t,flag)
                    // Non implémenté: pour bring-up, on accepte et retourne 0.
                    ret_v0 = 0;
                    break;
                case 0x0Cu: // C(0Ch) InitDefInt(priority)
                    ret_v0 = 0;
                    break;
                case 0x12u: // C(12h) InstallDevices(ttyflag)
                    ret_v0 = 0;
                    break;
                case 0x1Cu: // C(1Ch) AdjustA0Table()
                    ret_v0 = 0;
                    break;
                default:
                    handled = 0;
                    break;
            }
        }

        if (!handled)
        {
            if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::exc,
                    "HLE BIOS vector PC=0x%08X fn=0x%02X a0=0x%08X a1=0x%08X a2=0x%08X (unhandled, fallback v0=0)",
                    pc_,
                    fn,
                    a0,
                    a1,
                    a2
                );
            }
            ret_v0 = 0;
        }
        else
        {
            if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::exc,
                    "HLE BIOS vector PC=0x%08X fn=0x%02X -> v0=0x%08X",
                    pc_,
                    fn,
                    ret_v0
                );
            }
        }

        gpr_[2] = ret_v0;
        pc_ = gpr_[31]; // ra
        r.kind = StepResult::Kind::ok;
        r.instr = 0;
        return r;
    }

    // Exception vector en RAM (BEV=0): 0x80000080.
    // HLE "safe" uniquement si la RAM à 0x00000080 est encore vide (tout à 0).
    // Dès que le BIOS installe un vrai handler, on doit l'exécuter (sinon on casse le dispatch).
    if (hle_vectors_ && pc_ == 0x8000'0080u)
    {
        uint32_t w0 = 0;
        uint32_t w1 = 0;
        uint32_t w2 = 0;
        uint32_t w3 = 0;
        Bus::MemFault f{};
        const int ok0 = bus_.read_u32(0x0000'0080u, w0, f) ? 1 : 0;
        const int ok1 = bus_.read_u32(0x0000'0084u, w1, f) ? 1 : 0;
        const int ok2 = bus_.read_u32(0x0000'0088u, w2, f) ? 1 : 0;
        const int ok3 = bus_.read_u32(0x0000'008Cu, w3, f) ? 1 : 0;

        const int ram_vec_empty = (ok0 && ok1 && ok2 && ok3 && w0 == 0 && w1 == 0 && w2 == 0 && w3 == 0) ? 1 : 0;
        if (!ram_vec_empty)
        {
            // Laisser le handler RAM s'exécuter.
        }
        else
        {
            // On logge ici parce que si on boucle sur 0x80000080, c'est typiquement une exception
            // "non gérée" (MMIO manquant, IRQ, ou autre détail COP0).
            exc_vec_hits_++;
            const uint32_t cause = cop0_[COP0_CAUSE];
            const uint32_t status = cop0_[COP0_STATUS];
            const uint32_t epc = cop0_[COP0_EPC];
            const uint32_t bad = cop0_[COP0_BADVADDR];
            const uint32_t code = (cause >> 2) & 0x1Fu;

            // Throttle: premières occurrences, puis espacées (puissances de 2) pour éviter le spam.
            const int log_it = (exc_vec_hits_ <= 16u) || ((exc_vec_hits_ & (exc_vec_hits_ - 1u)) == 0u);
            if (log_it && sys_has_clock_)
            {
                // Lire l'instruction à EPC pour comprendre ce qui a réellement fauté.
                uint32_t epc_instr = 0;
                Bus::MemFault epc_fault{};
                const uint32_t epc_phys = virt_to_phys(epc);
                (void)bus_.read_u32(epc_phys, epc_instr, epc_fault);

                // Decode minimal, surtout utile pour ADES (store misaligné).
                const uint32_t opc = op(epc_instr);
                const uint32_t rs_i = rs(epc_instr);
                const uint32_t rt_i = rt(epc_instr);
                const int16_t imm_i = imm_s(epc_instr);
                const uint32_t base_v = gpr_[rs_i & 31u];
                const uint32_t rt_v = gpr_[rt_i & 31u];
                const uint32_t eff = base_v + (uint32_t)(int32_t)imm_i;

                const char* store_name = "";
                if (opc == 0x28u)
                    store_name = "SB";
                else if (opc == 0x29u)
                    store_name = "SH";
                else if (opc == 0x2Au)
                    store_name = "SWL";
                else if (opc == 0x2Bu)
                    store_name = "SW";
                else if (opc == 0x2Eu)
                    store_name = "SWR";

                flog::logf(
                    sys_log_,
                    sys_clock_,
                    flog::Level::warn,
                    "CPU",
                    "HLE empty RAM vector hit=%" PRIu64
                    " code=%u EPC=0x%08X BadVAddr=0x%08X Cause=0x%08X Status=0x%08X EPCInstr=0x%08X%s%s rs=%u(0x%08X) rt=%u(0x%08X) imm=%d eff=0x%08X%s",
                    exc_vec_hits_,
                    code,
                    epc,
                    bad,
                    cause,
                    status,
                    epc_instr,
                    store_name[0] ? " [" : "",
                    store_name[0] ? store_name : "",
                    rs_i,
                    base_v,
                    rt_i,
                    rt_v,
                    (int)imm_i,
                    eff,
                    store_name[0] ? "]" : ""
                );
                flog::logf(
                    sys_io_,
                    sys_clock_,
                    flog::Level::warn,
                    "CPU",
                    "HLE empty RAM vector hit=%" PRIu64
                    " code=%u EPC=0x%08X BadVAddr=0x%08X Cause=0x%08X Status=0x%08X EPCInstr=0x%08X%s%s rs=%u(0x%08X) rt=%u(0x%08X) imm=%d eff=0x%08X%s",
                    exc_vec_hits_,
                    code,
                    epc,
                    bad,
                    cause,
                    status,
                    epc_instr,
                    store_name[0] ? " [" : "",
                    store_name[0] ? store_name : "",
                    rs_i,
                    base_v,
                    rt_i,
                    rt_v,
                    (int)imm_i,
                    eff,
                    store_name[0] ? "]" : ""
                );
            }

            if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
            {
                rlog::logger_logf(
                    logger_, rlog::Level::debug, rlog::Category::exc, "HLE empty RAM exception vector -> 0xBFC00180"
                );
            }
            pc_ = 0xBFC0'0180u;
            r.kind = StepResult::Kind::ok;
            r.instr = 0;
            return r;
        }
    }

    // Exception vector en RAM (BEV=0): 0x80000080 (normal).
    // Si un handler est présent, on le laisse s'exécuter, donc pas de return ici.

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
        // Pour le boot BIOS, un IFETCH hors mapping est un signe fort qu'on est "sorti"
        // de la zone de code valide (bug CPU/delay slot ou hardware manquant).
        // On arrête ici pour garder un signal clair dans le log.
        if (logger_)
        {
            // Dump des dernières instructions connues (ring buffer)
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Recent trace (latest last):");
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t pos = (recent_pos_ - 64 + i) & 255u;
                const uint32_t pc = recent_pc_[pos];
                const uint32_t ii = recent_instr_[pos];
                if (pc == 0 && ii == 0)
                    continue;
                rlog::logger_logf(
                    logger_, rlog::Level::error, rlog::Category::exc, "  PC=0x%08X INSTR=0x%08X", pc, ii
                );
            }
        }
        r.kind = StepResult::Kind::mem_fault;
        r.mem_fault = fault;
        return r;
    }
    r.instr = instr;

    // Ring buffer: on capture après un fetch réussi.
    const uint32_t prev_pos = (recent_pos_ - 1) & 255u;
    const uint32_t prev_pc = recent_pc_[prev_pos];
    recent_pc_[recent_pos_ & 255u] = pc_;
    recent_instr_[recent_pos_ & 255u] = instr;
    recent_pos_ = (recent_pos_ + 1) & 255u;

    if (stop_on_pc_ && !stopped_on_pc_ && pc_ == stop_pc_)
    {
        stopped_on_pc_ = 1;
        if (logger_)
        {
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Stop-on-pc: PC=0x%08X", pc_);
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Recent trace (latest last):");
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t pos = (recent_pos_ - 64 + i) & 255u;
                rlog::logger_logf(
                    logger_,
                    rlog::Level::error,
                    rlog::Category::exc,
                    "  PC=0x%08X INSTR=0x%08X",
                    recent_pc_[pos],
                    recent_instr_[pos]
                );
            }
        }
        r.kind = StepResult::Kind::halted;
        return r;
    }

    // Debug: saut depuis le BIOS (ROM) vers de la RAM vide (NOP) => très typiquement un
    // mapping/hardware manquant pendant l'init.
    const auto is_ram_window = [&](uint32_t vaddr) -> int
    {
        // 2 MiB main RAM sur PS1.
        // On accepte les 3 alias courants: KUSEG (0x00000000), KSEG0 (0x80000000), KSEG1 (0xA0000000).
        if (vaddr < 0x0020'0000u)
            return 1;
        if (vaddr >= 0x8000'0000u && vaddr < 0x8020'0000u)
            return 1;
        if (vaddr >= 0xA000'0000u && vaddr < 0xA020'0000u)
            return 1;
        return 0;
    };

    if (stop_on_bios_to_ram_nop_ && instr == 0 && is_ram_window(pc_) && prev_pc >= 0xBFC0'0000u &&
        prev_pc < 0xBFC8'0000u)
    {
        if (logger_)
        {
            rlog::logger_logf(
                logger_,
                rlog::Level::error,
                rlog::Category::exc,
                "Stop: BIOS->RAM NOP transition (prev PC=0x%08X, PC=0x%08X)",
                prev_pc,
                pc_
            );
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Recent trace (latest last):");
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t pos = (recent_pos_ - 64 + i) & 255u;
                rlog::logger_logf(
                    logger_,
                    rlog::Level::error,
                    rlog::Category::exc,
                    "  PC=0x%08X INSTR=0x%08X",
                    recent_pc_[pos],
                    recent_instr_[pos]
                );
            }
        }
        r.kind = StepResult::Kind::halted;
        return r;
    }

    // Debug: stop dès qu'on "entre" dans un bloc de NOPs en RAM (transition non-NOP -> NOP).
    // C'est souvent un symptôme d'un return address corrompu / d'un handler IRQ/DMA non implémenté.
    const auto is_control_flow = [&](uint32_t i) -> int
    {
        const uint32_t o = op(i);
        if (o == 0x02 || o == 0x03) // J / JAL
            return 1;
        if (o == 0x00)
        {
            const uint32_t f = funct(i);
            if (f == 0x08 || f == 0x09) // JR / JALR
                return 1;
        }
        if (o == 0x04 || o == 0x05 || o == 0x06 || o == 0x07) // BEQ/BNE/BLEZ/BGTZ
            return 1;
        if (o == 0x01) // BLTZ/BGEZ + variants
            return 1;
        return 0;
    };

    const uint32_t prev_instr = recent_instr_[prev_pos];
    const auto is_load = [&](uint32_t i) -> int
    {
        const uint32_t o = op(i);
        if (o == 0x20 || o == 0x21 || o == 0x22 || o == 0x23 || o == 0x24 || o == 0x25 || o == 0x26)
            return 1; // LB/LH/LWL/LW/LBU/LHU/LWR
        if (o == 0x32)
            return 1; // LWC2
        if (o == 0x10 && rs(i) == 0x00)
            return 1; // MFC0 (mode simplifié: load delay slot)
        if (o == 0x12 && rs(i) == 0x00)
            return 1; // MFC2
        return 0;
    };

    // On ignore le NOP "normal" dans:
    // - le delay slot d'un control-flow (branch/jump)
    // - le load delay slot (souvent un NOP inséré par le code BIOS/compilo)
    const int is_expected_nop_slot = ((is_control_flow(prev_instr) || is_load(prev_instr)) && pc_ == (prev_pc + 4));

    if (stop_on_ram_nop_ && instr == 0 && is_ram_window(pc_) && prev_pc != 0 && prev_instr != 0 &&
        !is_expected_nop_slot)
    {
        if (logger_)
        {
            rlog::logger_logf(
                logger_,
                rlog::Level::error,
                rlog::Category::exc,
                "Stop: RAM NOP transition (prev PC=0x%08X prev INSTR=0x%08X, PC=0x%08X)",
                prev_pc,
                prev_instr,
                pc_
            );
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Recent trace (latest last):");
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t pos = (recent_pos_ - 64 + i) & 255u;
                rlog::logger_logf(
                    logger_,
                    rlog::Level::error,
                    rlog::Category::exc,
                    "  PC=0x%08X INSTR=0x%08X",
                    recent_pc_[pos],
                    recent_instr_[pos]
                );
            }
        }
        r.kind = StepResult::Kind::halted;
        return r;
    }

    // Debug: détecter l'entrée dans la zone "haut RAM" (typiquement stack),
    // ce qui peut signaler un return address corrompu pendant le boot BIOS.
    if (stop_on_high_ram_ && !stopped_on_high_ram_ && pc_ >= 0x801F'F000u && pc_ < 0x8020'0000u)
    {
        stopped_on_high_ram_ = 1;
        if (logger_)
        {
            rlog::logger_logf(
                logger_, rlog::Level::error, rlog::Category::exc, "Stop-on-high-ram: PC=0x%08X", pc_
            );
            rlog::logger_logf(logger_, rlog::Level::error, rlog::Category::exc, "Recent trace (latest last):");
            for (uint32_t i = 0; i < 64; ++i)
            {
                const uint32_t pos = (recent_pos_ - 64 + i) & 255u;
                rlog::logger_logf(
                    logger_,
                    rlog::Level::error,
                    rlog::Category::exc,
                    "  PC=0x%08X INSTR=0x%08X",
                    recent_pc_[pos],
                    recent_instr_[pos]
                );
            }
        }
        r.kind = StepResult::Kind::halted;
        return r;
    }

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

    // COP0 Count: utilisé par le BIOS pour des "delays" (busy-wait / timeouts).
    // Si Count ne bouge jamais, le BIOS peut rester bloqué indéfiniment.
    // Modèle simplifié: +1 par instruction (pas cycle-accurate, mais suffisant pour la démo).
    cop0_[COP0_COUNT] += 1;
    bus_.tick(1);

    // HLE: pseudo "vblank/tick" pour débloquer le kernel quand on n'a pas encore de GPU/VBlank réel.
    // On l'active généralement via C(00h) EnqueueTimerAndVblankIrqs().
    if (hle_pseudo_vblank_)
    {
        // Valeur arbitraire (pas cycle-accurate). Suffit pour casser les boucles "wait".
        hle_vblank_div_++;
        if (hle_vblank_div_ >= 100000u)
        {
            hle_vblank_div_ = 0;
            for (uint32_t i = 0; i < (uint32_t)(sizeof(hle_events_) / sizeof(hle_events_[0])); ++i)
            {
                HleEvent& e = hle_events_[i];
                // BIOS/kernel attend souvent différents "ticks" (vblank, root counters, etc.).
                // Pour le bring-up, on rend ready quelques événements courants.
                const int enabled_busy = (e.status & 0x2000u) ? 1 : 0;
                const int want_vblank = (e.cls == 0xF200'0003u && e.spec == 0x0002u) ? 1 : 0;
                const int want_tick = (e.cls == 0xF000'0009u && e.spec == 0x0000'0020u) ? 1 : 0;
                if (enabled_busy && (want_vblank || want_tick))
                {
                    // enabled/busy -> enabled/ready
                    e.status &= ~0x2000u;
                    e.status |= 0x4000u;

                    // Throttled log: utile pour vérifier qu'on "pulse" bien des events.
                    hle_mark_ready_calls_++;
                    const uint64_t n = hle_mark_ready_calls_;
                    const int log_it = (n <= 8u) || ((n & (n - 1u)) == 0u);
                    if (log_it && sys_has_clock_)
                    {
                        flog::logf(
                            sys_io_,
                            sys_clock_,
                            flog::Level::info,
                            "CPU",
                            "HLE DeliverTick cls=0x%08X spec=0x%08X -> READY (idx=%u, call=%" PRIu64 ")",
                            e.cls,
                            e.spec,
                            i,
                            n
                        );
                    }
                }
            }
        }
    }

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
    //
    // IMPORTANT (bring-up BIOS):
    // R3000A: COP0 Status bit16 = Isc (Isolate Cache). Quand Isc=1, les loads/stores sur segments "cached"
    // ne doivent plus toucher la RAM (ils vont dans les caches). Le BIOS utilise ça pour invalider/initialiser
    // les caches au boot via des boucles de stores: si on applique ces stores sur la RAM, on peut effacer
    // des structures kernel/vecteurs (ex: A0/B0/C0, 0x80) et bloquer l'init.
    //
    // On implémente ici une approximation minimale mais utile: quand Isc=1, on ignore les STORES vers la RAM
    // (phys < ram_size). Ça suffit généralement à éviter de "clobber" la low RAM pendant les routines cache-clear.
    const int cache_isolated = (cop0_[COP0_STATUS] & (1u << 16)) ? 1 : 0;
    auto is_cached_segment = [&](uint32_t vaddr) -> int
    {
        // PS1: KUSEG (0x0000_0000..0x7FFF_FFFF) + KSEG0 (0x8000_0000..0x9FFF_FFFF) sont "cached".
        // KSEG1 (0xA000_0000..) est "uncached" et doit continuer à accéder au bus.
        if (vaddr < 0x8000'0000u)
            return 1;
        if ((vaddr & 0xE000'0000u) == 0x8000'0000u)
            return 1;
        return 0;
    };

    auto cache_iso_read_u8 = [&](uint32_t vaddr, uint8_t& out) -> int
    {
        // I-cache est 4KB. Les adresses "cached" mappent via le low offset.
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        out = icache_data_[idx];
        return 1;
    };
    auto cache_iso_read_u16 = [&](uint32_t vaddr, uint16_t& out) -> int
    {
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        const uint32_t i0 = idx & 0x0FFFu;
        const uint32_t i1 = (idx + 1u) & 0x0FFFu;
        out = (uint16_t)icache_data_[i0] | (uint16_t)((uint16_t)icache_data_[i1] << 8);
        return 1;
    };
    auto cache_iso_read_u32 = [&](uint32_t vaddr, uint32_t& out) -> int
    {
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        const uint32_t i0 = idx & 0x0FFFu;
        const uint32_t i1 = (idx + 1u) & 0x0FFFu;
        const uint32_t i2 = (idx + 2u) & 0x0FFFu;
        const uint32_t i3 = (idx + 3u) & 0x0FFFu;
        out = (uint32_t)icache_data_[i0] | ((uint32_t)icache_data_[i1] << 8) | ((uint32_t)icache_data_[i2] << 16) |
            ((uint32_t)icache_data_[i3] << 24);
        return 1;
    };
    auto cache_iso_write_u8 = [&](uint32_t vaddr, uint8_t v) -> int
    {
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        icache_data_[idx] = v;
        return 1;
    };
    auto cache_iso_write_u16 = [&](uint32_t vaddr, uint16_t v) -> int
    {
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        icache_data_[idx & 0x0FFFu] = (uint8_t)(v & 0xFFu);
        icache_data_[(idx + 1u) & 0x0FFFu] = (uint8_t)((v >> 8) & 0xFFu);
        return 1;
    };
    auto cache_iso_write_u32 = [&](uint32_t vaddr, uint32_t v) -> int
    {
        const uint32_t idx = virt_to_phys(vaddr) & 0x0FFFu;
        icache_data_[idx & 0x0FFFu] = (uint8_t)(v & 0xFFu);
        icache_data_[(idx + 1u) & 0x0FFFu] = (uint8_t)((v >> 8) & 0xFFu);
        icache_data_[(idx + 2u) & 0x0FFFu] = (uint8_t)((v >> 16) & 0xFFu);
        icache_data_[(idx + 3u) & 0x0FFFu] = (uint8_t)((v >> 24) & 0xFFu);
        return 1;
    };
    auto load_u8 = [&](uint32_t vaddr, uint8_t& out) -> int
    {
        Bus::MemFault f{};
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_read_u8(vaddr, out);
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
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_read_u16(vaddr, out);
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
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_read_u32(vaddr, out);
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
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_write_u8(vaddr, v);
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
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_write_u16(vaddr, v);
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
        if (cache_isolated && is_cached_segment(vaddr))
            return cache_iso_write_u32(vaddr, v);
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
                                // On imprime sur stderr pour éviter d'être "noyé" par le --pretty/logs
                                // (stdout). Ça rend le printf guest beaucoup plus visible en live.
                                std::fprintf(stderr, "[GUEST] %u (0x%08X)\n", v, v);
                                std::fflush(stderr);
                            }
                            else if (svc == 0xFF02u)
                            {
                                const uint8_t ch = (uint8_t)(gpr_[4] & 0xFFu);
                                std::fputc((int)ch, stderr);
                                std::fflush(stderr);
                                if (text_out_)
                                {
                                    std::fputc((int)ch, text_out_);
                                    std::fflush(text_out_);
                                }
                                text_push_char(
                                    text_io_,
                                    text_clock_,
                                    text_has_clock_,
                                    text_line_,
                                    (uint32_t)sizeof(text_line_),
                                    text_pos_,
                                    ch
                                );
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
                                    std::fputc((int)b, stderr);
                                    if (text_out_)
                                    {
                                        std::fputc((int)b, text_out_);
                                    }
                                    text_push_char(
                                        text_io_,
                                        text_clock_,
                                        text_has_clock_,
                                        text_line_,
                                        (uint32_t)sizeof(text_line_),
                                        text_pos_,
                                        b
                                    );
                                    addr++;
                                }
                                std::fflush(stderr);
                                if (text_out_)
                                {
                                    std::fflush(text_out_);
                                }
                                // Pas de flush line ici: c'est un flux, on flush sur '\n'.
                            }
                            else
                            {
                                // PSX "SYS" calls:
                                // no$psx/psx-spx: le numéro de fonction est dans a0 (r4),
                                // l'imm20 de l'opcode SYSCALL est généralement 0.
                                // SYS(01) EnterCriticalSection, SYS(02) ExitCriticalSection.
                                const uint32_t sysfn = gpr_[4] & 0xFFu;
                                if (sysfn == 0x00u)
                                {
                                    // NoFunction(): ne fait rien.
                                    break;
                                }
                                if (sysfn == 0x01u)
                                {
                                    // EnterCriticalSection(): disable interrupts.
                                    //
                                    // PS1 kernel uses SYSCALL SYS(01)/(02) to enter/exit critical sections.
                                    // For our minimal COP0 model, the important part is COP0.Status.IEc (bit0):
                                    // - IEc=0 => interrupts globally disabled
                                    // - IEc=1 => interrupts enabled (subject to IM bits)
                                    //
                                    // Return value: keep it compatible with our previous behavior:
                                    // return 1 if interrupts were already disabled, else 0.
                                    uint32_t st = cop0_[COP0_STATUS];
                                    const uint32_t was_ie = st & 1u;
                                    st &= ~1u; // clear IEc
                                    cop0_[COP0_STATUS] = st;
                                    gpr_[2] = (was_ie == 0u) ? 1u : 0u;
                                    break;
                                }
                                if (sysfn == 0x02u)
                                {
                                    // ExitCriticalSection(): enable interrupts.
                                    //
                                    // On PS1, INTC output is wired to CPU HW interrupt line 2 (COP0.Status.IM2 / Cause.IP2).
                                    // If IM2 is not set, BIOS IRQ-driven facilities (notably VSync counters) will never tick.
                                    uint32_t st = cop0_[COP0_STATUS];
                                    st |= 1u;        // IEc
                                    st |= (1u << 10); // IM2
                                    cop0_[COP0_STATUS] = st;
                                    break;
                                }

                                // Petit HLE opportuniste: certains environnements BIOS/kernel utilisent
                                // SYSCALL pour des sorties debug/console.
                                // On détecte un pattern "write-like" et on imprime côté hôte.
                                //
                                // Heuristique "write-like":
                                // - v0 = 0
                                // - a0 = fd (souvent 1=stdout ou 2=stderr)
                                // - a1 = len
                                // - a2 = ptr
                                //
                                // NOTE: on le route aussi vers logs/outtext.log si branché (text_out_).
                                if ((gpr_[2] == 0) && ((gpr_[4] == 1) || (gpr_[4] == 2)) && (gpr_[5] <= 0x1000u))
                                {
                                    const uint32_t len = gpr_[5];
                                    const uint32_t ptr = gpr_[6];
                                    for (uint32_t i = 0; i < len && i < 1024u; ++i)
                                    {
                                        uint8_t b = 0;
                                        if (!load_u8(ptr + i, b))
                                            break;
                                        std::fputc((int)b, stderr);
                                        if (text_out_)
                                        {
                                            std::fputc((int)b, text_out_);
                                        }
                                        text_push_char(
                                            text_io_,
                                            text_clock_,
                                            text_has_clock_,
                                            text_line_,
                                            (uint32_t)sizeof(text_line_),
                                            text_pos_,
                                            b
                                        );
                                    }
                                    std::fflush(stderr);
                                    if (text_out_)
                                    {
                                        std::fflush(text_out_);
                                    }
                                    // Convention: retourne le nombre d'octets écrits.
                                    gpr_[2] = len;
                                    break;
                                }

                                // Sinon, comportement "réaliste": exception SYSCALL.
                                if (logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::exc))
                                {
                                    rlog::logger_logf(
                                        logger_,
                                        rlog::Level::debug,
                                        rlog::Category::exc,
                                        "SYSCALL guest v0=0x%08X a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X",
                                        gpr_[2],
                                        gpr_[4],
                                        gpr_[5],
                                        gpr_[6],
                                        gpr_[7]
                                    );
                                }
                                raise_exception(EXC_SYS, 0, r.pc);
                            }
                            break;
                        }
                    case 0x0D:
                        { // BREAK
                            // BREAK est normalement une exception Breakpoint (code = Bp = 9).
                            // Mais sans debugger attaché, le BIOS entre en boucle infinie.
                            // Pour le bring-up, on traite BREAK comme un NOP (skip).
                            static int break_skip_count = 0;
                            if (++break_skip_count <= 5)
                            {
                                std::fprintf(stderr, "[CPU] BREAK at PC=0x%08X (skipping as NOP)\n", r.pc);
                                std::fflush(stderr);
                            }
                            // Just continue execution (NOP behavior)
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
                        v = (v & 0x00FFFFFFu) | (w << 24);
                        break;
                    case 1:
                        v = (v & 0x0000FFFFu) | (w << 16);
                        break;
                    case 2:
                        v = (v & 0x000000FFu) | (w << 8);
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
                        v = (v & 0xFF000000u) | (w >> 8);
                        break;
                    case 2:
                        v = (v & 0xFFFF0000u) | (w >> 16);
                        break;
                    case 3:
                        v = (v & 0xFFFFFF00u) | (w >> 24);
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
                        const uint32_t old_st = st;
                        st = (st & ~0x3Fu) | ((st >> 2) & 0x3Fu);
                        cop0_[COP0_STATUS] = st;

                        // Debug: log RFE
                        static int rfe_count = 0;
                        ++rfe_count;
                        if (rfe_count <= 20 || (rfe_count % 10000 == 0))
                        {
                            std::fprintf(stderr, "[CPU] RFE #%d: PC=0x%08X status 0x%08X -> 0x%08X EPC=0x%08X\n",
                                rfe_count, r.pc, old_st, st, cop0_[COP0_EPC]);
                            std::fflush(stderr);
                        }
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

    // Debug: trace MMIO verbeuse (I/O) pour comprendre ce que le BIOS attend (IRQ/DMA/CDROM/GPU).
    if (trace_io_ && mem_valid && logger_ && rlog::logger_enabled(logger_, rlog::Level::debug, rlog::Category::mem))
    {
        const uint32_t phys = virt_to_phys(mem_addr);
        if (psx_is_mmio(phys))
        {
            const char* name = psx_mmio_name(phys);
            if (name)
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::mem,
                    "MMIO %s %s (vaddr=0x%08X phys=0x%08X) val=0x%08X",
                    mem_op,
                    name,
                    mem_addr,
                    phys,
                    mem_val
                );
            }
            else
            {
                rlog::logger_logf(
                    logger_,
                    rlog::Level::debug,
                    rlog::Category::mem,
                    "MMIO %s (vaddr=0x%08X phys=0x%08X) val=0x%08X",
                    mem_op,
                    mem_addr,
                    phys,
                    mem_val
                );
            }
        }
    }

    return r;
}

} // namespace r3000
