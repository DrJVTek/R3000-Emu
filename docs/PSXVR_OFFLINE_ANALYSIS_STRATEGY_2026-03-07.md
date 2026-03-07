# PSXVR Offline Analysis Strategy (2026-03-07)

## Objective

The real target is not only "make Ridge Racer work".

The real target is:

- a PS1 emulator path that can eventually be fast enough for VR on standalone hardware such as Quest 3,
- with a reconstruction pipeline able to recover useful 3D structure from original games,
- while keeping runtime overhead low enough to remain compatible with future performance work (`JIT x86`, `JIT ARM`, tighter GPU/GTE integration, reduced logging, better scheduling).

## Core idea

The current direction remains valid:

- analyze the game to discover where the useful information lives,
- persist that knowledge in a per-game profile,
- reuse that profile at runtime with minimal overhead.

This is likely a better long-term strategy than trying to infer everything blindly every frame forever.

## Why this direction is attractive

For VR, runtime cost matters much more than development-time cost.

If expensive reasoning is moved offline, then runtime can become:

- deterministic,
- cache/profile driven,
- low-overhead,
- compatible with future JIT execution.

That is exactly the right tradeoff for PSXVR.

## Proposed architecture direction

### 1. Offline analysis should happen primarily in CLI

CLI is the right place for deep analysis because it can host heavy tooling without polluting the runtime path:

- breakpoint-driven exploration,
- long runs,
- richer diagnostics,
- profile generation,
- external reverse-engineering assistance.

This is also the right place to combine:

- emulator traces,
- AI-assisted reasoning,
- external static analysis tools such as Ghidra/MCP-style workflows.

The exact external toolchain can evolve, but the principle is stable:

- static analysis + runtime traces + offline synthesis.

### 2. UE5 should still keep an analysis mode

UE5 analysis mode is still necessary, even if it is not the main analysis engine.

Reason:

- UE5 is interactive,
- some content is easier to reach there than in CLI,
- certain scene states are easier to validate visually than through logs alone.

But UE5 analysis should be treated as:

- a targeted acquisition mode,
- a validation mode,
- a fallback mode for missing coverage.

It should not be the place where the heaviest reverse-engineering logic lives.

### 3. Runtime should become profile-first

The expected runtime path is:

- load game,
- detect identity,
- load per-game profile,
- use known correlation rules with minimal validation,
- only escalate to analysis if coverage is missing.

This means the product becomes less "fully generic at runtime", but more realistic and more performant.

## Important tradeoff

This likely means accepting a more manual compatibility model:

- not every game will become compatible automatically,
- each supported game may need its own analysis file,
- the analysis may require some guided/offline work.

That is not ideal from a purity point of view.

But it is likely the correct engineering compromise if the actual end goal is:

- stable VR rendering,
- low runtime cost,
- predictable performance,
- compatibility that improves game by game.

## Why a fully generic approach is probably not enough

A fully generic dynamic approach has hard limits:

- too much runtime inference cost,
- too many game-specific packet construction patterns,
- too many edge cases in GTE -> CPU -> OT -> GPU transfer,
- too much risk of fragile heuristics that work on one title and fail on another.

For research, generic runtime analysis is useful.

For shipping PSXVR support, generic runtime analysis alone is probably not enough.

## Recommended model

Use a hybrid model:

- Generic runtime foundation:
  - token propagation,
  - hotspot detection,
  - cache validity checks,
  - geometric fallback,
  - camera candidate tracking.

- Offline per-game augmentation:
  - discovered code regions,
  - known packet builders,
  - known GTE/GPU correlation patterns,
  - camera roots,
  - stable compatibility metadata.

This keeps the system reusable while still allowing deep per-game optimization.

## Role of AI + Ghidra/MCP-style tooling

This combination is promising for offline analysis:

- the emulator provides exact execution context,
- static analysis provides code structure,
- AI helps correlate the two and synthesize rules/profiles faster.

This should be seen as an authoring pipeline, not as a runtime dependency.

That distinction matters.

The runtime must stay lean.

The authoring pipeline can be heavy.

## MCP-based analysis pipeline

Using LLMs together with MCP endpoints is a good idea if the role of each tool is kept explicit.

Recommended split:

- Emulator MCP:
  - control CLI execution,
  - set breakpoints/hooks,
  - inspect RAM / registers / DMA / GPU state,
  - capture traces and snapshots.

- UE5 MCP:
  - drive interactive scenes,
  - acquire states that are hard to reach in CLI,
  - validate rendered output,
  - trigger targeted capture in visual contexts.

- GhidraMCP:
  - inspect functions,
  - recover code structure,
  - name candidate systems,
  - correlate static code with runtime hotspots.

- LLM:
  - synthesize runtime evidence with static analysis,
  - propose candidate correlations,
  - generate or refine `.psx3dprof` content,
  - document assumptions and confidence.

This is a good idea only if the LLM is treated as a profile authoring assistant, not as a runtime dependency.

## Why this is useful

This pipeline can produce better profiles than runtime-only heuristics because it can combine:

- exact execution traces,
- static code understanding,
- scene-level validation,
- human review when needed.

That is particularly useful for:

- GTE/GPU correlation,
- camera root discovery,
- packet builder identification,
- subdivision logic,
- game-specific special cases.

## Important limits

This approach is not "fully automatic generic support".

It is better seen as:

- a semi-automatic compatibility authoring system,
- with reusable generic foundations,
- and per-game knowledge captured into profiles.

That is acceptable if the actual product target is high-quality VR support rather than perfect zero-touch compatibility.

## Lights extraction

Lighting should be treated as a separate analysis product from geometry correlation.

Possible offline targets:

- detect light vectors / colors loaded into GTE or game-side lighting structures,
- identify ambient / directional light roots,
- persist them into the per-game profile,
- map them to UE5 light proxies or material parameters.

This is promising, but should be optional:

- on constrained hardware, full dynamic UE5 lights may be too expensive,
- in many cases a cheaper material-space approximation may be better,
- Quest-class runtime may prefer baked or shader-driven lighting reconstruction.

So the profile should be able to carry lighting information, but runtime policy should decide whether to instantiate real UE lights or cheaper substitutes.

## VR patch layer

A game-specific VR patch layer is also realistic and probably necessary.

This layer is distinct from `.psx3dprof`.

The profile answers:

- where the useful data is,
- how to reconstruct geometry and camera-related semantics.

The VR patch layer answers:

- how to improve playability in VR,
- how to adjust menus / HUD / camera behavior,
- how to modify incompatible assumptions in the original game.

Typical VR patches could include:

- camera behavior overrides,
- HUD repositioning,
- menu depth handling,
- input remapping,
- comfort-related adjustments,
- optional gameplay-centric fixes.

This should be designed as a per-game patch system, not mixed directly into the core profile semantics.

## Practical conclusion

The likely winning strategy is:

1. keep improving the generic runtime foundation,
2. move deep reverse-engineering and profile generation to CLI/offline tools,
3. use MCP-driven tooling (`CLI`, `UE5`, `Ghidra`) as an authoring pipeline,
4. preserve a lighter UE5 analysis mode for acquisition and validation,
5. accept per-game compatibility profiles as a first-class concept,
6. keep lighting extraction and VR patching as explicit secondary layers,
7. optimize the final runtime path for future VR constraints.

## Consequence for current work

Current PSX3D analysis/profile work is still useful, but it should gradually evolve toward:

- stronger per-game profile semantics,
- better offline generation,
- cleaner separation between "authoring analysis" and "runtime use".

That separation is probably necessary if the long-term target is true PSX-in-VR on constrained hardware.
