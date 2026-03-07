# MCP Authoring Pipeline

## Goal

Define a practical offline-first authoring pipeline for producing high-quality
per-game PSX VR compatibility data:

- `.psx3dprof` for geometry/camera correlation fast paths
- optional lighting knowledge
- optional VR patch data

This pipeline is not part of the runtime.

The runtime stays lean.

The pipeline can be heavy.

---

## Roles

### 1. CLI MCP

Primary offline control surface for the emulator.

Responsibilities:

- boot a game in deterministic conditions
- run until a target state or frame budget
- enable/disable analysis mode
- install hooks/breakpoints/watchpoints
- inspect CPU/GTE/GPU/DMA state
- dump RAM / registers / draw-list / profile summaries
- trigger profile refresh
- save `.psx3dprof`

This is the main authoring endpoint.

### 2. UE5 MCP

Interactive companion endpoint.

Responsibilities:

- drive scenes that are hard to reach from CLI
- move free camera around reconstructed geometry
- drive `VR Recenterable` workflows
- trigger visual captures or debug toggles
- validate the result of `.psx3dprof`, lighting, and VR patch assumptions

UE5 MCP should not own the heavy analysis logic.

It is for acquisition and validation.

### 3. GhidraMCP

Static analysis endpoint.

Responsibilities:

- inspect functions and strings
- decompile hot functions
- resolve xrefs around runtime hotspots
- create durable names/labels/comments/bookmarks
- keep a readable map of what has already been understood

### 4. LLM

Synthesis/orchestration layer.

Responsibilities:

- correlate runtime evidence with static code
- propose candidate rules
- decide what should enter `.psx3dprof`
- document confidence, ambiguity, and residual risks

The LLM is an authoring assistant, not a runtime dependency.

---

## CLI MCP: Required Tools

The CLI MCP should expose a small, stable toolset first.

### Session / execution

- `session.open(game_path, bios_mode, hle, profile_path?)`
- `session.close()`
- `session.reset()`
- `run.steps(count)`
- `run.until_frame(frame_id)`
- `run.until_pc(pc)`
- `run.until_hook(id)`

### Runtime mode / profiling

- `psx3d.set_mode(mode)`
- `psx3d.set_analysis_enabled(enabled)`
- `psx3d.request_refresh(reason, scope)`
- `psx3d.save_profile(path?)`
- `psx3d.get_status()`
- `psx3d.get_frame_stats()`

### Breakpoints / hooks / watches

- `hook.add_pc(pc, name?, once?)`
- `hook.remove(id)`
- `hook.list()`
- `watch.add_ram(addr, size, mode)`
- `watch.remove(id)`
- `watch.list()`

### Inspection

- `cpu.get_registers()`
- `cpu.get_backtrace(depth?)`
- `gpu.get_drawlist_summary()`
- `gpu.get_primitive_window(count, around_frame?)`
- `gte.get_last_faces(count?)`
- `dma.get_channel_state(channel)`
- `memory.read(addr, size)`

### Export / capture

- `capture.save_ram(path, base, size)`
- `capture.save_drawlist(path, frame_id?)`
- `capture.save_profile_snapshot(path)`
- `capture.save_trace(path, category, budget)`

This first version should prioritize deterministic text/JSON outputs.

No complex UI is needed.

---

## UE5 MCP: Required Tools

The UE5 MCP can stay smaller.

### Scene / play session

- `ue.play()`
- `ue.stop()`
- `ue.get_psx3d_status()`

### Camera / alignment

- `ue.set_gpu3d_tracking_mode(actor_or_component, mode)`
- `ue.set_gpu3d_world_offset(actor_or_component, xyz)`
- `ue.set_gpu3d_vr_view_offset(actor_or_component, xyz)`
- `ue.gpu3d_recenter_to_player_view(actor_or_component)`

### Debug / validation

- `ue.set_gpu3d_debug(actor_or_component, key, value)`
- `ue.capture_log_window(filter?)`
- `ue.capture_screenshot(path?)`
- `ue.get_last_gpu3d_stats()`

This endpoint is about visual validation and live tuning.

---

## Ghidra Annotation Policy

When analysis finds something important, it must leave durable metadata in Ghidra.

This is mandatory.

### Actions to support

- rename function
- create label
- set plate comment
- set pre comment
- add bookmark

### Naming policy

Names should be operational, not decorative.

Recommended prefixes:

- `PSX3D_` for geometry/correlation systems
- `GPUOT_` for DMA2/ordering-table builders
- `GTE_` for transform/lighting related helpers
- `CAM_` for camera roots or matrix loaders
- `LIT_` for lighting candidates
- `VRPATCH_` for game-specific VR overrides

Examples:

- `GPUOT_BuildTrackPacketList`
- `CAM_LoadViewMatrix_FromRoot_80047098`
- `PSX3D_SubdivideQuadCpuPath`

### Comment policy

Comments should explain why the address matters.

Recommended content:

- runtime evidence
- hot PC / hit counts / frame cadence
- relevant RAM roots
- confidence level
- open questions

Example:

`Loads camera matrix to GTE RT/TR every frame after VBlank. Seen from runtime hotspot 0x80047098. Candidate camera root at 0x000D3F5C. Confidence: high.`

### Bookmark policy

Use bookmarks for investigation state:

- `validated`
- `candidate`
- `needs_runtime_check`
- `vr_patch`

This keeps reverse engineering incremental and searchable.

---

## Profile Generation Flow

Recommended flow:

1. Run game in CLI MCP.
2. Detect hotspots, misses, camera candidates, packet builders.
3. Query Ghidra around those PCs.
4. Write back names/comments/labels in Ghidra.
5. Synthesize validated knowledge into `.psx3dprof`.
6. Validate in UE5 MCP with free camera and VR alignment.
7. If needed, produce a separate VR patch layer.

---

## Separation of Outputs

Do not mix these layers:

- `.psx3dprof`
  - validated runtime fast path
  - correlation rules
  - camera roots
  - optional lightweight scene signatures

- `lighting knowledge`
  - optional
  - light roots, vectors, colors, ambient terms

- `VR patch layer`
  - HUD/menu/camera/input/comfort adjustments
  - game-specific playability fixes

- `Ghidra annotations`
  - persistent reverse engineering memory

---

## Why this matters

The target is not "maximum genericity at runtime".

The target is:

- a lightweight runtime for UE5 / future Quest-class VR
- a heavy offline authoring pipeline
- reusable generic foundations
- durable per-game knowledge

That is the realistic path to high-quality PSX VR support.

