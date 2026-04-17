# GTE/GPU Mode Discovery Method

## Goal

Define a repeatable method to discover how a game builds 3D packets, then turn
that discovery into:

- a generic runtime mode in code
- a per-game profile entry selecting that mode

This replaces ad-hoc recovery logic with explicit, testable modes.

## Core Principle

For a difficult game-specific case, do not start from the final rendered mesh.

Start from:

1. the real GTE producer zone
2. the real GPU packet/OT fill zone
3. the rule linking the two

That gives:

`GTE zone(s) + GPU fill zone(s) + link rule = reconstruction mode`

## Step 1: Find The Real Runtime Producer

Do not trust the CPU `PC` sampled at VBlank.

Useful runtime signals are:

- DMA writer PCs for words later consumed by DMA2/GPU
- top producer PCs for:
  - `quad_edge`
  - `quad_partial`
  - other reconstruction modes
- optional GTE trace summaries limited to a known frame window

In practice, these runtime analysis steps should be driven mainly from the CLI
emulator MCP path, not from UE5.

The reference analysis path is:

- `r3000_emu.exe --mcp-stdio` for discovery and summaries
- Ghidra for structural confirmation
- UE5 only afterwards for render-side validation when needed

For Ridge Racer intro flag, this method identified:

- hot producer instruction: `0x800264B0`
- enclosing function: `FUN_80026110`

The VBlank-time `PC` (`0x8004E9A0`) was unrelated to packet production.

## Step 2: Find The Real GTE Zone

Trace only structural GTE ops inside a useful frame window:

- `RTPT`
- `RTPS`
- `NCLIP`
- `NCT`
- `NCS`
- `AVSZ4`

Then aggregate:

- top PCs
- top opcodes

This gives the actual GTE code zones involved in the problematic scene, without
tracing the whole boot.

## Step 3: Find The Real GPU Fill Zone

Open the hot producer instruction in Ghidra and determine:

- packet format written in RAM
- coordinate write pattern
- color/UV write pattern
- OT insertion helper

Typical helper patterns:

- projection helper using GTE
- packet write loop
- `addPrim()`-style OT insertion

Important:

- this is a place where Ghidra may be more efficient than runtime-only tooling
- especially for custom engines with macro-ized OT insertion or packet builders
- the runtime should tell us which producer zone matters
- Ghidra should explain the actual structure of that zone

## Step 4: Define The Link Rule

Once the GTE and GPU zones are known, define the link rule explicitly.

Examples:

- `RTPT + RTPS -> GT4`
- `2xRTPT shared-edge -> GT4`
- `edge-strip built from adjacent tagged faces`
- `face-pair from V3 hint`

The link rule must be deterministic and describable in code and in the game
profile.

If the runtime evidence is still ambiguous, use Ghidra to confirm:

- whether two packet segments come from one logical object or two
- whether a helper writes one primitive family or several
- whether a matrix root or object struct is shared across the packet flow

## Step 5: Create An Explicit Mode

Do not solve the case with fallback logic.

Create an explicit mode, for example:

- `rtpt_rtps_gt4`
- `rtpt_pair_grid_gt4`
- `edge_strip_tagged`
- `face_pair_v3_hint`

The mode should specify:

- how the source geometry is produced
- how packet identity is matched
- how the final quad/triangle set is reconstructed

## Step 6: Store The Discovery In The Profile

The profile should keep validated knowledge such as:

- relevant GTE PC ranges
- relevant GPU fill PC ranges
- selected reconstruction mode
- optional memory structure notes

This is game knowledge, not runtime fallback.

## Ridge Racer Example

For the intro flag:

- hot packet producer instruction: `0x800264B0`
- enclosing function: `DrawFlag` (`0x80026110`)
- GTE helper: `GTE_Calc_Flag` (`0x800477A4`)
- OT helper: `AddOT` (`0x80043F38`)

Observed structure:

- repeated `RTPT`
- packet built in RAM as a GT4 grid
- OT insertion through `addPrim()`
- packet identity dominated by one edge face token, while the second edge may
  need to be recovered from the projected packet segment

This indicates a mode closer to:

- `paired_edge_rtpt_gt4`

not:

- `rtpt_rtps_gt4`

### Practical Link Rule

For this mode, the runtime rule is:

1. identify the dominant edge face from the packet token flow
2. identify the second edge face from:
   - a secondary face token, if preserved
   - otherwise the other projected packet segment
3. build the logical GT4 from the two RTPT edge faces

This is a valid explicit mode.
It is not a fallback.

### Why This Matters

The faulty intro flag window shows:

- `quad_cache=0`
- `quad_edge=312`
- `quad_partial=249`

and the hot producer histogram points almost entirely to the `DrawFlag` packet
builder.

So the unresolved bug is not "missing polygons at random".
It is:

- a specific builder mode is active,
- the runtime recognizes only part of that mode,
- and the remaining packets fall into `face_partial`.

## Rules

- No fallback as a final solution.
- Temporary debug recovery must be clearly marked and disabled by default.
- Prefer explicit generic modes.
- Use the profile to select the validated mode for a given game/scene family.
- Let the LLM choose the tool:
  - runtime first when the question is "what is active now?"
  - Ghidra first when the question is "what is this structure?"
  - both when building a durable mode or future object/mesh cache
