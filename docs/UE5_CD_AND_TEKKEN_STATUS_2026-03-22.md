# UE5 CD And Tekken Status

Date: 2026-03-22
Scope: UE5 live, Tekken (Europe), non-HLE, BIOS réel

## Goal
- Separate the older Tekken gameplay/loading issue from the newer CD boot regression.
- Record only what is known, what is inferred, and what was reverted.

## Previous Problem
- Last known better UE5 state in this session:
  - Sony logo OK
  - PlayStation/logo-license phase reached
  - Galaga playable
  - later stuck on `Now Loading`
  - still had a reduced number of `DMA3 MADR=0` errors

## Previous Problem: What Is Proven
- `FUN_8016A98C()` is the CD sync helper (`CD_datasync`).
- `FUN_8016AAE0(param_1, param_2)` is the routine that programs DMA3:
  - `MADR = param_1`
  - `BCR = param_2 | 0x10000`
  - `CHCR = 0x11000000`
- The bad Tekken path observed earlier is:
  - caller reaches `FUN_8016AAE0(a0=0, ...)`
  - then `DMA3.MADR=0`
  - then the guard blocks the transfer
- Runtime traces also showed a case where:
  - `dst_param != 0`
  - `dst_cur != 0`
  - `remain == 0`
  - and an extra DMA path still starts

## Previous Problem: What I Think
- The older Tekken `Now Loading` problem is most likely a generic CD flow / IRQ / read-completion issue.
- It does not currently look like a path problem or a raw data corruption problem.
- It also does not currently look like a `Pause`-handler regression in the active code, because the earlier `Pause` experiment was reverted.

## Current Problem
- Current regression is earlier than `Now Loading`.
- Current bad state observed after my CD changes:
  - no more BIOS no-disc loop
  - but boot stalls around Sony logo and no longer reaches the later license/PlayStation phase

## Current Problem: What Was Proven
- UE5/system logs explicitly showed:
  - `UE insert_disc begin ...`
  - `UE insert_disc FAILED ... err='could not open track file'`
- Additional logging then showed:
  - `errno=24`
  - `msg="Too many open files"`
  - failure occurred while opening track 10 from the Tekken `.cue`
- This means one concrete regression really did exist:
  - the old `.cue` path kept too many track files open under UE5 live

## Current Problem: Why That Was Not The Whole Story
- After changing the track-open strategy, boot behavior changed again:
  - disc insertion succeeded
  - sectors `LBA=4` and `LBA=5` were read correctly
  - but UE5 live dropped to roughly `55-58% speed` and `27-29 VBl/s`
  - boot then stalled around Sony logo
- So:
  - `errno=24` was real
  - but the first fix for it introduced a different regression

## Current Problem: What I Think
- The active suspicious area is not `Pause`.
- The active suspicious area is the track file backend in `src/cdrom/cdrom.cpp`:
  - opening all tracks at mount time was bad under UE5 live
  - opening/closing per-sector was also bad
  - keeping one track open at a time still did not restore the previous behavior
- That file-backend experiment has now been rolled back.
- After rollback, the early regression still remains.
- Therefore the active best hypothesis has shifted:
  - the remaining suspect is the CD continuous-read scheduling that feeds DMA3
  - not generic DMA itself
  - and not the earlier reverted `Pause` patch
- Latest UE5 log review adds one more concrete issue:
  - the CD command flow itself is still progressing
  - but the run is heavily slowed by high-volume CD polling logs during active reads
  - this pollutes the timing signal while diagnosing the Sony/logo regression
 - Latest DMA3 log review adds a stronger timing hypothesis:
   - after restoring continuous `ReadN`, the code was still using `10x fast` per-sector delivery
   - current logs show `ReadN advance -> LBA=...` bursts every few milliseconds
   - this can plausibly generate an extra sector-ready/DMA cycle before the game-side `Pause`/buffer accounting catches up
 - This is a generic CD timing issue, not a Tekken-only special case.
 - Additional observed side effect:
   - changing only the continuous sector timing also changes the visible BIOS/license behavior
   - user reported that PlayStation license text appears again in a way it did not before
   - therefore this timing path clearly participates in the Sony -> PlayStation transition and must be preserved in the debug history

