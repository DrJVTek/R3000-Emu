#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "log/logger.h"
#include "r3000/bus.h"
#include "r3000/cpu.h"

// Encode helpers (juste pour construire une mini-ROM de démo dans le code).
static uint32_t enc_i(uint32_t op, uint32_t rs, uint32_t rt, uint16_t imm)
{
    return (op << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) | (uint32_t)imm;
}
static uint32_t enc_r(uint32_t rs, uint32_t rt, uint32_t rd, uint32_t sh, uint32_t fn)
{
    return (0u << 26) | ((rs & 31u) << 21) | ((rt & 31u) << 16) | ((rd & 31u) << 11) |
           ((sh & 31u) << 6) | (fn & 63u);
}
static uint32_t enc_j(uint32_t op, uint32_t idx)
{
    return (op << 26) | (idx & 0x03FF'FFFFu);
}

static void write_u32_le(uint8_t* mem, uint32_t addr, uint32_t v)
{
    mem[addr + 0] = (uint8_t)(v & 0xFFu);
    mem[addr + 1] = (uint8_t)((v >> 8) & 0xFFu);
    mem[addr + 2] = (uint8_t)((v >> 16) & 0xFFu);
    mem[addr + 3] = (uint8_t)((v >> 24) & 0xFFu);
}

// Mini programme MIPS (R3000) :
// - configure t0 = 0x1F000000 (MMIO print)
// - compteur t1 de 1 à 5
// - à chaque itération: SW t1, 0(t0) -> imprime
// - BREAK
static void build_demo_rom(uint8_t* mem, uint32_t base_addr)
{
    uint32_t pc = base_addr;

    // lui  t0, 0x1F00
    write_u32_le(mem, pc, enc_i(0x0F, 0, 8, 0x1F00));
    pc += 4;
    // ori  t0, t0, 0x0000
    write_u32_le(mem, pc, enc_i(0x0D, 8, 8, 0x0000));
    pc += 4;
    // addiu t1, r0, 1
    write_u32_le(mem, pc, enc_i(0x09, 0, 9, 1));
    pc += 4;
    // addiu t2, r0, 6  (limit = 6 ; loop while t1 != t2)
    write_u32_le(mem, pc, enc_i(0x09, 0, 10, 6));
    pc += 4;

    const uint32_t loop_pc = pc;
    // sw t1, 0(t0)
    write_u32_le(mem, pc, enc_i(0x2B, 8, 9, 0));
    pc += 4;
    // addiu t1, t1, 1
    write_u32_le(mem, pc, enc_i(0x09, 9, 9, 1));
    pc += 4;
    // bne t1, t2, loop
    // off = (loop_pc - (pc+4)) >> 2 ; mais ici pc est sur bne, step() utilise pc_ déjà avancé =>
    // standard MIPS: target = PC+4 + (off<<2) Au moment de l'encodage: PC = pc (adresse de bne).
    // PC+4 = pc+4.
    const int32_t off_words = (int32_t)(loop_pc - (pc + 4)) / 4;
    write_u32_le(mem, pc, enc_i(0x05, 9, 10, (uint16_t)off_words));
    pc += 4;
    // nop (delay slot)
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0));
    pc += 4;

    // break
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0x0D));
    pc += 4;
}

// Mini programme GTE (COP2) :
// - configure t0 = 0x1F000000 (MMIO print)
// - configure quelques registres GTE via CTC2/MTC2
// - exécute RTPS + lit SXYP/SZ3/OTZ via MFC2
// - imprime les valeurs via SW sur MMIO
// - BREAK
//
// Objectif: vérifier rapidement "CPU -> COP2 -> GTE" + load delay slot sur MFC2.
static uint32_t enc_cop2_xfer(uint32_t rs_field, uint32_t rt_cpu, uint32_t rd_gte)
{
    return (0x12u << 26) | ((rs_field & 31u) << 21) | ((rt_cpu & 31u) << 16) |
           ((rd_gte & 31u) << 11);
}

static uint32_t enc_cop2_cmd(uint32_t funct6, uint32_t sf = 0, uint32_t lm = 0)
{
    // Command word (subset): sf bit(19), lm bit(10), funct bits(5..0).
    const uint32_t cmd = ((sf & 1u) << 19) | ((lm & 1u) << 10) | (funct6 & 63u);
    return (0x12u << 26) | (0x10u << 21) | cmd;
}

