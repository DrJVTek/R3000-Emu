// ────────────────────────────────────────────────────────────────────────────
//  cpu_dasm.cpp — pretty-print disassembler extracted from Cpu::step()
// ────────────────────────────────────────────────────────────────────────────
//
// Emits a human-readable trace line per instruction when pretty_ is on.
// Debug-only code path — never runs in shipping; zero cost otherwise.
//
// Previously inline in step() with 19 captured locals (pc, instr, opcode,
// wb_* / mem_* / ld_* / wb2_*).  Now driven by a Cpu::DasmContext struct
// populated by step() before calling Cpu::emit_dasm_line(ctx).
//
// Portability note: this file replaces the 4 MSVC-only
// ::strncat_s(..., _TRUNCATE) calls with a small portable append helper
// using std::snprintf.  No behavioural change — same truncation semantics.
// Targets: Quest 3 (Android ARM64), Windows desktop, Linux, macOS, visionOS.
// ────────────────────────────────────────────────────────────────────────────

#include "cpu.h"

#include <cstdio>
#include <cstring>

#include "../log/emu_log.h"
#include "cpu_helpers.h"   // reg_name

namespace r3000
{

// Portable replacement for MSVC-only ::strncat_s(dst, cap, src, _TRUNCATE).
// Appends `src` to the null-terminated string `dst` of capacity `cap`,
// truncating silently if the result would overflow (same semantics as
// _TRUNCATE).  Always leaves `dst` null-terminated within cap.
static inline void dasm_append(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0 || !src)
        return;
    const size_t cur = std::strlen(dst);
    if (cur + 1 >= cap)
        return;
    // snprintf truncates and always null-terminates when cap > 0.
    std::snprintf(dst + cur, cap - cur, "%s", src);
}

void Cpu::emit_dasm_line(const DasmContext& ctx) const
{
    // Mode lisible "désassemblage":
    // On reconstruit une string à la volée pour l'affichage live.
    // Important: ce n'est PAS un désassembleur complet, juste les instructions qu'on supporte.
    char line[256];
    line[0] = '\0';

    // Désassemblage minimal pour les opcodes supportés.
    const uint32_t o = ctx.opcode;
    const uint32_t instr = ctx.instr;
    if (o == 0x0F)
    {
        std::snprintf(
            line,
            sizeof(line),
            "PC=%08X  LUI  %s, 0x%04X",
            ctx.pc,
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
            ctx.pc,
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
            ctx.pc,
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
            ctx.pc,
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
            ctx.pc,
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
            ctx.pc,
            reg_name(rt(instr)),
            (int)(int16_t)imm_s(instr),
            reg_name(rs(instr))
        );
    }
    else if (o == 0x05)
    {
        const int16_t off = (int16_t)imm_s(instr);
        const uint32_t target = (ctx.pc + 4) + ((uint32_t)((int32_t)off << 2));
        std::snprintf(
            line,
            sizeof(line),
            "PC=%08X  BNE  %s, %s, 0x%08X",
            ctx.pc,
            reg_name(rs(instr)),
            reg_name(rt(instr)),
            target
        );
    }
    else if (o == 0x04)
    {
        const int16_t off = (int16_t)imm_s(instr);
        const uint32_t target = (ctx.pc + 4) + ((uint32_t)((int32_t)off << 2));
        std::snprintf(
            line,
            sizeof(line),
            "PC=%08X  BEQ  %s, %s, 0x%08X",
            ctx.pc,
            reg_name(rs(instr)),
            reg_name(rt(instr)),
            target
        );
    }
    else if (o == 0x02)
    {
        const uint32_t target = ((ctx.pc + 4) & 0xF000'0000u) | (jidx(instr) << 2);
        std::snprintf(line, sizeof(line), "PC=%08X  J    0x%08X", ctx.pc, target);
    }
    else if (o == 0x00 && funct(instr) == 0x08)
    {
        std::snprintf(line, sizeof(line), "PC=%08X  JR   %s", ctx.pc, reg_name(rs(instr)));
    }
    else if (o == 0x00 && funct(instr) == 0x00)
    {
        std::snprintf(
            line,
            sizeof(line),
            "PC=%08X  SLL  %s, %s, %u",
            ctx.pc,
            reg_name(rd(instr)),
            reg_name(rt(instr)),
            shamt(instr)
        );
    }
    else if (o == 0x00 && funct(instr) == 0x0D)
    {
        std::snprintf(line, sizeof(line), "PC=%08X  BREAK", ctx.pc);
    }
    else
    {
        std::snprintf(line, sizeof(line), "PC=%08X  INSTR 0x%08X", ctx.pc, instr);
    }

    if (ctx.wb_valid)
    {
        char tmp[128];
        std::snprintf(
            tmp, sizeof(tmp), "  ; %s:0x%08X->0x%08X", reg_name(ctx.wb_reg), ctx.wb_old, ctx.wb_new
        );
        dasm_append(line, sizeof(line), tmp);
    }

    if (ctx.mem_valid)
    {
        char tmp[128];
        std::snprintf(tmp, sizeof(tmp), "  ; %s [0x%08X]=0x%08X",
                      ctx.mem_op ? ctx.mem_op : "?",
                      ctx.mem_addr, ctx.mem_val);
        dasm_append(line, sizeof(line), tmp);
    }

    if (ctx.ld_valid)
    {
        char tmp[128];
        std::snprintf(
            tmp, sizeof(tmp), "  ; (LD sched) %s -> %s=0x%08X",
            ctx.ld_op ? ctx.ld_op : "?",
            reg_name(ctx.ld_reg), ctx.ld_val
        );
        dasm_append(line, sizeof(line), tmp);
    }

    if (ctx.wb2_valid)
    {
        char tmp[128];
        std::snprintf(
            tmp,
            sizeof(tmp),
            "  ; (LD commit) %s:0x%08X->0x%08X",
            reg_name(ctx.wb2_reg),
            ctx.wb2_old,
            ctx.wb2_new
        );
        dasm_append(line, sizeof(line), tmp);
    }

    emu::logf(emu::LogLevel::debug, "CPU_DASM", "%s", line);
}

} // namespace r3000
