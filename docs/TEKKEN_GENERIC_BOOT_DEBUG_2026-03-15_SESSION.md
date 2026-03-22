# Debug Session 2026-03-15/16 — Tekken Boot + Generic Fixes

## Session Overview

Long debug session targeting Tekken (Europe) boot on non-HLE BIOS (SCPH-7502).
Multiple generic emulator bugs found and fixed. MDEC decoder implemented.
Some regressions introduced — documented below for next session.

---

## Bug 1: I_MASK Auto-Enable Too Early (FIXED — CLI only)

### Symptom
- CPU stuck at 0x80000080/0x80000084 (infinite exception loop)
- Happens in CLI non-HLE mode

### Root Cause
`kForceMaskAfterCycles = 600000` (~0.018s PS1 time) forced `I_MASK = 0x0075` before
the first VBlank, before the BIOS had installed SysEnqIntRP chain handlers. When VBlank
fired, the BIOS exception handler had no chain to dispatch → I_STAT never acknowledged
→ RFE → IRQ re-fires → infinite loop.

### Fix
```cpp
// bus.cpp — increased from 600000 to 30 VBlank periods
static constexpr uint32_t kForceMaskAfterCycles = 30u * 680688u; // ~20M cycles
```

### Impact
- **CLI non-HLE**: Fixed. BIOS boots, CD loads, game runs.
- **UE5**: Was already working before (different timing path). No change.

---

## Bug 2: GPUSTAT Bit 31 — 480i Fix (FIXED, REGRESSION on non-480i, PARTIALLY REVERTED)

### Symptom
Tekken uses 480i mode and polls GPUSTAT bit31 with HBlank timers. Old code used a
simple per-frame toggle which didn't match real hardware for interlaced modes.

### Fix Applied
Added 480i-specific path that uses displayed field + VBlank suppression.

### REGRESSION INTRODUCED
Initially the scanline-parity code was applied to ALL modes. This broke Ridge Racer's
VSync detection → **GPU 3D rendering completely broken** (0 triangles).

### Partial Revert
Reverted non-480i to simple `even_odd_field_` toggle:
```cpp
if (display_.interlace && display_.v_res) {
    // 480i: field-based with VBlank suppression (Tekken)
} else {
    // Non-480i: simple per-frame toggle (Ridge Racer etc.)
    display_line_lsb = even_odd_field_ ? 1u : 0u;
}
```

### CURRENT STATUS: ⚠️ NEEDS VERIFICATION
User reports GPU 3D still broken after revert. Possible causes:
1. Live Coding didn't pick up the revert (user confirmed they compiled — unclear)
2. The `bMdecStub` UPROPERTY added to `R3000EmuComponent.h` may have shifted
   the serialized Blueprint property layout, corrupting existing component values
3. Other timer changes (target+1, overflow check, timer_check_irq on writes) may
   affect the VSync/frame timing that the 3D correlation system depends on

### TODO NEXT SESSION
- Verify GPU 3D works in CLI first (no UE5 variable)
- If CLI works → UE5 Blueprint property corruption: re-create the component
- If CLI broken → bisect timer changes vs GPUSTAT changes

---

## Bug 3: CD Timing Too Fast (PARTIALLY FIXED)

### Symptom
CD sector reads were 10x faster than real PS1. Loading screens (e.g. Tekken's Galaga
mini-game) lasted < 1 second instead of 30-60 seconds.

### Fix
Continuous sector read delay increased from ~0.3ms to ~1.7ms per sector (~4x real speed
instead of ~10x). Seek/spin-up timing kept at 10x fast for BIOS compatibility.

```cpp
// Continuous read: was 11000/22000, now 56000/112000 cycles
pending_irq_delay_ = (mode_ & 0x80u) ? 56000u : 112000u;
```

### Note on Failed Attempts
- **1x realistic timing**: BIOS boot stalled (CDROM responses too slow, BIOS timeout)
- **3x timing**: Only 2 ReadN in 60s — too slow for BIOS CD init
- **5x timing**: INT1 delivery failures — seek delays too long
- **4x continuous + 10x seek**: Current compromise, works

---

## Bug 4: DMA3 MADR=0 Kernel Corruption (GUARD ADDED, root cause UNKNOWN)

