# Changelog: `002-ue5-3d-materials` vs `001-ue5-integration` (c9ed133)

## Contexte

- **Branche précédente**: `001-ue5-integration` @ `c9ed133` — "Replace DLL with direct source compilation via symlinks"
- **Cette branche**: `002-ue5-3d-materials` — Shadow GTE/GPU 3D, 10 materials, VramViewer, symlink git

---

## 1. Architecture: Shadow GTE + Shadow GPU (NOUVEAU)

### Avant (001)
- **GteCorrelationTable** dans `src/gpu/gte_correlation.h/.cpp` — table de lookup par coordonnées SXY
- GPU primaire (`Gpu`) fait lookup dans cette table via `gte_corr_->lookup_mut(key)`
- Problème: lookup par SXY fragile (collisions, dépend du timing)

### Après (002)
- **Gte3D** (`src/gte/gte_3d.h/.cpp`) — shadow GTE complet, reçoit les mêmes ops COP2 via `cpu.set_gte_shadow()`
  - **Differential lattice encoding**: encode `face_idx` dans la DIFFÉRENCE entre vertices (carrier - reference)
  - Constantes: `REF_BASE=8192`, `SPACING=8`, dead zone `[2048, 32767]`
  - Résistant aux add/sub uniformes (le jeu peut ajouter un offset, les différences s'annulent)
  - Double-buffered face cache (`write_face_cache_` / `read_face_cache_`) + quad cache
  - Détection de quads depuis 2×RTPT consécutifs (Ridge Racer pattern: 343 quads/frame)
- **Gpu3D** (`src/gpu/gpu_3d.h/.cpp`) — shadow GPU, parser GP0 complet sans rasterization
  - Décode les tags différentiels → `face_idx` → lookup dans le face cache
  - Produit `FrameDrawList` avec `DrawCmd` (2D) + `DrawCmd3D` (3D inline)
  - State machine: idle → collecting → vram_skip → polyline_skip
- **CPU forwarding** (`src/r3000/cpu.cpp`) — MFC2/SWC2 pour SXY regs 12-15 redirigés vers shadow GTE
  - Le jeu lit les coordonnées taguées du shadow GTE (pas les vraies)
  - Toutes les autres lectures COP2: GTE primaire (fidèle)
- **Bus wiring** (`src/r3000/bus.cpp/.h`) — forwarde GP0/GP1 au shadow GPU parallèlement au vrai GPU

### Fichiers nouveaux
| Fichier | Lignes | Description |
|---------|--------|-------------|
| `src/gte/gte_3d.cpp` | 1670 | Shadow GTE complet (22 commandes COP2) |
| `src/gte/gte_3d.h` | 245 | Header: face cache, quad cache, differential encoding |
| `src/gte/igte.h` | 80 | Interface pure virtuelle (shared par Gte + Gte3D) |
| `src/gte/gte_internal.h` | 124 | Helpers partagés (clamp, division, etc.) |
| `src/gpu/gpu_3d.cpp` | 975 | Shadow GPU: GP0 parser + differential decoder |
| `src/gpu/gpu_3d.h` | 106 | Header shadow GPU |
| `src/gpu/igpu.h` | 27 | Interface GPU minimale (GP0/GP1) |

### Fichiers modifiés (core)
| Fichier | Delta | Description |
|---------|-------|-------------|
| `src/emu/core.h` | +11/-5 | Remplace `GteCorrelationTable` par `Gte3D` + `Gpu3D` |
| `src/emu/core.cpp` | +5/-3 | Init shadow systems, wire to bus/cpu |
| `src/r3000/cpu.h` | +5 | `set_gte_shadow()`, pointeur `gte_shadow_` |
| `src/r3000/cpu.cpp` | +14/-2 | Forward COP2 ops to shadow, MFC2/SWC2 redirect |
| `src/r3000/bus.h` | +11 | `set_gpu_3d()`, `set_gte_3d()` |
| `src/r3000/bus.cpp` | +16 | Forward GP0/GP1 to shadow GPU |
| `src/gpu/gpu.h` | +16 | `DrawCmd3D` enrichi: `is_quad`, `quad_half`, normals |
| `src/gte/gte_snapshot.h` | +30 | `GteCacheFace`, `GteCacheQuad`, `GteVertex3D` structs |

---

## 2. UE5: Séparation VRAM + 10 matériaux + SceneComponent

### Avant (001)
- `R3000GpuComponent` = UActorComponent, gère TOUT (VRAM upload + mesh 2D + debug viewer)
- `R3000Gpu3DComponent` = UActorComponent, 5 matériaux (`BaseMaterial`, `MatSemi0-3`)
- Pas de `VramViewerComponent`
- VRAM dupliquée entre 2D et 3D

### Après (002)
- **`R3000VramViewerComponent`** (NOUVEAU) — possède la texture VRAM (1024×512 BGRA8)
  - Upload VRAM chaque frame, partage via `SetVramTexture()` avec GPU et GPU3D
  - Debug viewer optionnel (plane VRAM plein écran)
  - `bShowViewer`, `ViewerMaterial`, `ViewerScale` configurables
- **`R3000GpuComponent`** → hérite `USceneComponent` (était UActorComponent)
  - VRAM retirée, reçue de VramViewer via `SetVramTexture()`
  - Toujours 5 matériaux: `BaseMaterial`, `MatSemi0-3`
  - HD scaling: `EHdDefinition` enum (720p/1080p/1440p/4K/Custom)
- **`R3000Gpu3DComponent`** → hérite `USceneComponent` (était UActorComponent)
  - **10 matériaux** (au lieu de 5):
    - Sections 0-4: `Mat2D_Opaque`, `Mat2D_Semi0-3` (2D/HUD en espace 3D)
    - Sections 5-9: `Mat3D_Opaque`, `Mat3D_Semi0-3` (géométrie 3D reconstruite)
  - `kNumSections = 10`
  - MatIdx routing: `BaseOfs = bHas3D ? 5 : 0`
- **`R3000EmuComponent`** — orchestre le binding:
  - VramViewer → GPU + GPU3D (SetVramTexture)
  - Core → GPU (BindGpu), GPU3D (BindGpu, BindGpu3D)

### Fichiers UE5 modifiés
| Fichier | Delta | Description |
|---------|-------|-------------|
| `R3000Gpu3DComponent.h` | +40/-20 | 10 materials, SceneComponent, kNumSections=10 |
| `R3000Gpu3DComponent.cpp` | +40/-34 | EnsureMaterial[10], MatIdx avec BaseOfs |
| `R3000GpuComponent.h` | -109 | VRAM retiré, SceneComponent |
| `R3000GpuComponent.cpp` | -295 | VRAM upload retiré (dans VramViewer) |
| `R3000EmuComponent.cpp` | +40/-34 | Binding VramViewer + GPU3D shadow |
| `R3000VramViewerComponent.h` | +116 (NEW) | Header VRAM owner + debug viewer |
| `R3000VramViewerComponent.cpp` | +260 (NEW) | VRAM upload, texture sharing |
| `R3000EmuRuntime.Build.cs` | +3/-3 | Include path fix for symlink |

---

## 3. Symlink Git (FIX)

### Avant (001)
- Junction Windows absolue: `Private/src → E:\Projects\github\Live\R3000-Emu\src`
- Non trackée par git (`core.symlinks=false`)
- Invisible sur GitHub

### Après (002)
- **Symlink relatif**: `Private/src → ../../../../../../src`
- `core.symlinks=true`, stocké comme git symlink (mode `120000`)
- Sur GitHub: lien navigable vers `src/`
- Au clone (Windows + Developer Mode): vrai symlink créé automatiquement

---

## 4. Résumé des changements par zone

```
Emulator core (src/)     : +3327 / -11  (shadow GTE/GPU, face cache, quad detection)
UE5 plugin (integrations/): -466 / +549 (VramViewer, 10 mats, SceneComponent, symlink)
Build (CMakeLists.txt)    : +2          (new source files in CORE_SOURCES)
```

## 5. Problèmes connus (cette branche)

1. **Couleurs mosaïque** sur le logo PS en 3D — visible de côté, les faces arrière sont cullées (material single-sided)
2. **Éléments 2D géants** quand `bSkip2DElements=false` — les quads plein écran 2D créent des plans énormes en espace 3D
3. **Coords 2D du shadow GPU** n'ont PAS l'offset de dessin soustrait (le GPU primaire le soustrait, pas le shadow)
4. **Profondeur fixe 2D** (`200.0f * WorldScale`) conflit avec la plage de profondeur 3D
