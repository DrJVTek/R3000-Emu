# Debug Research: UE5 3D GPU Component Bugs (Ridge Racer)
> Date: 2026-03-04 | Branch: `002-ue5-3d-materials`

## Summary

Investigation of 5 bugs observed in UE5 rendering of Ridge Racer (U) via the R3000-Emu PS1 emulator. The core 3D reconstruction pipeline (Shadow GTE → Shadow GPU → UE5 ProceduralMesh) is **functional** — the issue was initially misdiagnosed as "zero origin_3d" but that was a log-reading error.

## Pipeline Verification

### CLI vs UE5 Comparison
| Phase | CLI (30s run) | UE5 (system.log) |
|-------|---------------|-------------------|
| BIOS boot (640×480) | frames 0-~300: env cmds only | frames 0-~28: 640×480 PAL |
| PS logo (640×480) | frame 300: 396 tris, 0 3D | frames ~29-~100: 396 tris |
| **PS logo 3D** (640×480) | frame 300+: 280 cmds, 278 3D | frames ~311+: 280 cmds, 278 3D, 2 sections |
| Galaxian (320×240) | N/A (stuck before) | frames ~200+: various |
| **Gameplay** (320×240) | frame 600: 1242 cmds, **1120 3D** | frame 524+: 1212 cmds, **1120 3D**, 3 sections |
| After loading | frame 1200+: 0 cmds (stuck) | Similar stuck pattern |

**Key finding**: Both CLI and UE5 produce identical 3D data. All 5 bugs are **rendering/material issues in UE5**, not pipeline issues.

---

## Bug 1: HD/SD Resolution Mismatch

### Symptom
Sony/PlayStation logo renders at 640×480 (h_res=3, v_res=1), game renders at 320×240 (h_res=1, v_res=0). In UE5, the 2D overlay changes physical size when resolution switches.

### Root Cause
In `R3000Gpu3DComponent.cpp`, the 2D origin is derived from display resolution:
```cpp
const float OriginX = 0.5f * DL.display.width();   // 320 or 160
const float OriginY = 0.5f * DL.display.height();   // 240 or 120
```
And the auto-scale uses:
```cpp
EffScale2D = Last3DExtentY_ / DisplayW;  // varies with resolution
```
When resolution changes from 640→320, the 2D content shrinks by 2× because `OriginX/Y` halves and `EffScale2D` changes.

### Log Evidence
```
Frame 311 (640×480): Origin2D=(320.0,240.0) EffScale2D=1.0000
Frame  29 (320×240): Origin2D=(160.0,120.0) EffScale2D=0.3797
```

### Fix Direction
Normalize all 2D content to a fixed reference resolution (e.g., always treat as 320×240 or always 640×480). The scale factor should be independent of the PS1's display mode.

---

## Bug 2: Black Square on PlayStation 3D Logo

### Symptom
A large black quad appears on/around the 3D PlayStation logo.

### Root Cause
The PS1 game sends a **full-screen clear rectangle** as a GP0 polygon before drawing the logo:
```
2D TRI[0] frame=311 origin=1 (origin_2d_hud)
  v[0] screen=(0,0)   -> UE(-50.0, -320.0, 240.0) color=(0,0,0)
  v[1] screen=(640,0)  -> UE(-50.0, 320.0, 240.0)  color=(0,0,0)
  v[2] screen=(0,480)  -> UE(-50.0, -320.0, -240.0) color=(0,0,0)

2D TRI[1] frame=311 origin=1
  v[0] screen=(640,0)  -> UE(-50.0, 320.0, 240.0)   color=(0,0,0)
  v[1] screen=(640,480) -> UE(-50.0, 320.0, -240.0)  color=(0,0,0)
  v[2] screen=(0,480)  -> UE(-50.0, -320.0, -240.0) color=(0,0,0)
```
This is a 640×480 black quad at UE X=-50.0 (behind the 3D logo at X≈480-650). However, in the UE5 scene it's still visible as an opaque black plane spanning Y=[-320,320] Z=[-240,240].

### Frame Structure
```
Section[0]: mat=0 (2D) 6 verts 2 tris    ← Black clear-screen quad
Section[1]: mat=5 (3D) 834 verts 278 tris  ← PlayStation logo
```