## Active Current Suspect
- One functional delta versus the older better baseline was:
  - the removed `ReadN/ReadS continuous` requeue block in `src/cdrom/cdrom.cpp`
- That block used to:
  - queue the next `INT1`
  - keep the continuous CD read flow advancing
  - and therefore continue feeding DMA3
- Status update:
  - this block has now been restored as the minimal test to recover the previous UE5 baseline
  - no additional `Pause` / async / file-open logic was reintroduced with it

## What Was Reverted
- The earlier experimental changes around continuous-read scheduling were already reverted:
  - no special requeue on FIFO drain
  - no forced command priority over deferred async INT1
  - no Pause/Stop flush hack
- In this turn, the newer track-open strategy changes were rolled back to the previous CD baseline:
  - no on-demand one-track cache
  - no extra CUE FILE diagnostics in normal flow
  - no per-change fallback logic
- A version marker remains so rebuilds are visible in logs:
  - `CDROM source v13 (rollback_cd_baseline_2026_03_22)`

## Practical Conclusion
- There are two separate issues:
  1. Older real target issue: Tekken later stalls in loading / extra DMA path.
  2. Newer regression introduced during this session: CD boot behavior changed earlier.
- The immediate priority is to restore the previous UE5 baseline before resuming Tekken-specific correction.

## Recommended Next Step
- Confirm the rollback baseline in UE5 with:
  - `CDROM source v14`
  - whether boot returns to the earlier state:
    - Sony logo OK
    - PlayStation/logo-license phase OK
    - Galaga visible
    - later `Now Loading` stall
- Current immediate test:
  - verify whether restoring the removed `ReadN continuous` requeue block plus reducing CD polling log spam is enough to recover that baseline
  - then verify whether the previously documented compromise timing (`56000/112000`) removes the extra `DMA3 MADR=0` path before `Now Loading`
- Only after the older baseline is back should the later Tekken loading issue be debugged further.
## Current proven point

- Important correction:
  - the EXE callback globals sit at `0x801CB324/33C/334`
  - not `0x801DB324/33C/334`
- The current Tekken crash is not caused by the last valid CD DMA prep block.
- Proven by `TEKK_CDPREP_HIST`:
  - `0x8016A6DC` calls `FUN_8016AAE0` with valid destination `0x8011DAA0`
  - `0x8016A700` advances `dst_cur` to `0x8011E2A0`
  - `0x8016A710` sets CD `remain=0`
- The failing null DMA happens later in callback `0x8015D428`:
  - it reads `EXE_DST_PTR` from `0x801CB33C`
  - that pointer is still `0`
  - it then calls into the DMA wrapper with `a0=0`
- Therefore the immediate issue to debug is:
  - why `EXE_DST_PTR` is still zero when callback `0x8015D428` first runs
  - or why that callback runs before the expected init path

### Current timing correction under test

- `cdrom.cpp` `ReadN continuous` delay moved from the old fast compromise to physical cadence:
  - double-speed (`mode & 0x80`): `225,792` cycles/sector
  - single-speed: `451,584` cycles/sector
- Reason:
  - `cdrom.log` showed very fast `ReadN advance` bursts
  - `TEKK_EXEINIT` never ran before `0x8015D428`
  - `EXE_DST_PTR` was still `0` when the ready callback fired
- Important follow-up from UE5 live testing:
  - this timing change is not safe yet as a default
  - it regresses the game flow earlier, after the PlayStation license screen
  - in that regressed run, the later `DMA3 MADR=0` path no longer reproduces
  - this means the physical cadence is a valuable signal, but not yet a shippable baseline
