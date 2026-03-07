# Ridge Racer: CLI vs UE5 Comparison (2026-03-06)

## Context
- Goal: understand why UE5 still shows missing polygons while recent CPU/GPU provenance changes improved results.
- Constraint: no push for this step, analysis only.

## What was tested
- Local CLI run with current workspace code:
  - `lib/Debug/r3000_emu.exe --cd=ridgeracer.cue --max-steps=520000000 --psx3d-analysis=1 --psx3d-mode=game --emu-log-level=warn`
  - Log: `tmp_now_long.log`

## Key CLI result
- Critical Ridge Racer phase (menu/demo packet burst) is significantly improved:
  - At frames like `540/600/660/720`:
    - `3d=1122`
    - `tok_miss=0`
    - `miss_no_hint=0`
    - `miss_decode_fail=0`
- This means the current core can resolve token flow for the problematic burst in CLI.

## Why this matters
- The old bad signature was:
  - `3d=626`, `tok_miss=248`, `miss_no_hint=248`, `miss_decode_fail=248`
- That signature is **not** present in current CLI run.
- So the current regression seen by the user is likely not a pure core regression at this point.

## Likely UE5 divergence
- UE5 logs repeatedly showed:
  - `hotspots=110 analyzed=110`
  - `analysis pass done ... added_pcs=0`
  - profile path often ending in `psx3d.profil` (legacy)
- This strongly suggests UE5 can be stuck on a stale profile/analysis state.

## Main hypothesis
- CLI and UE5 are not using the same effective runtime state:
  - Different loaded binary, and/or
  - Different profile file/path, and/or
  - Legacy profile locking analysis into a plateau (`added_pcs=0` forever).

## Evidence points from logs
- CLI (current run):
  - burst resolves to mostly 3D (`1122`) with zero token decode misses.
- UE5 (reported sessions):
  - frequent refresh toggles with no new PCs added (`added_pcs=0`)
  - stable analyzed count (`110`) despite fallback spikes.

## Proposed UE5 verification plan (tomorrow)
1. Confirm UE5 loads the latest built runtime DLL from this workspace branch.
2. Remove/rename legacy profile:
   - `E:\Projects\github\Live\R3000-Emu\psx3d.profil`
3. In UE component:
   - keep `Psx3dProfilePath` empty (no forced legacy override), so default per-game profile path is used.
4. Run Ridge Racer to same phase and compare:
   - `GPU3D_VBLANK` fields: `3d`, `2d`, `tok_miss`, `miss_no_hint`, `miss_decode_fail`
   - `PSX3D` fields: `added_pcs`, total analyzed growth over time
5. If UE still diverges:
   - capture first window where UE is bad but CLI is good
   - compare top `DMA2_NOHINT` PCs between UE and CLI for same scene.

## Expected success signature in UE5
- In the critical phase, values should move toward CLI behavior:
  - high 3D count (`~1120` range in this run),
  - and `tok_miss/miss_no_hint/miss_decode_fail` near zero.

## Notes
- Some 2D fallback can remain (not all fallback is necessarily wrong).
- Immediate objective is to eliminate the large unresolved block (the old `248` decode misses pattern).
