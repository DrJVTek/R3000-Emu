#pragma once

#include <cstdint>
#include "gte_snapshot.h"

// IGte: Pure interface for all GTE (COP2) implementations.
//
// Exposes the FULL GTE API:
//   - COP2 register access (data[0..31], ctrl[0..31])
//   - COP2 load/store (LWC2/SWC2)
//   - Command dispatch (execute)
//   - All 22 individual GTE commands (RTPS, RTPT, MVMVA, NCLIP, etc.)
//   - 3D reconstruction snapshot
//
// Implementations:
//   - Gte          : faithful PS1 GTE (powers 2D rendering, CPU reads from here)
//   - Gte3D        : shadow GTE for 3D reconstruction (separate codebase, can diverge)
//   - Gte3DTagged  : shadow GTE + polygon index tagging in SXY high bits

namespace gte
{

class IGte
{
  public:
    virtual ~IGte() = default;

    virtual void reset() = 0;

    // ── COP2 register access ─────────────────────────────────────────
    virtual uint32_t read_data(uint32_t idx) const = 0;
    virtual void write_data(uint32_t idx, uint32_t v) = 0;

    virtual uint32_t read_ctrl(uint32_t idx) const = 0;
    virtual void write_ctrl(uint32_t idx, uint32_t v) = 0;

    // ── COP2 load/store ──────────────────────────────────────────────
    virtual void lwc2(uint32_t gte_reg, uint32_t word) = 0;
    virtual uint32_t swc2(uint32_t gte_reg) const = 0;

    // ── Command dispatch ─────────────────────────────────────────────
    // Decode + dispatch COP2 CO instruction. Returns cycle count (>0) or 0 if unknown.
    virtual int execute(uint32_t cop2_instruction) = 0;

    // ── GTE commands (all 22 COP2 CO functions) ──────────────────────
    // Geometry / projection
    virtual void cmd_rtps(uint32_t cmd) = 0;   // 0x01 — Perspective transform (single vertex)
    virtual void cmd_rtpt(uint32_t cmd) = 0;   // 0x30 — Perspective transform (3 vertices)
    virtual void cmd_mvmva(uint32_t cmd) = 0;  // 0x12 — Matrix-vector multiply + add
    virtual void cmd_nclip(uint32_t cmd) = 0;  // 0x06 — Normal clipping (winding order)
    virtual void cmd_avsz3(uint32_t cmd) = 0;  // 0x2D — Average Z (3 values)
    virtual void cmd_avsz4(uint32_t cmd) = 0;  // 0x2E — Average Z (4 values)
    virtual void cmd_sqr(uint32_t cmd) = 0;    // 0x28 — Square of vector
    virtual void cmd_op(uint32_t cmd) = 0;     // 0x0C — Outer product (cross product)

    // Lighting / color
    virtual void cmd_ncds(uint32_t cmd) = 0;   // 0x13 — Normal color depth single
    virtual void cmd_ncdt(uint32_t cmd) = 0;   // 0x16 — Normal color depth triple
    virtual void cmd_nccs(uint32_t cmd) = 0;   // 0x1B — Normal color color single
    virtual void cmd_ncct(uint32_t cmd) = 0;   // 0x3F — Normal color color triple
    virtual void cmd_ncs(uint32_t cmd) = 0;    // 0x1E — Normal color single
    virtual void cmd_nct(uint32_t cmd) = 0;    // 0x20 — Normal color triple
    virtual void cmd_cc(uint32_t cmd) = 0;     // 0x1C — Color color
    virtual void cmd_cdp(uint32_t cmd) = 0;    // 0x14 — Color depth cue

    // Depth cueing / interpolation
    virtual void cmd_dpcs(uint32_t cmd) = 0;   // 0x10 — Depth cue single
    virtual void cmd_dpct(uint32_t cmd) = 0;   // 0x2A — Depth cue triple
    virtual void cmd_dcpl(uint32_t cmd) = 0;   // 0x29 — Depth cue color light
    virtual void cmd_intpl(uint32_t cmd) = 0;  // 0x11 — Interpolation

    // General purpose
    virtual void cmd_gpf(uint32_t cmd) = 0;    // 0x3D — General purpose interpolation
    virtual void cmd_gpl(uint32_t cmd) = 0;    // 0x3E — General purpose interpolation + base

    // ── 3D reconstruction ────────────────────────────────────────────
    virtual const GteSnapshot& last_snapshot() const = 0;
};

} // namespace gte
