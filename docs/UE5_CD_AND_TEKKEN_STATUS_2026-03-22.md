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
- Only after the older baseline is back should the later Tekken loading issue be debugged further.
