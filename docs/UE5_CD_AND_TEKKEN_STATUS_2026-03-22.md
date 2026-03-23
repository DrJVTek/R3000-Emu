# UE5 CD And Tekken Status

Date: 2026-03-22
Scope: UE5 live, Tekken (Europe), non-HLE, BIOS réel

## Git milestones
- Stable temporary baseline pushed:
  - branch `stable/2026-03-22-ue5-tekken-baseline`
  - commit `8a4b571`
- Current debug checkpoint pushed:
  - branch `checkpoint/2026-03-22-tekken-dma3-callback-order`
- Current clean work branch pushed:
  - branch `work/2026-03-22-tekken-dma3-repro-next`

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

## 2026-03-22 late update - current callback-order proof

- UE5 live now proves that the CD scheduler itself is not the only remaining suspect:
  - event class 3 is scheduled (`0x8015D3D8`)
  - `gate4` dispatch runs
  - the callback global at `0x801EF68C` is updated from `0x8015D388` to `0x8015D428`
- Proven callback order:
  1. callback `0x8015D388` is installed
  2. during the same `Ready=1` period, callback `0x8015D428` is installed
  3. `0x8015D428` is then consumed while `EXE_DST_PTR == 0`
  4. this produces `FUN_8016AAE0(a0=0, a1=0x200)` and then `DMA3.MADR=0`
- The key direct proof from UE5 detailed logs:
  - `TEKK_CBSET` shows `cb_now=0x8015D388`, then `cb_now=0x8015D428`
  - at the second install:
    - `ready=0x00000001`
    - `exe_dst=0`
    - `exe_remain=0x150`
  - `TEKK_CB_ZERO` immediately follows on `0x8015D428`
- A more structural proof patch was then tested:
  - detect a callback swap during one contiguous `Ready=1`
  - log `TEKK_READY_REUSE_HOLD`
  - force `Ready=0` at the specific `jalr` point
- Result:
  - the hold triggers
  - but `0x8015D428` still runs immediately after with `EXE_DST_PTR=0`
  - therefore the current problem is not a single `jalr` site only
  - the callback is already latched/reconsumed elsewhere in the same flow

## Practical reading of the bug now

- What is proven:
  - this is not only a raw DMA bug
  - this is not only a missing event3 dispatch
  - this is not only the `0x8016AC2C` `jalr`
- What remains most likely:
  - a callback/ready consumption ordering issue in the CD BIOS flow
  - specifically, a new callback becomes visible too early for the same already-active `Ready` condition

## Recommended strategy from here

- Do not continue by instrumenting every byte of the BIOS path in the live branch.
- Keep two tracks separate:
  1. `8a4b571` / stable baseline:
     - the useful state that passes this earlier crash and reaches the later `Now Loading`/Galaga/loading problem
  2. current heavy callback instrumentation:
     - only for proving callback-order facts
- Recommended next debug direction:
  - go back to the useful baseline that reaches the later lock
  - compare that state against the current callback-order branch
  - isolate only the delta that reintroduced the early null-DMA path
- In plain terms:
  - the fast way out is not "trace more BIOS"
  - it is "return to the last state that gets past this crash, then bisect the few deltas that brought it back"

## 2026-03-22 seek timing comparison with DuckStation

- User hypothesis: the remaining ordering bug may originate in seek/read timing rather than the DMA path itself.
- Local DuckStation source used for comparison:
  - `E:\Projects\github\Live\duckstation\src\core\cdrom.cpp`
- Key differences that were found:
  - our old `calc_seek_time()` was still a 10x-fast hack
  - our first `ReadN/ReadS` sector used effectively `seek only`
  - DuckStation schedules the first sector as:
    - `GetTicksForRead() + GetTicksForSeek(...)`
    - or only `GetTicksForRead()` when immediately following a completed seek
  - our continuous read cadence still used the old fast `11000/22000` delays

### Applied correction

In `src/cdrom/cdrom.cpp`:

- replaced the old seek hack with a DuckStation-style distance-based seek model
- added `read_sector_ticks()`:
  - single speed: `451584`
  - double speed: `225792`
- `SetLoc` now sets `seek_pending_`
- `SeekL/SeekP` clear `seek_pending_`
- first `ReadN/ReadS` INT1 now waits:
  - one sector interval
  - plus seek/spin-up time if a seek is still pending
- continuous `ReadN/ReadS` now uses real per-sector cadence

### Why this matters

- If the drive model delivers the first sector too early, software-visible callback order changes.
- That matches the current Tekken symptom:

## 2026-03-22 latest CD controller review

- Latest proven point:
  - the current UE5 run loads the new CD code banner
  - `CDROM source v16 (secondary_status_refactor)`