### Fix Direction
Options:
1. **Filter clear-screen quads**: Detect when a 2D quad covers the entire display area (screen coords match display bounds) and skip it
2. **Material transparency**: Make the 2D material translucent or add depth-test-only for clear-screen detection
3. **Simply skip 2D prims for 3D frames**: If a frame has origin_3d content, skip origin_2d_hud that cover the full screen

---

## Bug 3: Ridge Racer Menu — Missing 3D Flag + Wrong Texture Format (Green)

### Symptom
- The Ridge Racer menu should show a 3D waving checkered flag — it doesn't appear
- 2D menu textures show as green instead of proper colors

### Analysis: Missing 3D Flag
The menu frames (around frame 200+) show **396 tris, all 2D** (396 2D skip):
```
Frame 211: Total: 396 cmds, 396 3D tris, 396 2D skip, 1 sections
```
All 396 primitives are classified as `origin_2d_hud` or `origin_2d_rect`. The waving flag should be origin_3d (GTE-tagged) but isn't being tagged.

Possible causes:
1. The flag animation might happen at a different point in the boot sequence that we're not reaching
2. The flag might use a different rendering technique (e.g., sprite-based animation, not GTE 3D)
3. The game may need more progress (CD loading) before the flag appears — our emulator gets stuck

### Analysis: Green Textures
The VRAM IS available (passed as `VramTexture_` to material instances). The issue is the texture **format interpretation** in the UE5 material shader.

Textured polygons during BIOS phase (frame 125):
```
tp=0x000B clut=0x602C flags=0x01 semi=0 tex=0  → 4-bit CLUT mode
tp=0x000A clut=0x6028 flags=0x01 semi=0 tex=0  → 4-bit CLUT mode
```
- `tex_depth=0` → 4-bit CLUT (extracted from `(texpage >> 7) & 3`)
- `TexMode = tex_depth + 1 = 1` passed via UV3.x to the material
- CLUT positions: (0x2C*16, 0x602C>>6) and (0x28*16, 0x6028>>6)

The C++ UV packing is identical between `R3000GpuComponent` (2D) and `R3000Gpu3DComponent` (3D):
- UV0 = texture UV coords
- UV1 = texpage base (X*64, Y*256)
- UV2 = CLUT position (X, Y)
- UV3.x = TexMode (0=none, 1=4bit, 2=8bit, 3=15bit)
- UV3.y = packed flags (semi_mode, semi_enable, raw_texture)

**Root cause**: The material shader (`M_PlayPoly0` / `M_PlayPoly0_3D`) likely doesn't correctly handle 4-bit CLUT mode (TexMode==1). It may be treating 4-bit data as 8-bit or 15-bit, causing wrong color lookup. Needs comparison between the 2D material (which may work correctly) and the 3D material.

### Fix Direction
- For missing 3D flag: investigate if the game reaches the menu flag animation (may be a loading/stuck issue — Galaxian mini-game runs during loading, menu appears AFTER loading completes)
- For green textures: verify the UE5 material shader handles `TexMode==1` (4-bit CLUT) correctly — compare with working 2D material if applicable

---

## Bug 4: Ridge Racer Demo — Zero 3D Polygons (During Gameplay)

### Symptom
User reports no 3D polygons during the gameplay demo.

### Analysis: ACTUALLY WORKS!
The logs show **1120 origin_3d** triangles per frame during gameplay:
```
Frame 524: Total: 1212 cmds, 1212 3D tris, 92 2D skip, 3 sections
  Section[0]: mat=0 (2D) 42 verts 14 tris
  Section[1]: mat=5 (3D) 3360 verts 1120 tris  ← 3D gameplay!
  Section[2]: mat=0 (2D) 234 verts 78 tris
```

3D BBox: X=[124.5..131.5] Y=[-56.0..52.0] Z=[-40.0..40.0]

**This bug might be visual** — the 3D content exists but might not be visible due to:
1. Camera position in UE5 (3D content at X≈125 might be out of view)
2. Material issues (3D section uses mat=5 which might be transparent/missing)
3. The 3D bounding box is very small (X range = 7 units) compared to the 2D content (X range = 205 units)
4. The `display.enabled=0` during frame 524 — display might be mid-swap

