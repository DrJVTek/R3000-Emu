# Tekken Generic Boot Debug (2026-03-14)

## Scope

This document is not a Tekken-specific workaround plan.

The goal is to isolate a generic boot-path failure in:

- BIOS exception flow
- IRQ/VBlank progression
- early CD boot handoff
- timer/root-counter semantics

No `psx3dprof` should be created for this investigation.

## Test Target

- Disc: `E:\Projects\PSX\roms\Tekken (Europe).cue`
- Detected game code: `SCES-000.05`
- BIOS: `Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin`
- Preferred mode: non-HLE (`bHleVectors=false`)

## Main Result

Tekken does not currently fail in PSX3D.

The earlier assumption "blocked before useful game flow" was too coarse.

With stronger stop/debug traces, the machine clearly reaches a game-side
VBlank/render loop:

- game-side IRQ dispatcher at `0x80067D48..0x80067E54`
- VBlank handler at `0x8006812C`
- repeated DMA2 linked-list submission and DMA6 OTC rebuilds
- repeated reads of `TMR1_COUNT`

So the current state is better described as:

- not stuck in a pure BIOS boot loop
- not blocked in a BIOS `WaitEvent` path
- already running game-side frame/update logic

The remaining generic problem is therefore narrower:

- timer/root-counter semantics
- GPU status / display semantics
- or another machine-state detail used by the game-side loop

## What Was Observed

### Non-HLE BIOS boot

Early execution repeatedly enters BIOS exception-loop code around:

- `0x00001E1C..0x00001E40`
- `0x00001F10..0x00001F48`

with states such as:

- `I_STAT=0x0001`
- `I_MASK=0x0075`

Later the system progresses far enough to show:

- `I_MASK 0x0075 -> 0x007D`
- `GP1 RESET`
- `GP1 DISPLAY ON`

but it still does not enter a useful game-loading flow.

### 60-second non-HLE run

During a long run:

- no useful `CDROM IRQ` activity was observed
- no visible `LBA` activity was observed
- no `DMA3/DMA4` activity was observed
- repeated `DMA2` GPU and `DMA6` OTC activity was observed

This strongly suggested that the machine reaches a display/update loop, but not
yet a clearly useful CD progression.

That remains true for CD activity, but it is now clear that the game is already
executing its own frame logic and not only BIOS code.

### Stop on `0x80067DE4`

Stopping on `0x80067DE4` gave:

- `I_STAT=0x00000001`
- `I_MASK=0x0000007D`
- only one allocated BIOS event in RAM:
  - `Event[0] cls=0xF0000009 spec=0x20 status=0 mode=0x2000`

This matters because it rules out a simple "BIOS event table stuck BUSY"
diagnosis for this point in the boot.

The surrounding instructions show a game-side IRQ dispatcher:

- read `I_STAT`
- read `I_MASK`
- compute masked pending bits
- acknowledge one bit in `I_STAT`
- dispatch to a per-bit handler table at `0x80114160`

So this is not a BIOS `WaitEvent` loop.

### Stop on `0x8006812C`

Stopping on `0x8006812C` shows the handler selected for interrupt bit 0
(`VBlank`).

The handler does two very simple things:

- increments a global counter at `0x8009ACFC`
- calls an optional callback from `0x801141C0` if non-null

At the stop point:

- `pending=0`
- callback pointer was null

So the game is servicing VBlank through its own dispatcher, not relying on the
BIOS event table there.

### 40-second trace with wider critical MMIO window

With the critical-MMIO trace window extended to 2048 entries, Tekken shows:

- repeated reads of `TMR1_COUNT` at:
  - `0x80067814`
  - `0x800678F4`
- steadily increasing `TMR1_COUNT` values:
  - `0x1BAA`
  - `0x1BB4`
  - `0x1BDB`
  - `0x1CF1`
  - `0x1D17`
  - `0x1E2E`
  - `0x1E54`
  - `0x1F6A`
  - `0x1F91`
  - `0x20A7`
