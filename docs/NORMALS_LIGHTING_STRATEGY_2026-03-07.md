# Normals and Lighting Strategy

## Goal

Define a clear strategy for normals and lighting reconstruction in the PSX3D / PSXVR pipeline.

The priority is:

- preserve source normals when they exist,
- avoid recomputing them unnecessarily,
- only fall back to geometric reconstruction when required.

This matters for:

- visual fidelity,
- stable shading in UE5,
- future object-space mesh caching,
- future VR lighting work.

---

## Core rule

Preferred order:

1. use source normals from the game / GTE-correlated path
2. use recovered per-object or per-vertex normals from offline analysis
3. compute fallback face normals only if no reliable source exists

The cross-product path must stay a fallback, not the default target.

---

## Current state

In the current UE5 reconstruction path:

- if `Cmd3D.nx/ny/nz` contain valid data, they are used
- otherwise a face normal is computed from the reconstructed triangle

This is acceptable as a first step, but not sufficient as a final system.

Why:

- many games reuse stable object-space normals
- recomputed normals can flatten or distort shading
- recomputing each frame wastes work
- a cached mesh should keep its original normals

---

## Sources of normals

Possible sources, in order of desirability:

### 1. Direct per-vertex normals in correlated GTE/GPU path

Best case.

If the reconstruction path already carries vertex normals:

- preserve them
- map coordinate system once
- store them with the reconstructed geometry

### 2. CPU-side geometry data in RAM

Some games keep vertex normals in geometry structures or transform input buffers.

If offline analysis identifies:

- normal arrays
- mesh structures
- per-object vertex blocks

then these should be treated as authoritative source data.

### 3. Derived lighting vectors from GTE/game-side lighting setup

Sometimes the game may not expose reusable vertex normals directly in the final pipeline,
but the surrounding code can still reveal:

- directional light vectors
- ambient terms
- normal transforms

This is useful for shading reconstruction even if geometric normals are only partially known.

### 4. Geometric fallback

If nothing else is available:

- compute face normal from triangle winding
- optionally duplicate it to all three vertices

This should remain a fallback only.

---

## Runtime policy

At runtime:

- never overwrite valid source normals with recomputed normals
- never recompute normals for a cached object-space mesh unless invalidated
- only compute fallback normals when the source path is absent or clearly invalid

Validation checks:

- zero-length normals
- NaN / degenerate values
- obviously broken orientation after coordinate remap

If source normals fail validation, then fallback is acceptable.

---

## Mesh cache implications

For the future object-space mesh cache:

- normals must be part of the canonical mesh payload
- they should be cached with:
  - positions
  - indices
  - UVs
  - material state

Do not design the cache as:

- positions cached
- normals recomputed every frame

That would throw away one of the main benefits of caching.

Correct model:

- canonical mesh = geometry + normals + UV + material metadata
- runtime instance = transform + visibility + optional per-instance overrides

---

## Offline analysis goals

The offline pipeline should try to identify:

- where normals come from
- whether they are per-face or per-vertex
- whether the game uses flat shading or smooth shading for a given path
- whether lighting is driven mainly by:
  - GTE lighting instructions
  - CPU-prelit colors
  - a hybrid path

This should be persisted in:

- Ghidra annotations
- offline knowledge
- possibly the profile if the information is needed at runtime

---

## Ghidra annotation guidance

When a normal-related path is identified, annotate it explicitly.

Recommended prefixes:

- `LIT_`
- `NRM_`
- `GTE_`

Examples:

- `NRM_LoadVertexNormals_8004AB10`
- `LIT_SetDirectionalLight_80052F80`
- `GTE_NormalColorSingle_80061A20`

Recommended comments:

- source of normals
- whether data is per-face or per-vertex
- related object/mesh structure
- confidence level
- relation to runtime PCs or hooks

---

## Lighting reconstruction

Normals and lighting should be related but not merged blindly.

Separate concepts:

- `normals`
  - geometric orientation
  - per-face or per-vertex

- `lighting`
  - ambient
  - directional light vectors
  - colors / intensities
  - game-specific shading rules

The system should support:

- source normals + cheap shader lighting
- source normals + proxy UE5 lights
- source normals + no dynamic lighting on constrained hardware

Quest-class runtime may prefer:

- preserved normals
- cheaper material-driven lighting approximation

instead of spawning many real UE lights.

---

## Practical direction

Short term:

- preserve `Cmd3D` normals when present
- keep fallback cross-product only for missing data

Mid term:

- identify normal sources per game/path through CLI + Ghidra analysis
- persist those findings

Long term:

- store canonical normals in object-space mesh cache
- reconstruct lighting as a separate optional layer

---

## Guiding principle

Do not treat normals as disposable derived data.

Whenever possible, treat them as first-class source data coming from the game.

