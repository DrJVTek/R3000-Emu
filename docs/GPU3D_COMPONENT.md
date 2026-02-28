# GPU 3D Component — Architecture & Design

## Overview

The **GTE-GPU 3D Reconstruction Pipeline** extracts original 3D geometry from a running PS1 emulator by correlating GTE (Geometry Transform Engine) projection outputs with GPU polygon draw commands. This enables rendering PS1 scenes in true 3D (e.g. VR) instead of the original flat 2D framebuffer.

### Why This Exists

The PS1 GPU only receives **2D screen coordinates** — the GTE projects 3D vertices to 2D, then the CPU sends those 2D coords to the GPU via GP0 polygon commands. The original 3D data is lost in a normal emulator. By capturing GTE state at projection time and matching it to GPU draw commands, we recover the original 3D vertices and can reconstruct the scene in full 3D.

---

## Architecture

### Pipeline Phases

```
Phase 1: GTE Snapshot Capture
  gte.cpp → capture_snapshot() after RTPS/RTPT
    ↓
Phase 2: Correlation Table Recording
  cpu.cpp → records snapshot in GteCorrelationTable
    ↓
Phase 3: GPU Lookup on Draw
  gpu.cpp → push_triangle() builds SXY key, looks up correlation
    ↓
Phase 4: Draw List with 3D Data
  gpu.h → FrameDrawList contains parallel cmds[] and cmds_3d[]
    ↓
Phase 5: UE5 3D Rendering
  R3000Gpu3DComponent.cpp → reads cmds_3d, builds ProceduralMesh
```

### File Map

| File | Role |
|------|------|
| `src/gte/gte_snapshot.h` | Data structures: `GteVertex3D`, `GteTransform`, `GteSnapshot` |
| `src/gte/gte.h` | GTE class: `capture_snapshot()`, `rtps_vert_fifo_[3]`, `last_snapshot_` |
| `src/gte/gte.cpp` | Snapshot capture after RTPS/RTPT with FIFO accumulation |
| `src/gpu/gte_correlation.h` | `GteSxyKey`, `GteCorrelation`, `GteCorrelationTable` |
| `src/gpu/gte_correlation.cpp` | `record()` and `lookup()` — map SXY screen coords to GTE snapshots |
| `src/gpu/gpu.h` | `DrawCmd3D`, `PrimOrigin`, `FrameDrawList::cmds_3d` |
| `src/gpu/gpu.cpp` | `push_triangle()` — correlation lookup, builds `DrawCmd3D` |
| `src/r3000/cpu.cpp` | Records GTE snapshot after COP2 RTPS/RTPT execution |
| `src/r3000/bus.h` | `gte_correlation()` accessor for CPU→GPU table pointer |
| `src/emu/core.h` / `core.cpp` | Owns `GteCorrelationTable`, wires it to Bus and GPU |
| `integrations/ue5/.../R3000Gpu3DComponent.h` | UE5 component header |
| `integrations/ue5/.../R3000Gpu3DComponent.cpp` | UE5 3D mesh builder |
| `integrations/ue5/.../R3000EmuComponent.cpp` | Wires 3D component to emulator core |

---

## Key Concepts

### 1. GTE SXY FIFO & Correlation Key

The GTE has a 3-element SXY FIFO (shift register). Each `push_sxy` call shifts:
```
SXY0 ← SXY1 ← SXY2 ← new_value
```

After projecting 3 vertices (either via 1x RTPT or 3x RTPS), SXY0/SXY1/SXY2 contain the 3 projected screen coordinates. The GPU receives these same coordinates as polygon vertices.

**Correlation key** = `{sx[0], sy[0], sx[1], sy[1], sx[2], sy[2]}` — the 3 screen coordinate pairs. This key is:
- **Recorded** by the GTE side after each RTPS/RTPT (reading SXY FIFO)
- **Looked up** by the GPU side in `push_triangle()` (using the vertex coords from the draw command, after subtracting draw offset)

The draw offset cancels out: `gp0_polygon` adds it, `push_triangle` subtracts it, so `cmd.v[].x/y == GTE SXY output`.

### 2. RTPS vs RTPT

