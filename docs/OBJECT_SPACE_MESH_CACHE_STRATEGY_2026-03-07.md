# Object-Space Mesh Cache Strategy (2026-03-07)

## Objective

Reduce runtime reconstruction cost by detecting when the emulator is repeatedly producing the same underlying 3D object, then cache that object as a persistent mesh instead of rebuilding equivalent geometry every frame.

This is especially relevant for the long-term PSXVR target:

- lower CPU cost,
- lower mesh rebuild cost,
- better fit for future standalone VR constraints,
- cleaner path toward true object instances instead of flat per-frame reprojection.

## Core idea

Because the emulator controls the GTE path, it can observe:

- source vertex flows,
- matrix usage,
- translation / rotation activity,
- repeated topology patterns.

That means it should be possible to detect:

- a stable canonical mesh,
- and separate it from its per-frame transform.

The target model is:

- cache canonical geometry once,
- then render instances by reapplying transforms,
- instead of reconstructing equivalent meshes from scratch every frame.

## Important constraint

The cache must not store "screen-space output".

That would be the wrong level.

The cache must store geometry in a stable reference space:

- object space when possible,
- or at least a consistent pre-projection space.

Otherwise the cache becomes invalid as soon as the camera or transform changes.

## Required capability: auto-detection

This cannot rely on hardcoded per-game assumptions only.

The system must be able to auto-detect when a flow is promotable to an object-space cache candidate.

That means the runtime or offline analysis must answer:

- are these vertices the same underlying object?
- is the topology stable?
- is the transform separable from the geometry?
- are we seeing repeated instances of the same source mesh?

## Proposed model

Split the problem into two caches:

### 1. Geometry cache

Stores canonical object data:

- canonical vertices
- indices / topology
- UVs
- material state keys
- optional normals / inferred attributes

### 2. Instance cache

Stores per-frame object instance data:

- current transform
- visibility
- material variant state
- optional animation/deformation version

The correct long-term runtime path is:

- detect object
- resolve canonical mesh
- update instance transform only

## Auto-detection signals

The following signals can be combined into a confidence score.

### A. Stable source memory pattern

Strong signal if:

- the same CPU/GTE path reads vertices from the same RAM region,
- or from a stable family of adjacent regions,
- across multiple frames.

### B. Stable topology

Strong signal if:

- the same face count / index order / strip pattern appears repeatedly,
- while only transforms differ.

### C. Transform separability

Strong signal if:

- vertices vary between frames in a way explainable by one transform,
- rather than by arbitrary per-vertex mutation.

This is the most important test.

### D. Repeated GTE matrix usage

Useful signal if:

- the same code path emits the same geometry under different RT/TR values,
- suggesting one object reused in different poses or positions.

### E. Temporal recurrence

Promotion should require recurrence:

- same candidate observed for `N` frames or `N` occurrences,
- with stable geometry signature,
- and transform-only variation.

## Promotion pipeline

### Stage 1: candidate

Create a candidate when:

- topology looks stable,
- source path is stable,
- and geometry appears reusable.

Do not cache aggressively at this stage.

### Stage 2: validation

Validate candidate over time:

- compare repeated occurrences,
- estimate whether transform explains the delta,
- reject if per-vertex deformation is too large or inconsistent.

### Stage 3: promotion

Promote to canonical mesh if confidence stays high:

- stable topology,
- separable transform,
- repeated recurrence,
- low invalidation rate.

### Stage 4: instance mode

Once promoted:

- reuse canonical mesh,
- create/update instances,
- bypass full rebuild where possible.

## Invalidation rules

The cache must be invalidated or downgraded if:

- topology changes,
- UV/material state changes incompatibly,
- source memory changes structurally,
- per-vertex motion can no longer be explained by a single transform,
- the object turns out to be CPU-deformed or subdivided dynamically.

This is critical.

A wrong persistent mesh is worse than no cache.

## Classification

Each observed geometry flow should be classified as one of:

- `static_candidate`
- `instanced_candidate`
- `dynamic_deformed`
- `subdivided_dynamic`
- `ephemeral_effect`
- `hud_or_screen_space`

Only the first two classes should be promoted into object-space caching.

## Relation to PSX3D analysis

This cache is not a replacement for the PSX3D correlation/profile system.

It depends on that system.

The expected order is:

1. recover reliable GTE/GPU correlation,
2. identify stable object-space geometry,
3. promote into persistent mesh cache,
4. render instances efficiently.

So:

- `.psx3dprof` remains the correlation knowledge base,
- mesh cache becomes a higher-level optimization / runtime exploitation layer.

## Runtime vs offline split

Auto-detection can happen in both places, but not with the same cost.

### Offline / CLI

Best place for:

- candidate discovery,
- transform separability analysis,
- confidence building,
- profile generation.

### Runtime / UE5 / future VR target

Best place for:

- light validation,
- cheap recurrence checks,
- instance updates,
- conservative cache use.

Heavy promotion logic should be done offline when possible.

## Why this matters for VR

If successful, this strategy changes the problem from:

- "rebuild lots of pseudo-meshes every frame"

to:

- "render persistent objects with changing transforms"

That is much closer to how a real engine wants to operate.

It is likely a necessary step if the final target is performant PS1-in-VR rendering.

## Open problem

The hardest part is not storing the mesh.

The hardest part is proving automatically that:

- the observed geometry is the same object,
- in a stable reference space,
- and not already camera-relative or CPU-deformed.

That is why promotion must be confidence-based and conservative.

## Recommended next step

Design a concrete "mesh cache candidate signature" with:

- source code path identifiers,
- source memory region identity,
- topology signature,
- transform signature,
- recurrence counters,
- promotion / invalidation thresholds.
