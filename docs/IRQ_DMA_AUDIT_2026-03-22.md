# IRQ / DMA Audit - 2026-03-22

## Scope

Targeted review of the current non-HLE boot path around:
- R3000 IRQ entry / exception dispatch
- IRQ controller (`I_STAT` / `I_MASK`)
- CDROM IRQ generation and latching
- DMA3 (CDROM -> RAM)

Files reviewed:
- `src/r3000/cpu.cpp`
- `src/r3000/bus.cpp`
- `src/cdrom/cdrom.cpp`
- `src/cdrom/cdrom.h`

User preference remains: `bHleVectors=false`.

---

## Executive Summary

The current codebase has two distinct IRQ/DMA models mixed together:

1. a real non-HLE path where the CPU takes an `EXC_INT` based on `I_STAT & I_MASK`
2. a set of bring-up workarounds in `bus.cpp` that modify BIOS event state directly

This means the emulator can appear to boot while still violating the real PS1 contract.
The most dangerous parts are not the visible CD commands themselves, but the hidden helpers that:
- force BIOS events to READY
- force-enable `I_MASK`
- keep DMA3 extremely simplified

These helpers may have been necessary for bring-up, but they are structural debt for correctness.

---

## What Is Actually Active In Non-HLE

### CPU IRQ entry is real enough

In `src/r3000/cpu.cpp:940-982`, the non-HLE path does:
- compute pending IRQ from `bus_.irq_pending_masked()`
- map it to `COP0.Cause.IP2`
- raise `EXC_INT` when `Status.IEc=1` and `IP & IM != 0`

This is the real non-HLE IRQ gate currently in use.

### The scary `ALWAYS use HLE for IRQ handling` block is not active when `bHleVectors=false`

In `src/r3000/cpu.cpp:2253-2280`, the code comment says IRQs are always handled in HLE.
That statement is misleading in the current project context.

The block is guarded by:
- `if (hle_vectors_ && pc_ == 0x80000080)`

So with user preference `bHleVectors=false`, this path is not the one currently blocking boot.
It is still technical debt because the comment is broader than the real condition.

---

## Findings

### 1. `bus.cpp` still mutates BIOS event state directly for CDROM IRQ

Reference:
- `src/r3000/bus.cpp:1947-1963`
- `src/r3000/bus.cpp:1893-1943`

When a CDROM IRQ edge is detected, the bus does two things:
- sets `I_STAT` bit 2
- calls `deliver_events_for_class(..., 0x28u)`

That second step is not hardware behavior. It is a BIOS event-table mutation.
It means the bus is not only exposing IRQ state, but also delivering the software event on behalf of the BIOS handler.

Risk:
- can hide real IRQ ordering bugs
- can create different behavior from real BIOS dispatch timing
- can make CD look correct while the kernel event contract is still wrong

Status:
- keep for now unless logs prove it is the current blocker
- do not remove blindly during active boot debugging

### 2. `bus.cpp` contains two major non-hardware IRQ workarounds

Reference:
- `src/r3000/bus.cpp:2285-2338`
- `src/r3000/bus.cpp:2340-2394`

#### 2a. `RESCUE` busy->ready event rewriting

After long VBlank stalls, the bus scans the BIOS event table and rewrites BUSY events to READY.

This is a hard behavior override.

#### 2b. auto-enable `I_MASK`

If `I_MASK` remains zero for long enough, the bus forces:
- `I_MASK = 0x0075`

This is also a hard behavior override.

Risk:
- can convert a real IRQ bug into a fake-working path
- can make later stages unstable because the BIOS did not reach that state naturally

Observation:
- older logs show `DIAG I_MASK re-enable` firing in previous sessions
- this remains a high-risk workaround even if it does not fire on every run

### 3. DMA3 is functionally useful but still highly simplified

Reference:
- `src/r3000/bus.cpp:1614-1656`
- `src/r3000/bus.cpp:1858-1888`

Current DMA3 behavior:
- one DMA start drains the CD data FIFO immediately in a tight loop
- 4 bytes are pulled from `1F801802` per word
- no finer-grained timing or pacing exists inside the transfer
- completion then updates DICR and possibly raises DMA IRQ

