#pragma once

// ────────────────────────────────────────────────────────────────────────────
//  CPU investigation logs — compile-time gate
// ────────────────────────────────────────────────────────────────────────────
//
// Some diagnostic emu::logf calls in cpu.cpp target game-specific investigations
// (Tekken Stage67 stall, DMA3.MADR=0 reproduction, outer loader ldst=3 trace,
//  callback reuse-hold detection, etc.).  They're valuable while debugging but
//  spam the log at warn-level on hot paths once the investigation is closed.
//
// Wrap these sites in CPU_INV_LOGF(...) instead of emu::logf(...) so they can
// be turned OFF at compile time — same signature as emu::logf.
//
//  Default: OFF (0)       — matches "shipping quiet" config.
//  To enable investigation traces: build with -DR3000_CPU_INVESTIGATION_LOGS=1
//
// Mirror of the pattern used in PSX3DRenderComponent.cpp (R3000_GPU3D_WARNINGS_ONLY).
//
// Keep this header free of dependencies on emu_log.h — it's included from sites
// that already include emu_log.h.
// ────────────────────────────────────────────────────────────────────────────

#ifndef R3000_CPU_INVESTIGATION_LOGS
#define R3000_CPU_INVESTIGATION_LOGS 0
#endif

#if R3000_CPU_INVESTIGATION_LOGS
    #define CPU_INV_LOGF(...) ::emu::logf(__VA_ARGS__)
#else
    #define CPU_INV_LOGF(...) do {} while (0)
#endif