| Command | Function | Vertices | Usage |
|---------|----------|----------|-------|
| RTPT (0x30) | Projects 3 vertices at once | 3 | BIOS logo, some games |
| RTPS (0x01) | Projects 1 vertex at a time | 1 | Ridge Racer, most games |

**The RTPS accumulation problem**: RTPS only projects 1 vertex, but the GPU receives triangles with 3 vertices. Without accumulation, RTPS snapshots (1 vertex) never match GPU triangle lookups (3 vertices).

**Solution**: After each RTPS, we:
1. Read **all 3 SXY FIFO values** (not just SXY2) — the FIFO naturally accumulates results from consecutive RTPS calls
2. Maintain a parallel **input vertex FIFO** (`rtps_vert_fifo_[3]`) — shifts the original 3D coordinates the same way the hardware shifts SXY
3. Always set `vertex_count = 3` — so the snapshot is treated identically to RTPT

After 3 consecutive RTPS calls, both FIFOs contain the correct data:
```
rtps_vert_fifo_[0] = 1st vertex 3D coords    SXY0 = 1st vertex screen coords
rtps_vert_fifo_[1] = 2nd vertex 3D coords    SXY1 = 2nd vertex screen coords
rtps_vert_fifo_[2] = 3rd vertex 3D coords    SXY2 = 3rd vertex screen coords
```

The **3rd RTPS snapshot** creates the correct 3-vertex key that matches the GPU triangle. Earlier (incomplete) snapshots won't match because SXY0/SXY1 contain stale values from previous triangles.

### 3. DrawCmd vs DrawCmd3D

Each GPU polygon generates a `DrawCmd` (2D) and a parallel `DrawCmd3D`:

```cpp
struct DrawCmd3D {
    PrimOrigin    origin;      // origin_3d or origin_2d_hud
    GteVertex3D   verts_3d[3]; // Original 3D coordinates from GTE input
    uint16_t      sz[3];       // GTE depth values (SZ FIFO)
    GteTransform  transform;   // RT matrix + TR vector at projection time
};
```

- `origin_3d`: GTE correlation found — real 3D geometry
- `origin_2d_hud`: No GTE match — likely HUD/UI drawn with direct 2D coords

### 4. Coordinate Mapping (GTE → UE5)

```
GTE:  X = right,    Y = down,       Z = into screen
UE5:  X = forward,  Y = right,      Z = up

Mapping:
  UE_X =  GTE_Z * WorldScale    (forward = into screen)
  UE_Y =  GTE_X * WorldScale    (right   = right)
  UE_Z = -GTE_Y * WorldScale    (up      = -down)
```

WorldScale default = 0.1 (PS1 GTE units are typically in range ±2000).

---

## UE5 Component: UR3000Gpu3DComponent

### Properties

| Property | Default | Description |
|----------|---------|-------------|
| `bEnabled` | `false` | Enable/disable 3D rendering (2D is primary) |
| `WorldScale` | `0.1` | PS1 GTE units → UE5 units scale factor |
| `WorldOffset` | `(0,0,0)` | World-space offset for 3D geometry |
| `bSkip2DElements` | `true` | Skip HUD/UI (non-GTE-correlated) primitives |
| `bDebug3DLog` | `false` | Log per-frame 3D correlation stats |
| `BaseMaterial` | `nullptr` | Opaque material (required) |
| `MatSemi0..3` | `nullptr` | Semi-transparency materials (4 PS1 blend modes) |

### How It Works

1. **TickComponent**: Checks `Gpu_->vram_frame_count()` for new frames
2. **RebuildMesh3D**: Called on new frame:
   - Thread-safe copy of draw list via `Gpu_->copy_ready_draw_list()`
   - Iterates `cmds[]` and `cmds_3d[]` in parallel
   - Skips `origin_2d_hud` if `bSkip2DElements` is true
   - For `origin_3d`: maps GTE 3D coords to UE5 world coords
   - Groups triangles into material sections (opaque + 4 semi modes)
   - Builds `UProceduralMeshComponent` mesh sections
3. **VRAM Texture Sharing**: Receives shared VRAM texture from 2D component via `SetVramTexture()` — no duplicate 1MB upload

### Material/UV Encoding