- Latest proven disk data point:
  - `SYSTEM.CNF` and the following `PS-X EXE` sector are read correctly
  - this removes payload corruption as the main suspect for the current post-logo stall
- Structural fix kept:
  - `data_lba_` now latches the sector actually announced by `INT1/DataReady`
  - FIFO fill no longer reuses a newer `loc_lba_` after `Pause`/`SetLoc`
- New structural fix applied:
  - `status_` is now treated explicitly as the CD secondary status
  - named bits now match DuckStation/PSX-SPX semantics:
    - motor on
    - reading
    - seeking
    - playing
    - error bit only in error responses
  - first/second response ordering was corrected for:
    - `ReadN`
    - `SeekL/SeekP`
    - `Pause`
    - `Stop`
    - `Init`
  - async responses can now use live secondary status at delivery time instead of a stale snapshot
- Current best hypothesis after this review:
  - the remaining stall after PlayStation logo is now more likely a controller-state contract issue
    than a raw sector payload issue
  - the immediate next validation is UE5 live behavior with `CDROM source v16`
  - callback `0x8015D428` becomes visible and is consumed while EXE state is still incomplete.
- This change is intended as a real CD timing correction, not a Tekken-specific workaround.

## 2026-03-22 - `v18` outcome and current blocker

- New CD banner:
  - `CDROM source v18 (irq_flag_reg_fix)`

- 2026-03-22 late session:
  - current CDROM build marker:
    - `CDROM source v19 (deferred_command_exec)`
  - structural change:
    - command writes no longer execute `exec_command()` immediately
    - commands are now latched and executed later from `Cdrom::tick()`
  - intent:
    - align our model with DuckStation's command-event behavior
    - stop exposing response FIFO bytes before the matching command event/IRQ timing
  - implementation notes:
    - removed remaining source references to `cmd_irq_pending_` / `cmd_irq_delay_`
    - command path now uses `schedule_command_execution(...)`
    - queued commands are restarted through the same deferred pipeline
  - current expectation for next UE5 validation:
    - BIOS CD MMIO order around `GetStat/GetID/ReadTOC/ReadN` should change materially
    - if the stall remains, the logs should now reflect a true deferred command model rather than the old immediate-response model

### Exact fix applied

- corrected `1F801803` index `1/3`:
  - old behavior: `irq_flags | cmd_ready | 0xE0`
  - new behavior: `(irq_flags & 0x1F) | 0xE0`

This changed BIOS-observed values from:
- `F3/F2/F1`
to:
- `E3/E2/E1`

### What `v18` proves

- the BIOS sees the corrected IRQ flag register
- but the run still stalls after the PlayStation logo
- and still ends with:
  - `ADES`
  - `PC=0xBFC03D1C`
  - `BadVAddr=0x3D20544E`

### Current diagnosis

The current blocker is no longer best explained by:
- bad payload
- lost IRQ2
- DMA3 failure
- bad IRQ flag register semantics

The best current diagnosis is:
- **CD command overlap while a `ReadN` is still active**

Observed in current UE5 logs:
- `LBA 60642` finishes normally
- `LBA 60643` starts normally
- then BIOS sends `ReadTOC` and `GetID` before a normal completion path for that read is visible

### Next required work

Review and correct command arbitration in the CD controller:
- `ReadN`
- `Pause`
- `ReadTOC`
- `GetID`
- pending async response / pending read completion interaction

The next real fix is now in the CD **state machine**, not in raw IRQ plumbing.

## 2026-03-22 - Command gating fix after v18

- Root cause candidate strengthened:
  - `mmio_write8()` accepted a new command while a prior CD command still had async work in flight
  - relevant state at that time was:
    - `cmd_irq_delay_`
    - `pending_irq_type_`
    - `read_pending_irq1_`
    - `async_stat_pending_`
- Fix applied in `src/cdrom/cdrom.cpp`:
  - `CMD_WRITE` now queues instead of executing immediately when any async response is still pending
  - queued commands now restart only when:
    - `irq_flags == 0`
    - `pending_irq_type_ == 0`
    - `cmd_irq_delay_ == 0`
    - `read_pending_irq1_ == 0`
    - `async_stat_pending_ == 0`
- Goal:
  - stop `ReadTOC`/`GetID` from overlapping an active `ReadN` path during BIOS boot (`LBA 60643` case)

## 2026-03-22 late update - current UE5 blocker and CLI comparison

### Current proven state in UE5
- Build markers loaded in UE5:
  - `CDROM source v19 (deferred_command_exec)`
  - `BUS source v28 (session_2026_03_22)`
- BIOS reads `LBA=16` and DMA3 writes the correct PVD bytes to RAM:
  - `madr=0xA000B070`
  - ASCII `.CD001..PLAYSTAT`