- repeated GPU status reads (`GPU_GP1`)
- repeated DMA2 linked-list submissions with large OT lists
- repeated DMA6 OTC rebuilds

This is strong evidence that:

- the game is alive in a frame loop
- `TMR1` is actively used as a read-side timing source
- root-counter semantics remain a prime generic suspect

## False Leads Eliminated

### PSX3D / profile system

Not relevant for the current block.

The game fails before a dedicated 3D reconstruction strategy would matter.

### `0x80061CD0`

Stopping on `0x80061CD0` is misleading.

The surrounding code looks like SPU/voice initialization, not the generic stall:

- `s0 = 0x1F801C00` (SPU base)
- loop bound uses `0xF0`
- call chain is consistent with SPU setup

This is not the right place to fix the boot path.

### `0x80057320`

Stopping on `0x80057320` also does not isolate the root cause.

The surrounding code is arithmetic-heavy and appears to be game-side logic, not direct MMIO waiting.

It is better used as a symptom location than as the primary fix site.

## Strong Hypotheses

The remaining generic suspects are:

1. timer/root-counter semantics (`TMR1` is definitely read by game code)
2. GPU status / display semantics seen by the game-side loop
3. IRQ/VBlank progression inside the game-side dispatcher
4. CDROM interrupt edge or command progression later in the loop
5. bus-level bring-up hacks masking the real failure

## Existing Risky Bring-Up Hacks

Current code still contains generic hacks that may hide the true failure mode:

- `auto-enable I_MASK=0x0075`
- `RESCUE: BUSY -> READY`

These exist in:

- `src/r3000/bus.cpp`

They may help some titles boot, but they also make clean diagnosis harder.

## Important Existing Constraints

### Do not re-add manual `deliver_events_for_class()` for VBlank or CDROM

`docs/DEBUG_UE5_STUCK.md` already established that this caused regressions.

The BIOS exception path already delivers these events; doing it again causes double-delivery and corruption.

### Fast boot is not the debug target

`fast_boot_from_cd()` and `fast_boot_from_exe()` still force:

- `cpu_->set_hle_vectors(1);`

That is not the target path for this investigation.

The target is the real BIOS path in non-HLE.

## Logs Produced

- `logs/tekken_cli_nonhle_2026-03-14.log`
- `logs/tekken_cli_hle_2026-03-14.log`
- `logs/tekken_cli_nonhle_60s_2026-03-14.log`
- `logs/tekken_cli_nonhle_pcsample_2026-03-14.log`
- `logs/tekken_stop_80061CD0_2026-03-14.log`
- `logs/tekken_stop_80057320_2026-03-14.log`

## Working Conclusion

The current problem should be treated as a generic boot-path bug:

- not a Tekken-specific content bug
- not a PSX3D bug
- not a per-game profile problem

The next useful debugging step is to identify which MMIO-visible state the machine is no longer advancing correctly after the BIOS handoff.

## Next Actions

1. Validate `TMR1` semantics precisely:
   - HBlank cadence
   - reset-at-target behavior
   - 16-bit wrap behavior
   - compare count progression against expectations
2. Inspect the game-side loop around:
   - `0x80067814`
   - `0x800678F4`
   - `0x80067D48..0x80067E54`
3. Verify GPU status bits returned by `GPU_GP1`, since the loop reads them
   repeatedly alongside `TMR1_COUNT`.
4. Only after that, revisit CDROM progression if the render/timer loop still
   looks correct.

## Generic Fix Criteria

A valid fix must:

- work without a Tekken-specific profile
- work in non-HLE BIOS boot
- not depend on ad-hoc per-game patches
- improve the machine model for all titles using the same boot/IRQ/timer semantics

## Update: HBlank Timing Pass

### Generic fix applied

`src/r3000/bus.cpp` no longer uses a single hardcoded `2150`-cycle approximation for timer 1 external clock.

Timer 1 now derives its HBlank cadence from the active GPU video mode:

- PAL: `680688 cycles / 314 lines`
- NTSC: `571088 cycles / 263 lines`

