#pragma once

// ────────────────────────────────────────────────────────────────────────────
//  cpu_helpers.h — file-static helpers extracted from cpu.cpp
// ────────────────────────────────────────────────────────────────────────────
//
// Pure decoding / formatting / text-routing utilities that used to live as
// `static` functions at the top of cpu.cpp.  Moving them here shrinks cpu.cpp
// and lets them be reused by upcoming cpu_debug.cpp / hle_bios.cpp without
// re-declaring.
//
// All functions are `inline` (implicitly internal linkage when also tagged
// `static`, otherwise linker-merged across TUs).  No dependency on Cpu
// internal state.
//
// NOTE: The `text_flush_line` / `text_push_char` helpers take a flog::Sink by
// reference, so this header pulls in filelog.h and emu_log.h transitively.
// Keep this header free of any include of "cpu.h" to avoid a cycle.
// ────────────────────────────────────────────────────────────────────────────

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../log/emu_log.h"
#include "../log/filelog.h"

namespace r3000
{

// ── GTE opcode name / structural-op classification ─────────────────────────
inline const char* gte_func_name(uint32_t funct)
{
    switch (funct & 0x3Fu)
    {
    case 0x01: return "RTPS";
    case 0x06: return "NCLIP";
    case 0x0C: return "OP";
    case 0x10: return "DPCS";
    case 0x11: return "INTPL";
    case 0x12: return "MVMVA";
    case 0x13: return "NCDS";
    case 0x14: return "CDP";
    case 0x16: return "NCDT";
    case 0x1B: return "NCCS";
    case 0x1C: return "CC";
    case 0x1E: return "NCS";
    case 0x20: return "NCT";
    case 0x28: return "SQR";
    case 0x29: return "DCPL";
    case 0x2A: return "DPCT";
    case 0x2D: return "AVSZ3";
    case 0x2E: return "AVSZ4";
    case 0x30: return "RTPT";
    case 0x3D: return "GPF";
    case 0x3E: return "GPL";
    case 0x3F: return "NCCT";
    default:   return "UNKNOWN";
    }
}

inline int gte_func_is_structural(uint32_t funct)
{
    switch (funct & 0x3Fu)
    {
    case 0x01: // RTPS
    case 0x06: // NCLIP
    case 0x1E: // NCS
    case 0x20: // NCT
    case 0x2E: // AVSZ4
    case 0x30: // RTPT
        return 1;
    default:
        return 0;
    }
}

// Top-N histogram dump used by frame-level GTE diagnostics.
// Keeps template instantiation in header since it's used from cpu.cpp only.
template<typename K>
inline void gte_trace_dump_top(const char* tag, uint32_t frame,
                               const std::unordered_map<K, uint32_t>& hist,
                               const char* (*name_fn)(uint32_t) = nullptr)
{
    if (hist.empty())
        return;
    std::vector<std::pair<K, uint32_t>> items(hist.begin(), hist.end());
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    const size_t topn = std::min<size_t>(items.size(), 8);
    for (size_t i = 0; i < topn; ++i)
    {
        if (name_fn)
        {
            emu::logf(emu::LogLevel::debug, tag, "frame=%u top[%zu] op=%s(0x%02X) count=%u",
                frame, i, name_fn((uint32_t)items[i].first), (uint32_t)items[i].first, items[i].second);
        }
        else
        {
            emu::logf(emu::LogLevel::debug, tag, "frame=%u top[%zu] pc=0x%08X count=%u",
                frame, i, (uint32_t)items[i].first, items[i].second);
        }
    }
}

// ── Camera-tracking address predicate ──────────────────────────────────────
inline int is_camera_track_addr(uint32_t paddr, uint32_t ram_size)
{
    if (paddr < ram_size)
        return 1; // Main RAM
    if (paddr >= 0x1F800000u && paddr < 0x1F801000u)
        return 1; // Scratchpad (often used for matrix staging)
    return 0;
}

// ── ASCII classification + line-buffered text routing ──────────────────────
inline int is_printable_ascii(uint8_t b)
{
    if (b == '\t' || b == '\r' || b == '\n')
        return 1;
    return (b >= 0x20 && b <= 0x7Eu) ? 1 : 0;
}

inline void text_flush_line(flog::Sink& s, const flog::Clock& c, int has_clock,
                            char* buf, uint32_t& pos)
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

inline void text_push_char(flog::Sink& s, const flog::Clock& c, int has_clock,
                           char* buf, uint32_t cap, uint32_t& pos, uint8_t ch)
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

// ── MIPS ABI register names ────────────────────────────────────────────────
inline const char* reg_name(uint32_t idx)
{
    // Noms "ABI" MIPS pour que le trace soit lisible en live.
    // Exemple: t0/t1/t2 = temporaires, a0..a3 = arguments, sp = stack pointer, ra = return address.
    // NOTE: l'ABI est un "convention de nommage". Le CPU, lui, ne connait que 32 registres GPR.
    static const char* k[32] = {"r0", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                "t3", "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                "s6", "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
    return k[idx & 31u];
}

// ── PSX MMIO naming + classification ───────────────────────────────────────
inline const char* psx_mmio_name(uint32_t phys_addr)
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

inline int psx_is_mmio(uint32_t phys_addr)
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

inline int psx_is_critical_mmio(uint32_t phys_addr)
{
    switch (phys_addr)
    {
        case 0x1F801070u: // I_STAT
        case 0x1F801074u: // I_MASK
        case 0x1F8010F0u: // DPCR
        case 0x1F8010F4u: // DICR
        case 0x1F801800u: // CDROM idx/stat
        case 0x1F801801u: // CDROM cmd
        case 0x1F801802u: // CDROM param
        case 0x1F801803u: // CDROM resp/data
        case 0x1F801810u: // GPU GP0
        case 0x1F801814u: // GPU GP1
            return 1;
        default:
            break;
    }

    if (phys_addr >= 0x1F801100u && phys_addr < 0x1F801130u)
        return 1; // TMR0/1/2 count/mode/target

    return 0;
}

} // namespace r3000
