# Ridge Racer (U) - Ghidra Reverse Engineering Analysis

> **Date**: 2026-02-22
> **Tool**: GhidraMCP (LaurieWired) connected to Ghidra HTTP server (port 8080)
> **Binary**: Ridge Racer (U) PS-EXE loaded in Ghidra
> **Total functions**: 762 (99 GTE macros + 663 game/BIOS functions)

---

## 1. Bug Context: 800 MPH vs 2 MPH

The car speedometer in our emulator shows ~800 mph while DuckStation shows ~2 mph.

**Root cause identified**: `cycle_multiplier_` (default: 2) was declared in `cpu.h` but NEVER used
in `cpu.cpp`. This meant every instruction consumed only 1 cycle instead of 2, causing 2x too many
instructions per VBlank period. The game executes ~33.87M instructions/sec instead of ~16.9M.

**Fix applied** (in `cpu.cpp`):
```cpp
const uint32_t instr_cycles = last_instr_cycles_ * cycle_multiplier_;
last_multiplied_cycles_ = instr_cycles;
```

**Status**: Fix in source, UE5 rebuild pending (touched wrapper .cpp to force).

---

## 2. Key Addresses & Strings

| Address      | Content                    | Usage                          |
|-------------|----------------------------|--------------------------------|
| `80010254`  | `"MAX SPEED    MPH "`      | Menu/settings display          |
| `800101e4`  | `"SPEEDSTER"`              | Game mode name                 |
| `8001029c`  | `"LAP TIME"`               | HUD lap time display           |
| `80079fe8`  | `"CAR #%d"`                | Car selection screen           |
| `800101a8`  | `"RIDGE RACER"`            | Title string                   |
| `80011904`  | `"VSync: timeout\n"`       | VSync wait timeout debug       |
| `80079620`  | `vsync.c v1.15 1995/05/18` | BIOS vsync source ID           |

---

## 3. Game Architecture

### Main Loop (`main` at `0x80011b10`)

```c
// Initialization
DAT_80130cf0 = 0x80;    // timing multiplier (128)
DAT_801d6d28 = 2;       // initial game state
DAT_8007f114 = 0;       // frame counter

do {
    // Double-buffer swap
    DAT_801daf98 = DAT_8007f114 & 1;

    // Setup ordering tables & buffers
    FUN_800418bc(buffer_ptr, 0x2c0);
    FUN_8003a07c();   // GPU/display setup
    FUN_8003a7f8();   // some update

    // DISPATCH: Call current game state handler
    (**(code**)(&dispatch_table + DAT_801d6d28 * 4))();

    // Post-frame
    FUN_80041538(0);  // DrawSync?
    FUN_80037a40();   // swap/display

    // VSync wait: poll hretrace timer until delta >= threshold
    do {
        iVar1 = FUN_8004e824(1);  // returns hretrace delta
    } while (iVar1 < (DAT_80130cf0 * 0xf0) >> 8);  // = 120 scanlines

    FUN_8004e824(0);   // reset hretrace baseline

    // Display list processing
    FUN_8002e0bc();
    FUN_80041ab8(ordering_table);
    FUN_80041c10(ordering_table + 0x5c);
    FUN_80041a48(ordering_table + 0x70);

    DAT_8007f114++;    // frame counter
} while (true);
```

### Dispatch Table (at `0x8006e7bc`)

| State | Address     | Function       | Purpose (estimated)          |
|-------|------------ |----------------|------------------------------|
| 0     | 8006e7bc    | (unknown)      |                              |
| 1     | 8006e7c0    | (unknown)      |                              |
| 2     | 8006e7c4    | `0x8001c824`   | Initial state → sets state 3 |
| 3     | 8006e7c8    | `0x8001ca1c`   | (transition)                 |
| 4     | 8006e7cc    | `0x8003d25c`   | Loading → sets state 5       |
| 5     | 8006e7d0    | `0x8003d380`   | ?                            |
| 6     | 8006e7d4    | `0x8001ea10`   | ?                            |
| 7     | 8006e7d8    | `0x8001eb50`   | ?                            |
| 8     | 8006e7dc    | `0x8002ace8`   | Replay save?                 |
| 9     | 8006e7e0    | `0x8002ae20`   | ?                            |
| 10    | 8006e7e4    | `0x8001ca1c`   | (same as state 3)            |
| ...   | ...         | ...            | (many more states)           |

Game state is stored in `DAT_801d6d28` — a `uint16` that changes frequently during transitions.

### VSync / Timing System