What is acceptable:
- this is enough for many boot flows
- the DMA IRQ/DICR plumbing is at least present

What is missing relative to hardware:
- no sub-transfer pacing
- no deeper handshake between DREQ/data-ready and transfer start
- no bus contention / partial progress model

This is not automatically the blocker for the current boot, but it is still not a faithful DMA engine.

### 4. CDROM IRQ model is internally coherent, but still layered on top of BIOS-facing hacks

Reference:
- `src/cdrom/cdrom.cpp:844-866`
- `src/cdrom/cdrom.cpp:959-973`
- `src/cdrom/cdrom.cpp:2738-2846`

Current CD side is better than before:
- IRQ type is stored in `irq_flags_`
- `irq_line()` respects `irq_enable_`
- async IRQ delivery is delayed and edge-shaped
- current logs now expose:
  - `IRQ set`
  - `BUS IRQ2 latch`
  - `DMA3 start/done/blocked`

This is useful and should be kept.

But because the bus still mutates the BIOS event table directly, we do not yet have a pure IRQ path.

### 5. `cpu.cpp` still contains a large amount of one-off Tekken debug logic

Reference:
- `src/r3000/cpu.cpp` large sections around `TEKK_*`

This is not an IRQ/DMA correctness issue by itself, but it is now maintenance risk.
It increases the chance of misreading the real path and makes future IRQ work harder to audit.

Recommendation:
- keep only if still actively used
- otherwise isolate or remove once this boot issue is solved

---

## Safe Conclusions

### Safe conclusion 1
Current post-logo blocking is no longer obviously a raw CD payload problem.
Recent runs already show:
- `SYSTEM.CNF` read correctly
- `PS-X EXE` header read correctly
- `Pause` cancelling pending continuous reads more cleanly than before

### Safe conclusion 2
IRQ path remains suspect because BIOS-visible behavior is still partly synthesized.
The two biggest reasons are:
- direct CD event delivery from `bus.cpp`
- forced `I_MASK` / `RESCUE` logic

### Safe conclusion 3
Do not rip out those hacks all at once.
Doing so would likely collapse boot entirely and destroy observability.
They must be removed in a controlled order with logs.

---

## Recommended Fix Order

### Phase 1: observability only

Already in progress with `v17`:
- log CD IRQ set
- log bus IRQ2 latch
- log DMA3 start/end/block

Goal:
- determine whether IRQ sequencing is coherent during the stuck phase

### Phase 2: prove whether `I_MASK` or `RESCUE` actually fire on the failing path

Do not remove anything yet.
First prove on the failing run whether:
- `Auto-enable I_MASK` fires
- `RESCUE` fires

If they do not fire, they are debt but not the immediate blocker.

### Phase 3: remove synthetic CD event delivery before removing anything else

Target:
- `deliver_events_for_class(..., 0x28u)` in `check_cdrom_irq_edge()`

But only after we have enough logs to prove the BIOS handler really sees and processes IRQ2 correctly.
This is the cleanest medium-term IRQ fix because it removes software event mutation from the hardware bus.

### Phase 4: only then revisit `I_MASK` auto-enable and `RESCUE`

Order matters:
1. prove real IRQ2 path
2. stop synthetic CD event delivery
3. then test whether `I_MASK` auto-enable is still needed
4. only then remove `RESCUE`

### Phase 5: DMA accuracy later

DMA3 accuracy is a lower priority than IRQ truthfulness for the current boot issue.
Keep DMA3 simple until IRQ/event ownership is cleaned up.

---

## Conservative Fixes Applied In This Session

No risky behavioral IRQ/DMA hack was removed in this audit pass.
Only observability was improved via `cdrom.log`:
- IRQ set traces
- bus IRQ2 latch traces
- DMA3 start/end/block traces

This was intentional to avoid recollapsing boot while establishing the real failing sequence.

## Important Diagnostic Caveat Discovered Later

Several critical IRQ traces were originally guarded by function-local `static` counters:
- `pending_masked=`
- `TAKE_IRQ`
- `EXC_INT`
- `MTC0 Status`
- `RFE`