UV channels match the 2D component's encoding (same VRAM texture shader):
- **UV0**: Raw U,V from PS1 draw command
- **UV1**: Texpage base (TpBaseX, TpBaseY)
- **UV2**: CLUT position (ClutX, ClutY)
- **UV3**: Texture mode + flags packed

### Winding Order

PS1 uses CW winding, UE5 uses CCW. The component remaps: `{0, 2, 1}`.

### Activation Toggles

Both 2D and 3D components have independent `bEnabled` toggles:
- **2D only** (default): Normal PS1 rendering
- **3D only**: Reconstructed 3D geometry
- **Both ON**: Debug overlay — compare 2D flat vs 3D reconstructed

The 2D component **always updates VRAM texture** even when its mesh is hidden, because the 3D component shares it.

---

## Debug & Troubleshooting

### Log Tags

| Tag | Destination | Content |
|-----|-------------|---------|
| `GPU3D` | system.log | Per-frame stats: 3D tris, 2D skipped, sections |
| `CORE` | system.log | Init: component search, bind, VRAM texture |
| `GTE` | system.log | GTE projection stats |

**Important**: All `emu::logf` calls use `LogLevel::warn` because `EmuLogLevel` is typically set to `warn` in the UE5 Blueprint. Using `info` or `debug` will be silently filtered.

### Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| 0 3D tris, high 2D skipped | RTPS not accumulating | Ensure `rtps_vert_fifo_` shifts correctly |
| 3D tris during logo, 0 after | Game uses RTPS, logo uses RTPT | RTPS accumulation fix required |
| No logs at all | Tag not in routing list | Add "GPU3D" to UELogCallback strcmp |
| Mesh invisible | `BaseMaterial` is NULL | Assign material in Blueprint |
| Wrong orientation | Coordinate mapping wrong | Adjust GTE→UE5 mapping in RebuildMesh3D |
| 0 tris even with bSkip2D=false | Double `continue` bug (fixed) | Both continue paths must be removed |
| CORR MISS with table>0 but key mismatch | RTPS SXY key != GPU vertex key | Check `sign_extend_11` vs GTE saturation |

### Debug: CORR MISS Diagnostic

When `table_size > 100`, failed GPU lookups are logged (first 20):
```
[GPU] CORR MISS #0 frame=500 key=(x,y)(x,y)(x,y) tbl=560 rec=560 hit=0
[GPU]   TBL[0] key=(sx,sy)(sx,sy)(sx,sy) 3D=(vx,vy,vz)(vx,vy,vz)(vx,vy,vz)
```
- **key**: what the GPU is looking for (DrawCmd vertex coords, offset subtracted)
- **TBL**: what the GTE recorded (SXY FIFO values + original 3D vertices)
- **tbl/rec/hit**: table size, recordings this frame, successful matches

### 2D Fallback Mode

When `bSkip2DElements = false`, non-correlated primitives render on a flat plane:
- Fixed depth at `200 * WorldScale` on UE X axis
- Screen coords centered at (160, 120) → UE Y/Z
- Useful for verifying the rendering pipeline works independently of GTE correlation

### Diagnostic Checklist

1. Enable `bDebug3DLog` on the 3D component
2. Check `system.log` for `[GPU3D]` entries
3. Look for "GPU3D connected OK" at startup
4. Verify 3D tri counts > 0 during gameplay
5. Compare `total cmds` vs `3D tris` — ratio indicates correlation hit rate
6. Set `bSkip2DElements = false` to render ALL primitives (flat fallback for uncorrelated)

---

## History

| Date | Change |
|------|--------|
| 2026-02-25 | Initial implementation: GTE snapshot, correlation table, GPU lookup, DrawCmd3D |
| 2026-02-25 | UE5 components: R3000Gpu3DComponent + bEnabled toggles on both 2D/3D |
| 2026-02-25 | Fix: RTPS accumulation — input vertex FIFO + full SXY FIFO read |
| 2026-02-25 | Fix: Double `continue` bug — bSkip2DElements=false now renders 2D fallback |
| 2026-02-25 | Add: 2D fallback rendering (flat plane at fixed depth for uncorrelated prims) |
| 2026-02-25 | Add: CORR MISS diagnostic — logs GPU key vs GTE table entries on mismatch |
| 2026-02-25 | Fix: Log level (warn not info) + GPU3D tag routing |