This is implemented as a rational accumulator, so fractional line timing is preserved instead of truncating to a single integer divisor.

### Result

This fix is real and generic, but it did **not** qualitatively unblock Tekken:

- the game still reaches the same game-side IRQ/VBlank loop
- it still services VBlank through `0x80067D48..0x80067E54`
- it still reads `TMR1_COUNT` repeatedly
- it does not transition into an obviously different boot phase

So the PAL/NTSC HBlank bug was worth fixing, but it is not sufficient by itself.

### Important refinement around `0x80067814`

The stop-on-PC investigation around `0x80067814` is consistent with:

- a `GPU_GP1` read immediately before the timer sample
- then a read of the current timer-backed counter
- then a subtraction against a previously stored counter value
- then a `& 0xFFFF` delta path

This strongly suggests that Tekken is using timer 1 as a 16-bit free-running delta-time source.

That means the next generic suspects are now narrower:

1. exact 16-bit `TMR1` semantics
2. precise update/wrap behavior of the timer-backed count
3. game-visible `GPUSTAT` semantics only if they affect the same loop materially

At this point, PSX3D and per-game profile work remain out of scope for the Tekken boot issue.

## Update: 16-bit target period fix

### Generic fix applied

`src/r3000/bus.cpp` timer logic no longer shortens reset-at-target counters by one tick:

- `reset_at_target` now wraps with `target + 1`, not `target`
- 16-bit overflow now trips on `> 0xFFFF`, not `>= 0xFFFF`

This matters directly for Tekken because it programs:

- `TMR1_TARGET = 0xFFFF`
- `TMR1_MODE = 0x0148`

With the old logic, that path effectively behaved like a `65535`-tick period instead of a true `65536`-tick 16-bit cycle.

### Result

This also did **not** qualitatively unblock Tekken.

Tekken still stops in the same game-side timing path around `0x80067814`.

### New stop-on-PC detail

At `0x80067814`, the important live values are now:

- `t6 = 0x1F801110` → timer register base for `TMR1_COUNT`
- `t6[0] = 0x00001B70` at stop
- `t8` is loaded from `0x8009ABE4` just before the timer delta is computed

This reinforces the current interpretation:

- the loop is consuming `TMR1` as a 16-bit timing source
- but there is still a problem in the game-visible timing path beyond the two obvious generic timer bugs already fixed

The next debug target is therefore not PSX3D and not CD boot policy.

It is:

1. understanding how the game initializes and updates the previous timer sample at `0x8009ABE4`
2. checking whether the current `TMR1_COUNT` read semantics still differ from what the game expects

## Update 2026-03-15 - GPUSTAT bit31 480i

Findings:
- Tekken uses GPUSTAT in 480i mode (`bit19=1`, `bit22=1`).
- Our previous `bit31` implementation used `(display_y + current_scanline) & 1` for all modes.
- DuckStation uses a special 480i path where `display_line_lsb` depends on the displayed field and suppresses the field contribution during VBlank.

Change made:
- `src/gpu/gpu.cpp`
- `GPUSTAT bit31` now uses a 480i-specific formula when `display_.interlace && display_.v_res`.

Observed effect:
- Tekken no longer spins in the tight inner poll loop `0x800678C4..0x800678D8`.
- Trace now repeatedly exits through `0x800678DC -> 0x80067910`.
- Example trace file:
  - `logs/tekken_regtrace_loop_480i_bit31fix_2026-03-15.log`
- Non-regression:
  - `logs/ridge_480i_bit31fix_2026-03-15.log`

Current status:
- This is a generic hardware fix, not a per-game workaround.
- Tekken still has no proven CDROM progression in the short CLI window, but the previous GPUSTAT/TMR1 wait pathology is no longer the dominant blocker.
- Next targets should be:
  1. verify whether CDROM command traffic starts later once this loop is gone,
  2. inspect remaining BIOS/IRQ boot hacks (`auto I_MASK`, rescue paths),
  3. compare any remaining GPUSTAT semantic differences only if a new wait loop appears.