### Root Cause: Degenerate Quad Triangles (FIXED)
Ridge Racer uses **RTPT-only** (no RTPS) for vertex transformation. Each RTPT produces 3 tagged vertices, and GP0 quads combine vertices from 2 different RTPTs. The quad cache (designed for RTPT+RTPS or shared-vertex RTPT+RTPT patterns) had 0 hits, causing all quad_half=1 triangles to collapse (V3≈V1 fallback).

**Fix**: Decode V3's face_idx directly from its tagged screen coordinates:
- **Carrier V3** (56%): V3 is SXY0/SXY2 of another RTPT → decode face_idx_B from carrier value, use vertex 0
- **Reference V3** (44%): V3 is SXY1 (REF_BASE) → use face_idx ± 1 heuristic, vertex 1

Result: **560/560 quad hits, 0 misses** per frame. All 1120 3D triangles have proper vertex positions.

### Remaining visibility issues
- Camera position in UE5 (3D content at X≈125 might be out of view)
- 3D bounding box is small (X range = 7) compared to 2D content (X range = 205)

---

## Bug 5: Galaxian Material Bugs (Low Priority)

### Symptom
Minor material rendering issues in Galaxian mini-game.

### Status
Not investigated. Low priority per user.

---

## Architecture Reference

### 3D Reconstruction Pipeline
```
CPU → COP2 (GTE) → Shadow GTE (Gte3D) → tags SXY with face_idx
         ↓ (MFC2/SWC2 intercept for SXY12-15)
Game code stores tagged coords to OT
         ↓ (DMA2 linked-list)
Primary GPU renders 2D    Shadow GPU (Gpu3D) decodes face_idx
                              ↓ (push_triangle with PrimOrigin)
                          FrameDrawList (cmds_3d array)
                              ↓ (copy_ready_draw_list)
                          UE5 R3000Gpu3DComponent::RebuildMesh3D()
                              ↓ (ProceduralMeshComponent sections)
                          UE5 Rendered Scene
```

### Key Files
| File | Purpose |
|------|---------|
| `src/gte/gte_3d.cpp` | Shadow GTE: RTPS/RTPT tagging, face cache |
| `src/gpu/gpu_3d.cpp` | Shadow GPU: GP0 parser, face_idx decode, push_triangle |
| `src/gpu/gpu.h` | DrawCmd, DrawCmd3D, FrameDrawList, PrimOrigin |
| `src/r3000/cpu.cpp:4004` | MFC2/SWC2 SXY intercept for shadow GTE |
| `src/r3000/bus.cpp:1298,1343,1533` | GP0/GP1 forwarding to shadow GPU |
| `src/emu/core.cpp:202-205` | Shadow GTE/GPU wiring |
| `integrations/ue5/.../R3000Gpu3DComponent.cpp` | UE5 mesh reconstruction |

### Display Modes
| Phase | Resolution | h_res | v_res | PAL | Frames |
|-------|-----------|-------|-------|-----|--------|
| BIOS boot | 640×480 | 3 | 1 | yes | 0-~28 |
| Game init (Galaxian/menu) | 320×240 | 1 | 0 | no | ~29-~400 |
| PS logo | 640×480 | 3 | 1 | yes | ~300-~370 |
| Gameplay demo | 320×240 | 1 | 0 | no | ~500+ |

### Diagnostic Tools Added
- `GPU3D_VBLANK` log: per-frame GP0 word/cmd/tri counts + state
- `GPU3D_DIAG` log: per-polygon raw coords and face_idx decode (first 5/frame)
- CLI flag: `--3d-diag` for detailed 3D pipeline stats

---

## Next Steps

1. **Bug 1 (Resolution)**: Implement fixed reference resolution for 2D scaling
2. **Bug 2 (Black square)**: Filter full-screen clear quads in RebuildMesh3D
3. **Bug 3 (Menu)**: Investigate texture/CLUT rendering in UE5 materials
4. **Bug 4 (Demo 3D)**: Check UE5 camera position and mat=5 visibility — likely a rendering issue since data exists
5. **Bug 5 (Galaxian)**: Deferred