In the UE5 workflow, the editor process survives across PIE runs, so those counters also survived across runs.
That means:
- absence of one of these logs in a later run was **not** reliable evidence that the path did not execute
- it could simply mean the per-process quota had already been exhausted in a previous run

This has now been corrected in code by moving the critical IRQ trace quotas to per-instance fields reset with each core lifetime.

Additional targeted traces were also added:
- `I_STAT ACK CD ...` when bit 2 is actually cleared, with the writer `PC`
- `BIOS CD pending ...` when BIOS ROM code is running while CD IRQ is still pending

These two traces are higher-value than the previous inference-only method because they tell us:
1. whether the BIOS is explicitly acknowledging `I_STAT bit 2`
2. whether the CPU sees a pending CD IRQ while still executing BIOS ROM code

---

## Next Concrete Checks

On the next failing run, inspect `cdrom.log` for this exact chain:

1. `IRQ set:`
2. `BUS IRQ2 latch:`
3. `DMA3 start:`
4. `DMA3 done:`
5. next command / ACK / next IRQ

Questions to answer:
- does every CD `INT1/INT2/INT3` produce a visible `IRQ2` latch?
- does `DMA3` consume FIFO only after the expected `INT1/DataReady`?
- who clears `I_STAT bit 2`, and from which `PC`?
- when BIOS ROM is active with CD IRQ pending, what are `Status`, `IEc`, `IP`, and `IM`?
- does the stuck phase happen after IRQs stop, or while IRQs continue normally?

That determines whether the next real fix is:
- IRQ ownership / BIOS event dispatch
or
- post-BIOS CPU/VBlank/game event logic

- 2026-03-22: removed synthetic CD BIOS event injection (deliver_events_for_class(..., 0x28u)) from the CD IRQ push path and the legacy sampling path; keep only hardware IRQ2 latch via I_STAT bit 2 for validation.

- 2026-03-22: added targeted BIOS CD MMIO trace in Bus for PC range 0xBFC04A00..0xBFC05580 to capture the exact register poll/write sequence during the post-logo BIOS loop.

- 2026-03-22 late finding: 1F801803 index 1/3 was incorrectly ORing bit4 command-ready into IRQ flag reads (returning F1/F2/F3 instead of E1/E2/E3). Fixed in CDROM source v18 (irq_flag_reg_fix).
- 2026-03-22 late finding: the larger structural divergence vs DuckStation was command timing itself:
  - our CD controller executed `exec_command()` immediately on command write, filled the response FIFO immediately, and only delayed the IRQ
  - DuckStation defers command execution itself via a command event
  - CDROM source v19 (`deferred_command_exec`) starts migrating to that model by latching commands and executing them later from `Cdrom::tick()`

## 2026-03-22 latest state after `v18`

### Confirmed fixed

- `1F801803` index `1/3` now returns:
  - `E3`
  - `E2`
  - `E1`
- not:
  - `F3`
  - `F2`
  - `F1`

This confirms the old `command-ready` contamination of the IRQ flag register is fixed.

### Confirmed still good

- `CDROM IRQ push`
- `BUS IRQ2 latch`
- `I_STAT ACK CD via SW`
- `DMA3 start/done`

So the path:
- CD IRQ generation
- bus latch
- BIOS-side ACK
- DMA transfer

is not the main blocker anymore.

### Current blocker

The strongest current evidence now points to **CD command overlap / bad command arbitration**.

Observed live:
- `LBA 60642` completes normally
- `LBA 60643` starts with `ReadN`
- then BIOS issues:
  - `ReadTOC`
  - `GetID`
before a normal completion sequence for that active read is visible

The drive still reports:
- `read=1`
- `stat=0x22`

This strongly suggests that commands are being accepted while a `ReadN` pipeline is still active.

### Recommended next fix

Stop focusing on raw IRQ latch behavior here.

The next fix should target:
- command acceptance in `Cdrom::mmio_write8()`
- queued command execution
- pending async read completion

Goal:
- serialize or defer `ReadTOC` / `GetID` / similar commands correctly while `ReadN` is still active