**VSync counter**: `DAT_8007961c` (incremented by VBlank IRQ callback at `0x8004e764`)

**Timer init** (`FUN_8004e6b4`):
- TMR1 mode = `0x107`: sync mode 1 (reset at HBlank), count HBlanks, IRQ on target
- Installs VBlank callback via `FUN_8004dd80(0, 0x8004e764)`

**VSync function** (`FUN_8004e824`):
- `param_1 = 1`: Returns hretrace timer delta (scanlines since last reset)
- `param_1 = 0`: Resets hretrace baseline, waits for VBlank boundary
- `param_1 > 1`: Full VSync with timeout (waits N VBlanks)

**Frame timing**: Main loop waits for hretrace delta ≥ `(DAT_80130cf0 * 0xf0) >> 8`:
- Default: `(0x80 * 0xf0) >> 8 = 120 scanlines`
- NTSC has 262 scanlines/frame → ~46% of a frame
- Effective target: ~130 Hz (but `DAT_80130cf0` may change during gameplay)

### VSync Timeout (`FUN_8004e96c`)
```c
void FUN_8004e96c(int target_vblank, int timeout) {
    timeout = timeout << 15;
    do {
        if (target_vblank <= VBlankCounter) return;
        timeout--;
    } while (timeout != -1);
    printf("VSync: timeout\n");
    ChangeClearPAD(0);
    ChangeClearRCnt(3, 0);
}
```

---

## 4. Key Functions Found

| Function       | Address      | Role                                    |
|---------------|-------------|------------------------------------------|
| `main`        | `80011b10`  | Main game loop + dispatch                |
| `FUN_8004e824`| `8004e824`  | VSync polling (hretrace timer)           |
| `FUN_8004e96c`| `8004e96c`  | VSync wait with timeout                  |
| `FUN_8004e6b4`| `8004e6b4`  | Timer/VBlank init                        |
| `FUN_8001d590`| `8001d590`  | Menu: "MAX SPEED MPH" display            |
| `FUN_8001d790`| `8001d790`  | Car selection screen (tachometer needle)  |
| `FUN_8001fd10`| `8001fd10`  | HUD: "LAP TIME" display                  |
| `FUN_80027ed4`| `80027ed4`  | Text rendering (x, y, string, color)     |
| `FUN_8002abb0`| `8002abb0`  | Replay data save (`sim:replay.dat`)      |

---

## 5. Speed Display Chain (FOUND)

### Speedometer rendering (sprite-based gauge with needle)

**Texture location**: VRAM (336, 64), 4-bit CLUT, 24x24 digit tiles

**Display chain** (in `FUN_8001a9a4` — the HUD renderer):
```c
// DAT_80080234 = car speed (param_1 + 0xA0 in car struct at 0x80080194)
iVar1 = (DAT_80080234 * 0xA0) / 0x92;   // × 160/146 ≈ × 1.096
FUN_8001f780(iVar1 >> 3);                 // ÷ 8, then display
```

**In FUN_8001f780**: `display_value = param * 0.625` → 3-digit number at (264, 216)

**Total formula**: `displayed_mph = speed * 160/146 / 8 * 0.625 ≈ speed * 0.0856`

### Digit renderer (`FUN_8001f56c`)
- Draws a 24×24 textured quad for each digit (0-9)
- UV X = `(digit % 10) * 24`, UV Y = 0x98
- Called by `FUN_8001f780` for each of 3 digits (hundreds, tens, units)

---

## 6. Physics Engine (FOUND)

### Main physics function: `FUN_800195b0`

**Car struct** (base at `0x80080194` for player):

| Offset | Field | Description |
|--------|-------|-------------|
| +0x24  | angle | Car heading angle |
| +0x82  | gear_index | Current gear (short) |
| +0x84  | rpm | Engine RPM (0-15000, clamped) |
| +0x98  | something | Used in steering calc |
| +0x9C  | car_type | Car type index |
| +0xA0  | **speed** | **Current speed (DAT_80080234)** |
| +0xA4  | speed_delta | Speed increment this frame |
| +0xA8  | heading | Movement direction |
| +0xB0  | torque | rpm × gear_ratio |
| +0xB4  | drive_mode | 0=normal, 1=drift, 2=?, 3=? |
| +0xB8  | braking | Brake state |
| +0xC4  | throttle_state | Throttle detection state |
| +0xC8  | steering_L | Left input |
| +0xCA  | steering_R | Right input |