static void build_gte_demo_rom(uint8_t* mem, uint32_t base_addr)
{
    uint32_t pc = base_addr;

    // lui  t0, 0x1F00  (MMIO print)
    write_u32_le(mem, pc, enc_i(0x0F, 0, 8, 0x1F00));
    pc += 4;
    // ori  t0, t0, 0x0000
    write_u32_le(mem, pc, enc_i(0x0D, 8, 8, 0x0000));
    pc += 4;

    // ---- Setup GTE control regs (identity rotation, H=256) ----
    // t1 = 0x00010000 (r11=1, r12=0)  -> CTC2 t1, C_R11R12 (idx 0)
    write_u32_le(mem, pc, enc_i(0x0F, 0, 9, 0x0001));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x06, 9, 0));
    pc += 4;

    // t1 = 0x00000001 (r33=1) -> CTC2 t1, C_R33 (idx 4)
    write_u32_le(mem, pc, enc_i(0x09, 0, 9, 1));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x06, 9, 4));
    pc += 4;

    // t1 = 0x00000100 (H=256) -> CTC2 t1, C_H (idx 26)
    write_u32_le(mem, pc, enc_i(0x09, 0, 9, 0x0100));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x06, 9, 26));
    pc += 4;

    // ZSF3=1, ZSF4=1 (pour AVSZ3/4 si besoin)
    write_u32_le(mem, pc, enc_i(0x09, 0, 9, 1));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x06, 9, 29));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x06, 9, 30));
    pc += 4;

    // ---- Setup V0 = (x=100,y=50,z=1000) ----
    // t2 = (y<<16)|x
    write_u32_le(mem, pc, enc_i(0x09, 0, 10, 100));
    pc += 4;
    write_u32_le(mem, pc, enc_i(0x09, 0, 11, 50));
    pc += 4;
    // sll t3, t3, 16 ; or t2, t2, t3
    write_u32_le(mem, pc, enc_r(0, 11, 12, 16, 0x00));
    pc += 4;
    write_u32_le(mem, pc, enc_r(10, 12, 10, 0, 0x25));
    pc += 4;
    // MTC2 t2, VXY0 (idx 0)
    write_u32_le(mem, pc, enc_cop2_xfer(0x04, 10, 0));
    pc += 4;

    // t2 = z (1000) ; MTC2 t2, VZ0 (idx 1)
    write_u32_le(mem, pc, enc_i(0x09, 0, 10, 1000));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x04, 10, 1));
    pc += 4;

    // ---- Execute RTPS ----
    write_u32_le(mem, pc, enc_cop2_cmd(0x01));
    pc += 4;

    // MFC2 t1, SXYP (idx 15) ; NOP ; SW t1, 0(t0)
    write_u32_le(mem, pc, enc_cop2_xfer(0x00, 9, 15));
    pc += 4;
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0)); // nop (load delay)
    pc += 4;
    write_u32_le(mem, pc, enc_i(0x2B, 8, 9, 0));
    pc += 4;

    // MFC2 t1, SZ3 (idx 19) ; NOP ; SW
    write_u32_le(mem, pc, enc_cop2_xfer(0x00, 9, 19));
    pc += 4;
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0));
    pc += 4;
    write_u32_le(mem, pc, enc_i(0x2B, 8, 9, 0));
    pc += 4;

    // AVSZ3 ; MFC2 OTZ ; NOP ; SW
    write_u32_le(mem, pc, enc_cop2_cmd(0x2D));
    pc += 4;
    write_u32_le(mem, pc, enc_cop2_xfer(0x00, 9, 7));
    pc += 4;
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0));
    pc += 4;
    write_u32_le(mem, pc, enc_i(0x2B, 8, 9, 0));
    pc += 4;

    // break
    write_u32_le(mem, pc, enc_r(0, 0, 0, 0, 0x0D));
    pc += 4;
}

static const char* arg_value(int argc, char** argv, const char* key_prefix)
{
    const size_t n = std::strlen(key_prefix);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key_prefix, n) == 0)
            return argv[i] + n;
    }
    return nullptr;
}

static int has_flag(int argc, char** argv, const char* flag)
{
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], flag) == 0)
            return 1;
    }
    return 0;
}

int main(int argc, char** argv)
{
    rlog::Logger logger{};
    rlog::logger_init(&logger, stdout);

    const char* lvl = arg_value(argc, argv, "--log-level=");
    if (lvl)
    {
        rlog::logger_set_level(&logger, rlog::parse_level(lvl));
    }

    const char* cats = arg_value(argc, argv, "--log-cats=");
    if (cats)
    {
        rlog::logger_set_cats(&logger, rlog::parse_categories_csv(cats));
    }

    const uint32_t kRamSize = 2u * 1024u * 1024u;
    uint8_t* ram = (uint8_t*)std::calloc(1, kRamSize);
    if (!ram)
    {
        std::fprintf(stderr, "Out of memory\n");
        return 1;
    }

    // ROM chargée à 0x00000000 (démo).
    const uint32_t reset_pc = 0x00000000u;
    const char* demo = arg_value(argc, argv, "--demo=");
    if (demo && std::strcmp(demo, "gte") == 0)
        build_gte_demo_rom(ram, reset_pc);
    else
        build_demo_rom(ram, reset_pc);

    r3000::Bus bus(ram, kRamSize, &logger);
    r3000::Cpu cpu(bus, &logger);
    cpu.reset(reset_pc);
    cpu.set_pretty(has_flag(argc, argv, "--pretty"));

    rlog::logger_logf(
        &logger, rlog::Level::info, rlog::Category::exec, "R3000 demo start (PC=0x%08X)", cpu.pc()
    );

    for (;;)
    {
        const auto res = cpu.step();
        if (res.kind == r3000::Cpu::StepResult::Kind::ok)
        {
            continue;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::halted)
        {
            rlog::logger_logf(
                &logger, rlog::Level::info, rlog::Category::exec, "HALT at PC=0x%08X", res.pc
            );
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::illegal_instr)
        {
            std::fprintf(stderr, "Illegal instruction at PC=0x%08X: 0x%08X\n", res.pc, res.instr);
            break;
        }

        if (res.kind == r3000::Cpu::StepResult::Kind::mem_fault)
        {
            std::fprintf(
                stderr,
                "Mem fault at PC=0x%08X addr=0x%08X kind=%d\n",
                res.pc,
                res.mem_fault.addr,
                (int)res.mem_fault.kind
            );
            break;
        }
    }

    std::free(ram);
    return 0;
}
