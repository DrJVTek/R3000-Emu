# CODEX Recursive Debug Status

Date: 2026-03-22
Scope: UE5 live + CLI, Tekken non-HLE, BIOS réel

## Rule
- Update this file at the end of each debugging session.
- Keep only factual state:
  - current symptom
  - last known good state
  - active code changes
  - reverted changes
  - next step

## Current Symptom
- Current UE5 regression is earlier than the old Tekken `Now Loading` issue.
- Current bad state after Sony logo:
  - disc insert is OK
  - boot does not reach the old later baseline (`license` / PlayStation logo / Galaga)
  - practical symptom: stuck around Sony logo / early BIOS CD flow

## Current Proven Cause
- The `.cue` is valid.
- All referenced `Tekken (Europe) (Track NN).bin` files exist on disk.
- CLI can open the same `.cue`.
- UE5/live previously showed a real mount-time failure:
  - `errno=24`
  - `msg="Too many open files"`
  - while opening track 10
- That specific file-open experiment has now been rolled back.
- After rollback, the early Sony-logo regression still remains.
- Therefore the active current bug is not explained anymore by the file-open experiment alone.

## Current Best Hypothesis
- The active suspect is not `Pause` itself.
- The active suspect is the CD continuous-read scheduling that feeds DMA3.
- The strongest functional delta versus the older baseline was:
  - the removal of the `ReadN/ReadS continuous` requeue block in `src/cdrom/cdrom.cpp`
  - this block used to queue the next `INT1` / sector-advance after ACK while continuous reading remained active
- Practical interpretation:
  - the likely regression is not "generic DMA engine broken"
  - it is more likely "CD sector scheduling feeding DMA3 changed"
- Status update:
  - this block has now been restored as the minimal baseline-recovery patch
  - no other CD control-flow change was added in the same step
  - latest log review also shows the CD command flow does continue past `GetID/ReadTOC/SetLoc/SeekL/ReadN/Pause`
  - the remaining immediate problem in UE5 logs is that CD polling/data logs are still emitted at very high volume during reads, while runtime speed stays around `43-56%`

## Important Observation
- CLI does not reproduce the current UE5 early-boot behavior.
- CLI Tekken non-HLE with BIOS real sees the disc correctly:
  - `SYSTEM.CNF` found
  - region inferred `E / SCEE`
  - `disc inserted (files=28 tracks=28)`
- Therefore the current regression is not a generic ISO/core boot failure.

## Last Known Better State
- Better UE5 state reached earlier in this session:
  - PlayStation logo OK
  - Galaga playable
  - later stuck on `Now Loading`
  - still had a reduced number of `DMA3 MADR=0` errors

## Tekken Root Cause Work Already Proven
- `FUN_8016A98C()` = `CD_datasync`
- `FUN_8016AAE0(param_1, param_2)` programs DMA3:
  - `MADR = param_1`
  - `BCR = param_2 | 0x10000`
  - `CHCR = 0x11000000`
- The bad path is an extra DMA start where:
  - `a0 = 0`
  - while global CD state can still be valid
- Prior runtime trace showed:
  - `dst_param != 0`
  - `dst_cur != 0`
  - `remain == 0`
  - then a call still reaches `FUN_8016AAE0(a0=0, ...)`

## Active Changes Kept
- Core/MDEC/GPU fixes from this session:
  - removed fake `mdec_stub`
  - MDEC DMA completion goes through `dma_finish()`
  - GPUSTAT bit 31 tied to raster parity
- UE5 logging helper:
  - explicit `insert_disc` logging added in `R3000EmuComponent.cpp`
- Version markers updated:
  - `CORE v7`
  - `GPU v14`
  - `GTE v12`
  - `CDROM v13`
  - `CPU v10`
  - `BUS v27`
  - `Worker Run v26`

## Current Performance Observation
- Latest UE5 logs show:
  - `insert_disc OK`
  - command flow reaches `Pause`, `SetLoc`, `SeekL`, `SetMode`, `ReadN`
  - data reads continue on the data port
- Therefore the active symptom is not a hard CD init stop at `insert_disc`.
- However, the same run still shows:
  - `~43-56% speed`
  - `~21-28 VBl/s`
- High-volume CD polling logs (`RD/WR/FIFO/IRQ_ACK/Async IRQ`) were still active in that run.
- New minimal change applied:
  - demote those polling logs out of `info`
  - keep command-level logs visible

## Reverted Changes
- Reverted the whole recent CD continuous-read scheduling experiment:
  - no special requeue on FIFO drain
  - no forced command priority over deferred async INT1
  - no Pause/Stop flush of pending async read state
- Reason:
  - caused regressions
  - current goal is to return to the last known better UE5 state first
- Reverted the file-open backend experiment from this session:
  - no delayed one-track cache
  - no extra `errno=24` diagnostics in the normal path
  - no `file_util.h` fallback change
- Remaining suspect after those rollbacks:
  - the removed `ReadN continuous` requeue block

## Current UE5 Truth Source
- `E:\Projects\github\Live\PSXVR\logs\system.log`
- `E:\Projects\github\Live\PSXVR\logs\cdrom.log`

## What To Check Next
1. Rebuild UE5 live and rerun once.
2. In `system.log`, look for:
   - `UE insert_disc OK`
   - `CDROM source v13`
3. In `cdrom.log`, look for:
   - `GetID`
   - `ReadTOC`
   - `SetLoc`
   - `SeekL`
   - `ReadN`
   - `Pause`
4. Verify whether restoring the `ReadN continuous` requeue block is enough to recover the earlier better baseline.

## Baseline Requirement Before Push
- Do not push until UE5 is back at least to:
  - normal boot
  - no BIOS CD loop
  - ideally Galaga visible again
