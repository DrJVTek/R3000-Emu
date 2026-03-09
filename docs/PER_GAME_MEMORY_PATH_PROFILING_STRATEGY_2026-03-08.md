# PSX3D Per-Game Memory Path Profiling Strategy

Date: 2026-03-08
Status: working design note

## Goal

Define a second, more explicit reconstruction strategy for difficult games like Ridge Racer:

- reverse the game code and data flow,
- identify where projected 2D data is written in RAM,
- identify where OT / GPU packets are built from that data,
- reconnect GPU primitives to the original 3D source through per-game knowledge,
- store the result in a per-game profile.

This is not meant to replace the generic pipeline entirely.
It is a complementary strategy for cases where generic tagging/correlation loses information.

## Why this strategy exists

The current generic approach is:

1. intercept GTE output,
2. tag projected 2D coordinates,
3. let the CPU build OT/GPU packets,
4. recover the related 3D face/quad later from the GPU side.

This works when the game preserves enough structure between:
- GTE projected vertices,
- RAM packet staging,
- final OT/GPU packet emission.

Some games, or some code paths inside a game, do not preserve that structure cleanly.
Typical failure modes:
- vertices are projected and stored separately from face assembly,
- intermediate RAM buffers shuffle, merge, or compress the data,
- face construction is split across multiple routines,
- the final GPU packet no longer contains enough direct correlation evidence.

Ridge Racer is a good example of this class of problem.

## Core idea

Instead of relying only on tagged 2D data at the GPU boundary, reverse the per-game CPU path:

1. find where 3D vertices are transformed/projected,
2. find where projected 2D values are stored in RAM,
3. find where those RAM structures are consumed to build OT/GPU packets,
4. identify the exact memory path linking source geometry to final packet emission,
5. encode that path as explicit per-game profile knowledge.

In other words:
- generic path = infer late,
- per-game memory-path strategy = understand early and reconnect deterministically.

## Expected advantages

### 1. More robust on hard games

If a game breaks generic token flow because it separates:
- vertex projection,
- polygon assembly,
- OT insertion,

then a memory-path profile can still reconnect them.

### 2. Better runtime performance

Once the path is known, runtime can use:
- explicit source RAM addresses,
- explicit builder functions,
- explicit structure layouts,

instead of repeated heuristic reconstruction.

That is attractive for Quest 3 / VR where runtime cost matters.

### 3. Better fit for offline authoring

This strategy maps well to:
- CLI automation,
- Ghidra reverse engineering,
- MCP tooling,
- LLM-assisted analysis.

It is heavy to discover, but cheap to consume after profiling.

## Expected disadvantages

### 1. More per-game work

This is not a free generic solution.
It requires game-specific reverse engineering.

### 2. More profile complexity

The profile no longer stores only generic runtime hints.
It may also need:
- RAM structure descriptions,
- function roles,
- packet builder metadata,
- phase-specific paths.

### 3. Harder maintenance

If the game has multiple render paths:
- menu,
- race,
- replay,
- effects,

each may need separate understanding.

## What to reverse concretely

For each important render path, identify:

1. GTE transform/projection routine
- where `RTPT` / `RTPS` are issued,
- which inputs are loaded,
- which outputs are stored.

2. Projected vertex storage
- RAM addresses / structures receiving SXY/SZ,
- layout of projected vertex records,
- whether normals/colors/UV references are carried alongside.

3. Face assembly routine
- where the game groups projected vertices into triangles/quads,
- how it chooses vertex order,
- whether it uses indices, direct copies, or derived patterns.

4. OT/GPU packet builder
- where final GP0 packet words are written,
- textured/untextured path,
- GT3 / GT4 / sprite / line variants,
- ordering table insertion.

5. Phase selection logic
- menu path,
- gameplay path,
- special effect path,
- replay/demo path.

## Example: Ridge Racer flag/menu case

The menu flag loop appears to be a real:
- `RTPT`
- then `gte_stsxy3_gt3`
- then `RTPS`
- then `gte_stSXY2`
- then GT4 packet assembly

That means a strong per-game profile could store:
- the routine address responsible for this loop,
- the RAM staging layout for its projected data,
- the GT4 packet layout and ordering,
- the fact that this path should use an explicit `rtpt_rtps_gt4` mode.

That is stronger than trying to reconstruct the same fact late from partially preserved tags.

## Relationship with the generic system

This strategy should not become a giant hardcoded special-case layer.
It should integrate as:

1. generic runtime modes in code,
2. per-game profile selecting which mode applies,
3. optional per-game memory-path metadata when generic tagging is insufficient.

So the runtime model becomes:
- generic mode engine in C++,
- per-game profile selects/parametrizes modes,
- offline authoring produces the profile.

## Recommended profile content

A profile using this strategy may contain:

### identity
- filename-based game id
- hashes / validation metadata

### runtime_fast_path
- known render modes for important PCs or phases,
- packet builder PCs,
- projected vertex buffer descriptors,
- OT builder descriptors,
- known vertex ordering rules,
- known quad modes,
- confidence / validation version.

### offline_knowledge
- Ghidra function names,
- comments and notes,
- RAM structure hypotheses,
- packet layouts,
- analysis provenance,
- unresolved questions.

## Recommended terminology

To avoid confusion, distinguish clearly:

- `fallback`
  - temporary debug recovery only
- `mode`
  - explicit supported reconstruction strategy
- `profile rule`
  - per-game selection or parameterization of a mode
- `memory-path descriptor`
  - per-game description of how projected data becomes GPU packets

## Proposed runtime modes that fit this strategy

Examples:
- `rtpt_rtps_gt4`
- `consecutive_rtpt_shared2`
- `edge_strip`
- `projected_buffer_indexed_tri`
- `projected_buffer_indexed_quad`
- `ot_builder_copy_path`

The profile should say which one applies, not ask runtime to guess blindly.

## Tooling fit

This strategy is a strong match for the planned toolchain:

- CLI MCP:
  - run game,
  - place hooks,
  - dump RAM ranges,
  - inspect packet builders,
  - export traces.

- Ghidra MCP:
  - rename functions,
  - add comments/bookmarks,
  - inspect xrefs and decompilation,
  - recover structure layouts.

- LLM:
  - correlate dynamic traces with static reverse engineering,
  - propose structure interpretations,
  - generate profile entries,
  - document the result.

- UE5:
  - validate visually after the profile is generated,
  - useful for interactive/debug confirmation,
  - not the main authoring environment for this strategy.

## Practical workflow

1. Reproduce a target phase in CLI.
2. Capture the dominant packet-builder PCs.
3. Use Ghidra to identify the projection and packet-build loops.
4. Trace RAM writes from projected data to OT/GPU words.
5. Identify the exact reconstruction mode.
6. Store the result in the game profile.
7. Validate in CLI first.
8. Validate in UE5 second.

## Engineering recommendation

This strategy is worth keeping and documenting.

It is probably:
- less generic,
- more labor-intensive,
- but more robust for the hardest games,
- and potentially better for final VR runtime performance.

The correct architecture is likely hybrid:
- generic runtime modes,
- per-game profiles,
- offline reverse-assisted authoring,
- minimal runtime guessing.

## Final position

Yes: this approach can be more effective than relying only on late-stage 2D tag recovery.

It should be treated as:
- an offline authoring strategy,
- powered by CLI + Ghidra + MCP + LLM,
- producing explicit per-game profile knowledge,
- consumed by a lightweight runtime.