### Symptom
Game code at `PC=0x8016AB78` writes `DMA3.MADR = 0x00000000`. DMA3 (CDROM→RAM) then
writes a full 2048-byte sector to physical address 0x00000000, overwriting the BIOS
exception handler, API vectors, and SysEnqIntRP chain heads.

Next IRQ → RI exception (code=10) at `EPC=0x000000EC` → infinite RI loop.

### Evidence (from UE5 r3000_core.log)
```
code=0  EPC=0x8016D690   ← last normal IRQ from game code
code=10 EPC=0x000000EC   ← RI! CPU jumped to corrupted kernel area
code=10 EPC=0x80000080   ← handler also corrupted → infinite loop
```

### Guard
```cpp
if (ma < 0x200u) {
    emu::logf(emu::LogLevel::error, "BUS",
        "DMA3 BLOCKED: madr=0x%05X words=%u", ma, words);
    dma_finish(ch);
    break;
}
```

### Root Cause Analysis
- Happens in BOTH CLI and UE5 (confirmed)
- Happens WITH and WITHOUT real MDEC (MDEC does not fix this)
- Game code at `0x8016AB78` reads a buffer pointer that is 0
- The pointer should be set by an upstream init that we don't emulate correctly
- Probably related to CDROM sector loading state machine in game code

### Ghidra Addresses to Investigate
| Address | Description |
|---------|-------------|
| `0x8016AB78` | Writes DMA3.MADR = 0 (reads null buffer pointer) |
| `0x8016CE7C` | Caller that jumps to uninitialized function pointer |
| `0x80165A7C` | `MDEC_in_sync()` — polls DMA_MDEC_IN_CHCR bit 24 |
| `0x8016D640-0x8016D6A4` | Game main loop (Galaga/FMV transition area) |

---

## Feature: MDEC Decoder (IMPLEMENTED)

### Files
- `src/mdec/mdec.h` — Interface
- `src/mdec/mdec.cpp` — Implementation (~400 lines)
- Added to `CMakeLists.txt` CORE_SOURCES
- UE5: auto-discovered via `Private/src` symlink

### Architecture
- 3 commands: DecodeMacroblock (cmd=1), SetQuantTable (cmd=2), SetScaleTable (cmd=3)
- Pipeline: RLE decode → inverse quantize → IDCT 8×8 → YUV→RGB
- 6 blocks per macroblock: Cr, Cb, Y1-Y4 (16×16 color pixels)
- Output formats: 4-bit, 8-bit, 24-bit RGB, 15-bit RGB
- DMA0 (MDEC IN): reads compressed data from RAM → feeds decoder
- DMA1 (MDEC OUT): writes decoded pixels from decoder → RAM

### Integration
- Bus: MMIO read/write at 0x1F801820 (data) / 0x1F801824 (status/control)
- DMA0/DMA1: real data transfer through `mdec_->dma_write()` / `mdec_->dma_read()`
- Core: creates `mdec::Mdec` instance in `init_from_image()`, passes to Bus
- DMA completion: clears CHCR busy bit WITHOUT triggering DMA IRQ (avoids phantom IRQ)

### NOT YET TESTED
The MDEC decodes data but we haven't verified the output is correct (no visual test).
Tekken still crashes due to DMA3 MADR=0 (separate bug) before MDEC output is visible.

---

## Timer Fixes (FIXED — may cause regression)

### Changes
1. **16-bit target period**: `t.count %= (t.target + 1)` instead of `t.count %= t.target`
2. **Overflow check**: `t.count > 0xFFFF` instead of `>= 0xFFFF`
3. **IRQ check on mode write**: Added `timer_check_irq(ch, t.count)` in `timer_write_mode()`
4. **IRQ check on count/target writes**: Added `timer_check_irq()` calls

### ⚠️ REGRESSION RISK
These changes fire `timer_check_irq` on every timer register write. This may cause
spurious timer IRQs that disrupt game timing. If GPU 3D is broken even after the
GPUSTAT bit31 revert, these timer changes should be investigated.

---

## HBlank Timer Phase-Lock (FIXED)

Timer 1 (HBlank source) now derives its increment from actual GPU scanline
progression instead of a hardcoded cycle approximation:
```cpp
inc = gpu_scanline_delta; // phase-locked to GPU scanline counter
```