- No `SYSTEM.CNF` request follows in UE5.
- So UE5 is blocked before the BIOS starts the later ISO9660 path.

### Fresh CLI comparison with the same current core
A fresh standalone CLI run with the same BIOS and Tekken disc shows that the same core does continue after `LBA=16`.

CLI sequence observed:
- `ReadN/S START: LBA=16`
- `Async IRQ1 delivered (resp=0x22)`
- `FIFO LBA=16 [01434430 30310100]`
- `CMD 0x09 (Pause)`
- then further reads:
  - `LBA=18`
  - `LBA=22`
  - `LBA=60642` (`SYSTEM.CNF`)
  - `LBA=60643` (`PS-X EXE`)

UE5 sequence observed:
- `ReadN/S START: LBA=16`
- `DMA3 done: madr=0xA000B070`
- `QUEUE CMD 0x09 (Pause): irq=0x00 busy=0 cmd_exec=0 pend_type=1 read_pend=0 async=0`
- `Cancelled pending continuous ReadN INT1 advance`
- then no later `SetLoc/ReadN` for `60642/60643`

### Current diagnosis
- The current blocker is no longer a generic CDROM content/DMA bug.
- The current blocker is now best described as:
  - an UE5-specific runtime divergence after the PVD `INT1/DMA` path,
  - before the BIOS continues into the `SYSTEM.CNF` lookup sequence.

### Practical meaning
- CLI and UE5 do not diverge on the PVD bytes.
- They diverge on what happens immediately after the PVD sector has been delivered.
- That points to timing/order/runtime interaction rather than ISO9660 payload corruption.



## 2026-03-22 correction - actual UE5 `LBA16` behavior

The latest UE5 run with promoted `info` logs around `LBA=16` proves the following sequence really happens in UE5:
- `ReadN/S START: LBA=16`
- IRQ1 arrives
- BIOS ACKs IRQ1
- BIOS performs MMIO writes that request data:
  - `WR 0x3 idx=0 val=0x80`
- FIFO is filled:
  - `FIFO LBA=16 [01434430 30310100]`
- DMA3 copies the sector to BIOS RAM:
  - `madr=0xA000B070`
- BIOS then writes:
  - `WR 0x1 idx=0 val=0x09`
  - `QUEUE CMD 0x09 (Pause)`
- and UE5 stops there.

Therefore the previous statement "UE5 never consumes the PVD" is incorrect.
The corrected statement is:
- UE5 consumes the `LBA16` PVD transfer path correctly through FIFO + DMA + `Pause`
- the divergence versus CLI appears immediately after this `Pause`, not before the PVD DMA

## 2026-03-22 latest update - queued `Pause` fix and absolute-time CD timing moved UE5 past the old BIOS crash

### What changed

- A real bug was fixed in `src/cdrom/cdrom.cpp`:
  - a queued `Pause` command could stay queued forever in UE5 if the pipeline became idle without another MMIO ACK path to restart it
  - the restart was moved into `Cdrom::tick()` when the command/IRQ/response pipeline becomes truly idle

### Effect in UE5

- These fixes did move UE5 forward.
- The run no longer stops only at the old `LBA16 -> Pause` point.
- The latest UE5 run now reaches all of:
  - `LBA 16`
  - `LBA 18`
  - `LBA 22`
  - `LBA 60642` (`SYSTEM.CNF`)
  - `LBA 60643` (`PS-X EXE`)
- The old BIOS `ADES` at `0xBFC03D1C` is no longer the active failure in the latest run.

### New hard evidence

From the latest UE5 run:
- BIOS DMA scratch dumps show:
  - `read_lba=16` with `.CD001..PLAYSTAT`
  - `read_lba=60642` with `BOOT = cdrom:SCE`
  - `read_lba=60643` with `PS-X EXE........`
- So the BIOS is now receiving the correct `PS-X EXE` sector in UE5, not stale `SYSTEM.CNF`.

### Current diagnosis

- The blocker is no longer best described as:
  - PVD transfer failure
  - IRQ2 loss
  - DMA3 failure
  - stale `SYSTEM.CNF` still present when `PS-X EXE` should have arrived
- The current blocker is now:
  - BIOS handoff / post-boot transition after successful `PS-X EXE` delivery
  - or the first post-BIOS execution step after that handoff

### Practical meaning

- The queued-`Pause` restart fix and deadline-based CD timing were necessary.
- They did not finish the boot.
- They moved the failure later:
  - from BIOS scratch-buffer corruption
  - to BIOS exit / post-`PS-X EXE` startup behavior

### Current target

- Trace the first non-BIOS PC after `LBA 60643` DMA.
- If no non-BIOS PC appears, debug the BIOS loop after successful `PS-X EXE` delivery.
- If a non-BIOS PC appears, move the investigation to the game-side startup path.