### Speed update per frame:
```c
// 1. RPM update (acceleration - drag):
rpm = (accel - drag) + rpm;    // clamp to [0, 15000]

// 2. Torque = rpm × gear_ratio:
torque = rpm * gear_ratio_table[gear];

// 3. Speed delta from torque:
speed_delta = torque >> 13;  // or adjusted by mode

// 4. Speed friction (each frame):
if (drive_mode == 1) // drift
    speed = (speed * 0x3E4) / 1000;  // × 0.996 (low friction)
else                  // normal
    speed = (speed * 0x5E) / 100;    // × 0.94 (high friction)

// 5. Write back:
*(param_1 + 0xA0) = speed;           // DAT_80080234
*(param_1 + 0xA4) = speed_delta;
```

### Call chain:
```
main loop dispatch → [unknown race state 0x80014xxx]
  → FUN_8001bd80 (race tick, called for each car)
    → FUN_800195b0 (physics: steering, throttle, RPM, speed, collision)
```

`FUN_8001bd80` is called **5 times** per frame (once per car: player + 4 AI?).

### CRITICAL: `cycle_multiplier_` does NOT affect speed

**Analysis**: The worker thread gives `CycleDebt = DeltaTime × 33.87MHz`. With multiplier=2,
each instruction costs 2 cycles → 16.9M instructions/sec. Without multiplier, each costs 1
→ 33.87M instructions/sec. **BUT** in both cases, bus.tick() receives 33.87M cycles/sec total.
Timers, VBlank, and TMR1 run at the same rate. The game loop is frame-locked by VSync wait.
Physics runs once per frame regardless.

**The real cause of 800 mph is STILL UNKNOWN.** cycle_multiplier changes instruction throughput
but not timing.

---

## 7. Outstanding Questions

- [x] Where is the speedometer rendered? → `FUN_8001f780` at (264, 216), 3 digits
- [x] Which function is the physics handler? → `FUN_800195b0`
- [x] What is the speed variable? → `DAT_80080234` = car_struct + 0xA0
- [ ] **Why 800 mph?** The physics runs once per frame, timing seems correct
- [ ] Does the speed friction `× 0.94` work correctly? (DIV instruction)
- [ ] Is there a sub-frame physics loop we're missing?
- [ ] Could an instruction bug cause the wrong friction/acceleration values?
- [ ] Large unidentified code region 0x80014300-0x80015254 needs analysis

---

## 8. Menu Flag GT4 Loop (Update 2026-03-08)

Une nouvelle lecture Ghidra/ASM a isole la boucle du drapeau menu:

- **Adresse**: `0x80046B18`
- **Nom Ghidra**: `RR_MenuFlagQuadDrawLoop`

### Pattern observe

La boucle ne correspond pas a un edge-strip reconstruit a posteriori.
Elle suit un vrai schema `GT4`:

1. charger 3 sommets
2. `RTPT`
3. `gte_stsxy3_gt3`
4. charger le 4e sommet
5. `RTPS`
6. `gte_stSXY2`
7. `AVSZ4`
8. lighting (`NCT/NCS`)
9. insertion dans l'ordering table

### Consequence directe pour le code C++

Le drapeau menu doit etre traite comme un mode explicite:

- `rtpt_rtps_gt4`

et non comme:
- `edge_strip`,
- ou un fallback tardif dans `Gpu3D::push_quad()`.

### Point critique retrouve dans le pipeline

Quand `Gpu3D::push_quad()` ne trouve pas de `GteCacheQuad` pour un GT4,
le code peut encore tomber sur un mode partiel pour le 2e triangle:

```cpp
fill_cmd3d_from_face(cmd3d, face, 1, 1, 2);
```

Ce mode partiel explique visuellement:
- le drapeau coupe en deux,
- la surface vrillee,
- et le rendu "couche" au lieu d'un vrai panneau face camera.

### Conclusion

Le probleme principal restant n'est pas dans UE5.
Il est dans le pipeline de reconstruction:

- soit le producteur `Gte3D` ne construit pas tous les `GteCacheQuad`
  attendus pour ce mode `RTPT+RTPS`,
- soit le consommateur `Gpu3D` route encore une partie de ces GT4 vers un
  mode incomplet.

---

## 7. GTE Bugs Found (Separate from Speed Bug)

From comparison with DuckStation's `gte.cpp`:

| Register | Issue | Impact |
|----------|-------|--------|
| 15 (SXYP) | Write should push SXY FIFO | Incorrect vertex projection results |
| 28 (IRGB) | Write should set IR1/2/3 from packed RGB | Color interpolation broken |
| 29 (ORGB) | Read should compute from IR1/2/3 | Color readback wrong |
| 30 (LZCS) | Write should compute LZCR | Leading zero count broken |
| 31 (LZCR) | Read should return computed count | Affects LOD/ordering |
| 1,3,5,8-11 | Write should sign-extend 16-bit | Signed math issues |

These are real bugs but NOT the cause of the 800 mph issue.

---

## 8. GhidraMCP Setup

- **Plugin**: Installed in Ghidra (Java plugin, HTTP server on port 8080)
- **Bridge**: `C:/tmp/GhidraMCP-release-1-4/bridge_mcp_ghidra.py`
- **Config**: `.mcp.json` in project root (but MCP tools not loaded this session — used curl instead)
- **API reference**: See `bridge_mcp_ghidra.py` for all endpoints (GET with query params or POST with body)

### Key API Endpoints:
```
GET  /list_functions                    → all functions
GET  /searchFunctions?query=<term>      → search by name
GET  /strings?filter=<text>&limit=N     → search strings
GET  /xrefs_to?address=0x<addr>         → cross-references TO address
GET  /xrefs_from?address=0x<addr>       → cross-references FROM address
POST /decompile  body=<function_name>   → decompile function
GET  /decompile_function?address=0x<addr> → decompile by address
GET  /disassemble_function?address=0x<addr> → disassembly
GET  /data?offset=N&limit=N             → labelled data items
```
## 2026-03-08 - Runtime Producer Identified

The bad intro/menu flag phase is now tied to a real runtime producer, not the
CPU PC sampled at VBlank.

Observed during the faulty window (`frame=540/600/660/720`):

- `quad_cache=0`
- `quad_edge=312`
- `quad_partial=249`
- `GPU3D_EDGE_PC top[0] pc=0x800264B0 count=312`
- `GPU3D_PARTIAL_PC top[0] pc=0x800264B0 count=248`
- `GPU3D_PARTIAL_PC top[1] pc=0x80019214 count=1`

Important:

- `0x8004E9A0` seen in `GPU3D_VBLANK` is only the CPU PC at VBlank time.
- It is not the geometry-building loop.
- The real hot producer is `0x800264B0`, which lies inside `FUN_80026110`.

Ghidra decompilation shows:

- `FUN_80026110` builds and submits a waving GT4 grid packet sequence.
- `0x800264B0` is one of the packet coordinate stores (`swr`) into the GPU packet.
- The helper at `0x800477A4` is a true GTE projection wrapper:
  - `gte_ldv3`
  - `gte_rtpt_b`
  - `gte_stsxy3`
  - `gte_stSZ3`
- The helper at `0x80043F38` is `addPrim()`.

Loop structure inside `FUN_80026110`:

- outer loop: `0x14` iterations (`local_68 = 1 .. 0x14`) -> 20 rows
- inner loop: `0x1c` iterations (`s6 = 1 .. 0x1c`) -> 28 quads per row
- total expected grid quads: `20 * 28 = 560`

This lines up with runtime counts:

- `312 edge + 249 partial = 561`

So the current shadow pipeline is effectively reconstructing about one full
quad-grid worth of packets, but almost all of it is going through the wrong
mode (`edge` + `partial`) instead of a true quad-cache mode.

The marginal `0x80019214` source belongs to a separate packet builder
(`FUN_80019064`) and only contributes one isolated partial packet in this
window.

## 2026-03-08 - Intro Flag Mode Identified

The visible intro flag bug is not primarily a `RTPT + RTPS -> GT4` case.

The active builder for the bad window is:

- `DrawFlag` (`0x80026110`)
- hot packet store inside it: `0x800264B0`
- GTE helper: `GTE_Calc_Flag` (`0x800477A4`)
- OT helper: `AddOT` (`0x80043F38`)

### Builder Pattern

`DrawFlag` builds a regular waving flag grid:

- outer loop: `20`
- inner loop: `28`
- expected quads: `560`

For each cell, it:

1. calls `GTE_Calc_Flag`
2. updates the next edge of the grid
3. calls `GTE_Calc_Flag` again
4. writes a GT4 packet in RAM
5. submits through `AddOT`

So the practical mode is:

- `paired_edge_rtpt_gt4`

### Runtime Symptoms

During the faulty intro window:

- `quad_cache=0`
- `quad_edge=312`
- `quad_partial=249`

After adding the first explicit segment-based link rule:

- a subset now resolves through `face_b_src=segment_link`
- but the counts only improve slightly:
  - `quad_edge=313`
  - `quad_partial=248`

Conclusion:

- the mode is real
- the first implementation of the mode is incomplete
- most failing packets still do not recover the second edge correctly

### Working Hypothesis

The remaining failures are likely due to packet segment orientation/order.

Current `segment_link` assumes the packet exposes the two edge segments as:

- `(x0,y0)-(x1,y1)`
- `(x2,y2)-(x3,y3)`

But `DrawFlag` packet writes likely use more than one effective segment layout.

So the next correction should target:

- exact packet coordinate order in `DrawFlag`
- exact mapping from packet segment to dominant/secondary edge face

not:

- UE5 rendering
- VBlank-time CPU PC
- arbitrary fallback logic

## 2026-03-08 - Title / Flag Init Chain

Current title/menu chain recovered from Ghidra:

- `FUN_8003ebbc`
  - reset/title setup
  - calls `FUN_800260c0`
- `FUN_800260c0`
  - initializes the two flag data sets
  - calls `FUN_80025fe4(0)`
  - calls `FUN_80025fe4(1)`
  - calls `FUN_80025df0(&DAT_80131c84)`
  - calls `FUN_80025df0(&DAT_80154878)`
- `DrawFlag` (`0x80026110`)
  - true flag packet builder
  - hot packet store at `0x800264B0`
- `GTE_Calc_Flag` (`0x800477A4`)
  - projection helper used twice per flag cell
- `AddOT` (`0x80043F38`)
  - OT insertion helper

This gives the validated method for future cases:

1. identify the real packet producer PC at runtime
2. map it to the containing builder function in Ghidra
3. recover the GTE helper(s) used by that builder
4. recover the OT insertion helper
5. encode the resulting builder pattern as a mode rule in the per-game profile

For the intro flag, the packet order written by `DrawFlag` is:

- `packet[0] = prev RTPT edge point 0`
- `packet[1] = prev RTPT edge point 1`
- `packet[2] = current RTPT edge point 0`
- `packet[3] = current RTPT edge point 1`

In other words, this builder writes two real projected edge segments into one GT4 packet. The runtime mode must therefore recover two packet edges, not carrier duplicates.

## 2026-03-08 - Demo / Gameplay Builder Candidate

Validated gameplay-side hot path:

- `DivPloyFT4` (`0x80047E38`)
- hot packet stores:
  - `0x800482A8`
  - `0x800482AC`

This builder is structurally different from `DrawFlag`.

What it does:

- takes one source FT4-like primitive description
- performs repeated color/vertex interpolation with `INTPL`
- runs nested subdivision loops
- projects intermediate vertices with `RTPT`
- clips to screen bounds
- writes multiple child GT4 packets
- inserts them into the OT

So the gameplay/demo case is not:

- `rtpt_rtps_gt4`
- `paired_edge_rtpt_gt4`

It is a different generic mode, closer to:

- `subdivided_ft4_intpl_rtpt`

Key evidence from `DivPloyFT4`:

- outer and inner nested loops controlled by `param_3` and `param_4`
- repeated `gte_intpl_b()`
- repeated `gte_rtpt_b()`
- packet writes at:
  - `0x80048114`
  - `0x80048164`
  - `0x800481A8`
  - `0x800482A8`
  - `0x800482AC`

Implication for the method:

1. the runtime hot `writer_pc` is enough to find the real builder function
2. the builder function tells us which mode family we are in
3. the per-game profile should bind those producer ranges to a mode rule

Best next step for the gameplay loop:

- recover callers/xrefs to `DivPloyFT4`
- identify the main race/demo producer that feeds it
- define a second explicit mode rule for this builder family

Current Ridge Racer profile now carries this second rule as:

- `subdivided_ft4_intpl_rtpt`
- producer range: `0x80047E38-0x800482F4`
- GTE range: `0x80047FFC-0x800482AC`
- OT write range: `0x80048288-0x800482B0`

2026-03-08 update:
- SCUS-943.00.psx3dprof is now the canonical Ridge Racer USA profile.
- Runtime profile saves preserve authored MODE rules instead of overwriting them.
- paired_edge_rtpt_gt4 now takes precedence over the generic quad-cache split when its packet-edge rule matches.
- Expected load log: profile loaded ... game=SCUS-943.00 modes=2.

- 2026-03-08 follow-up: runtime saves now preserve MODE rules, and the paired-edge quad cache stores corners interleaved as [prev0, cur0, prev1, cur1] instead of grouped by edge.