---

## Stall Report Address Correction

The original stall report identified the VBlank counter at `0x8009ACFC`.
This was incorrect — MIPS sign-extension makes the actual address `0x8008ACFC`:
```
LUI 0x8009 + offset 0xACFC (sign-extended = -0x5304) = 0x8008ACFC
```
With the correct address, the VBlank counter increments normally (1/frame, confirmed).

---

## Current State Summary

| Component | CLI | UE5 | Notes |
|-----------|-----|------|-------|
| BIOS boot (non-HLE) | ✅ | ✅ | I_MASK auto-enable fixed |
| CD timing | ✅ | ✅ | 4x fast continuous, 10x seek |
| MDEC decoder | ✅ Built | ✅ Built | Not visually tested yet |
| DMA3 kernel guard | ✅ | ✅ | Prevents crash, root cause unknown |
| GPUSTAT bit31 480i | ✅ | ? | Tekken-specific path |
| Timer fixes | ✅ | ? | May cause regression |
| Ridge Racer 3D | ? | ⚠️ BROKEN | Needs investigation (see below) |
| Tekken | Partial | Partial | Galaga runs, crashes at FMV transition |
| Moto Racer | ✅ No crash | ? | Needs UE5 testing |

---

## ⚠️ KNOWN REGRESSION: GPU 3D Rendering Broken in UE5

### Symptom
- No 3D triangles rendered (0 triangles in draw list)
- PlayStation logo gone, Ridge Racer 3D gone
- Was working before this session's changes

### Possible Causes (in order of likelihood)

1. **Blueprint property shift**: Adding `bMdecStub` UPROPERTY to `R3000EmuComponent.h`
   before existing properties may have corrupted the serialized Blueprint values.
   **Fix**: Re-create the R3000EmuComponent in the Blueprint, or delete + re-add it.

2. **GPUSTAT bit31 revert incomplete**: The revert restored `even_odd_field_` for
   non-480i but the scanline variables (`current_scanline`, `period`, `total_lines`)
   are still computed. These are harmless but verify the final `display_line_lsb`
   value matches the old code path.

3. **Timer IRQ spam**: The new `timer_check_irq()` calls on count/target writes may
   cause extra IRQs that break the game's VSync loop timing, preventing draw list
   submission.

### Debug Plan for Next Session
1. Test Ridge Racer in **CLI** with `--3d-diag` to check if 3D works outside UE5
2. If CLI 3D works → UE5 Blueprint corruption, re-create component
3. If CLI 3D broken → `git stash` all changes, verify 3D works, then bisect

---

## Files Modified This Session

| File | Changes |
|------|---------|
| `src/r3000/bus.cpp` | I_MASK threshold, MDEC registers, DMA0/1/3 guards, timer fixes, HBlank phase-lock, diagnostic logging |
| `src/r3000/bus.h` | MDEC pointer/stub fields |
| `src/gpu/gpu.cpp` | GPUSTAT bit31 480i, `current_scanline()`, `total_scanlines()` |
| `src/gpu/gpu.h` | (if changed — verify) |
| `src/emu/core.cpp` | MDEC creation, mdec_stub option |
| `src/emu/core.h` | MDEC member, mdec_stub InitOption |
| `src/cdrom/cdrom.cpp` | CD timing (seek, spin-up, continuous read delays) |
| `src/r3000/cpu.cpp` | FAULT diagnostic logging |
| `src/mdec/mdec.h` | NEW — MDEC decoder interface |
| `src/mdec/mdec.cpp` | NEW — MDEC decoder implementation |
| `CMakeLists.txt` | Added mdec.cpp to CORE_SOURCES |
| `cli/main.cpp` | `--mdec-stub` flag |
| `integrations/ue5/.../R3000EmuComponent.h` | `bMdecStub` property, `GetProgramCounterString()` |
| `integrations/ue5/.../R3000EmuComponent.cpp` | mdec_stub wiring in all 3 init paths |

---

## Next Session Priority

1. **Fix GPU 3D regression** (highest priority — this breaks Ridge Racer which was working)
2. Investigate DMA3 MADR=0 root cause in Ghidra
3. Test Moto Racer in UE5
4. Visual test MDEC output (once Tekken gets past the crash)
5. Clean up diagnostic logging
