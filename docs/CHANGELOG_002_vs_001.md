# Changelog: `002-ue5-3d-materials` vs `001-ue5-integration` (c9ed133 / 5513ca3)

## Contexte

- **Branche précédente**: `001-ue5-integration` @ `c9ed133` — "Replace DLL with direct source compilation via symlinks"
  - Commit précédent significatif: `5513ca3` — "DLL build + GTE-GPU 3D correlation with double-buffered table"
- **Cette branche**: `002-ue5-3d-materials` — Shadow GTE/GPU 3D, 10 materials, VramViewer, symlink git

---

## 1. Architecture: Shadow GTE + Shadow GPU (REMPLACEMENT COMPLET)

### Avant (001 — GteCorrelationTable)
- **GteCorrelationTable** dans `src/gpu/gte_correlation.h/.cpp`
- Pipeline: GTE capture → CPU enregistre snapshot dans table SXY → GPU fait `lookup_mut(key)` par coords SXY
- Le GPU primaire (`Gpu::push_triangle()`) construit une clé SXY et cherche dans la table
- **Problèmes**: collisions SXY, dépend du timing exact, fragile

### Après (002 — Shadow GTE + Shadow GPU)
- **Architecture complètement différente**: 2 systèmes shadow parallèles aux vrais
- Le CPU forward les mêmes opérations COP2 au shadow GTE
- Le Bus forward les mêmes GP0/GP1 au shadow GPU
- Le shadow GTE **modifie les coordonnées SXY** avec des tags différentiels
- Le shadow GPU **décode ces tags** pour retrouver les face_idx

#### Shadow GTE (`Gte3D` — `src/gte/gte_3d.h/.cpp`)
- GTE complet (22 commandes COP2), reçoit les ops via `cpu.set_gte_shadow()`
- **Differential lattice encoding**: encode `face_idx` dans la DIFFÉRENCE entre vertices
- Constantes: `REF_BASE=8192`, `SPACING=8`, dead zone `[2048, 32767]`
- Résistant aux add/sub uniformes (les différences s'annulent)
- Double-buffered face cache + quad cache
- Détection de quads depuis 2×RTPT consécutifs

#### Shadow GPU (`Gpu3D` — `src/gpu/gpu_3d.h/.cpp`)
- Parser GP0 complet sans rasterization (state machine: idle/collecting/vram_skip/polyline_skip)
- Décode tags différentiels → `face_idx` → lookup dans face cache
- Produit sa propre `FrameDrawList` avec `DrawCmd` (2D) + `DrawCmd3D` (3D inline)
- **CRITIQUE**: Le composant UE5 lit maintenant le draw list du **shadow GPU**, pas du GPU primaire

#### CPU/Bus forwarding
- `src/r3000/cpu.cpp`: MFC2/SWC2 pour SXY regs 12-15 redirigés vers shadow GTE
- `src/r3000/bus.cpp`: forwarde GP0/GP1 au shadow GPU parallèlement au vrai GPU

### Fichiers nouveaux (core)
| Fichier | Lignes | Description |
|---------|--------|-------------|
| `src/gte/gte_3d.cpp` | ~1670 | Shadow GTE complet (22 commandes COP2) |
| `src/gte/gte_3d.h` | ~245 | Header: face cache, quad cache, differential encoding |
| `src/gte/igte.h` | ~80 | Interface pure virtuelle (shared par Gte + Gte3D) |
| `src/gte/gte_internal.h` | ~124 | Helpers partagés (clamp, division, etc.) |
| `src/gpu/gpu_3d.cpp` | ~975 | Shadow GPU: GP0 parser + differential decoder |
| `src/gpu/gpu_3d.h` | ~106 | Header shadow GPU |
| `src/gpu/igpu.h` | ~27 | Interface GPU minimale (GP0/GP1) |

### Fichiers modifiés (core)
| Fichier | Delta | Description |
|---------|-------|-------------|
| `src/emu/core.h` | +11/-5 | Remplace `GteCorrelationTable` par `Gte3D` + `Gpu3D` |
| `src/emu/core.cpp` | +5/-3 | Init shadow systems, wire to bus/cpu |
| `src/r3000/cpu.h` | +5 | `set_gte_shadow()`, pointeur `gte_shadow_` |
| `src/r3000/cpu.cpp` | +14/-2 | Forward COP2 ops to shadow, MFC2/SWC2 redirect |
| `src/r3000/bus.h` | +11 | `set_gpu_3d()`, `set_gte_3d()` |
| `src/r3000/bus.cpp` | +16 | Forward GP0/GP1 to shadow GPU |

---

## 2. DrawCmd3D & gte_snapshot.h — Données 3D enrichies

### Avant (001)

```cpp
// gpu.h — DrawCmd3D (5513ca3)
struct DrawCmd3D {
    PrimOrigin origin{PrimOrigin::origin_2d_hud};
    gte::GteVertex3D verts_3d[3]; // Original 3D vertices (from GTE input)
    gte::GteTransform transform;  // RT + TR at projection time
    uint16_t sz[3];               // Depth values
};
```

```cpp
// gte_snapshot.h (5513ca3) — seulement 3 structs
struct GteVertex3D { int32_t vx, vy, vz; };
struct GteTransform { int16_t rt[9]; int32_t tr[3]; };
struct GteSnapshot { ... vertex_count, valid, sequence_id ... };
```

### Après (002)

```cpp
// gpu.h — DrawCmd3D (HEAD) — ENRICHI
struct DrawCmd3D {
    PrimOrigin origin{PrimOrigin::origin_2d_hud};
    gte::GteVertex3D verts_3d[3]; // Original 3D vertices (model space)
    int16_t nx[3], ny[3], nz[3]; // ← NOUVEAU: Per-vertex normals (from GTE NCS/NCT)
    gte::GteTransform transform;  // RT + TR at projection time
    uint16_t sz[3];               // Depth values (screen Z)
    uint32_t face_idx{0xFFFFFFFFu}; // ← NOUVEAU: Face cache index
    bool is_quad{false};             // ← NOUVEAU: True if half of a GP0 quad
    uint8_t quad_half{0};            // ← NOUVEAU: 0 = first tri, 1 = second tri
};
```

```cpp
// gte_snapshot.h (HEAD) — 3 nouvelles structs ajoutées
struct GteCacheVertex { vx,vy,vz, nx,ny,nz, transform, sx,sy, sz };
struct GteCacheFace   { vx[3],vy[3],vz[3], nx[3]..., transform, sx/sy/sz[3] };
struct GteCacheQuad   { vx[4],vy[4],vz[4], nx[4]..., transform, sx/sy/sz[4] };
```

### Impact UE5
- Les normals `nx/ny/nz` sont **disponibles** dans DrawCmd3D mais **PAS encore utilisées** par RebuildMesh3D
- `face_idx` et `is_quad`/`quad_half` sont **disponibles** mais **PAS encore exploités** dans UE5
- Donc visuellement: **aucun changement** dû à ces nouveaux champs

---

## 3. R3000Gpu3DComponent — Rendering UE5

### Ce qui a CHANGÉ

| Aspect | Avant (001) | Après (002) |
|--------|-------------|-------------|
| **Héritage** | `UActorComponent` | `USceneComponent` |
| **Matériaux** | 5 slots (`BaseMaterial` + `MatSemi0-3`) | 10 slots (`Mat2D_Opaque/Semi0-3` + `Mat3D_Opaque/Semi0-3`) |
| **kNumSections** | 5 | 10 |
| **Source draw list** | `Gpu_->copy_ready_draw_list()` | `Gpu3D_->copy_ready_draw_list()` (shadow GPU) |
| **BindGpu3D()** | N'existe pas | Nouveau: connecte au shadow GPU |
| **MeshComp attach** | `Owner->GetRootComponent()` | `AttachToComponent(this, ...)` (SceneComponent) |
| **MatIdx routing** | `0-4 (opaque + semi)` | `BaseOfs = bHas3D ? 5 : 0` → 0-4=2D, 5-9=3D |
| **2D centering** | Hardcodé `160, 120` | `DL.display.width()/2, height()/2` |
| **2D depth** | Fixe `200.0f * WorldScale` | `Avg3DDepth` (moyenne TR[2] des triangles 3D) + ZStep |
| **Constructor** | rien de spécial | `SetMobility(EComponentMobility::Movable)` |

### Ce qui est IDENTIQUE (≈ le code de rendu 3D lui-même)

Le cœur du rendering — la boucle principale dans `RebuildMesh3D()` — est **quasi-identique**:

```cpp
// ═══ IDENTIQUE entre 001 et 002 ═══

// 3D vertex transform (RT * v + TR → camera space)
const int32_t vx = V3.vx, vy = V3.vy, vz = V3.vz;
const float cx = static_cast<float>((T.rt[0]*vx + T.rt[1]*vy + T.rt[2]*vz) / 4096 + T.tr[0]);
const float cy = static_cast<float>((T.rt[3]*vx + T.rt[4]*vy + T.rt[5]*vz) / 4096 + T.tr[1]);
const float cz = static_cast<float>((T.rt[6]*vx + T.rt[7]*vy + T.rt[8]*vz) / 4096 + T.tr[2]);

// Coordinate mapping (GTE → UE5)
Pos = FVector(
    cz * WorldScale,    // GTE Z (depth) → UE X (forward)
    cx * WorldScale,    // GTE X (right) → UE Y (right)
   -cy * WorldScale     // GTE Y (down)  → UE -Z (up)
);

// UV encoding (4 channels: texel coords, tpage, CLUT, tex depth + flags)
// → IDENTIQUE

// Winding flip CW→CCW
static const int32 WindingRemap[3] = {0, 2, 1};
// → IDENTIQUE

// Face normal computation
FVector FaceNorm = FVector::CrossProduct(B - A, C - A).GetSafeNormal();
// → IDENTIQUE

// Colors from DrawCmd vertex RGB
FLinearColor(V.r / 255.0f, V.g / 255.0f, V.b / 255.0f, 1.0f)
// → IDENTIQUE
```

**Conclusion**: Si la géométrie 3D affichée est identique entre les deux versions, c'est normal — le code de rendu n'a pas changé. Les différences visuelles (si il y en a) viennent des **données** produites par le shadow GPU vs le GPU primaire.

---

## 4. R3000GpuComponent — Séparation VRAM

### Avant (001)
- Hérite `UActorComponent`
- **Possède** la texture VRAM (1024×512 BGRA8): `CreateVramTexture()`, `UpdateVramTexture()`
- Debug viewer VRAM intégré (plane, material, scale)
- ~295 lignes de code VRAM upload + viewer
- Set corrélation GTE via GPU primaire: `Gpu_->set_gte_correlation(&gte_corr_table_)`

### Après (002)
- Hérite `USceneComponent`
- **Ne possède plus** la texture VRAM — reçue via `SetVramTexture(UTexture2D*)`
- Debug viewer retiré (déplacé dans `VramViewerComponent`)
- Code VRAM supprimé (-295 lignes)
- Correlation retirée (shadow systems la remplacent)

### Fichier: R3000GpuComponent.h — Supprimé
- `GetVramWidth()`, `GetVramHeight()`, `DecodeSemiMode()` helpers
- `bShowVramViewer`, `VramViewerMaterial`, `VramViewerScale` props
- `PixelBuffer_`, `VramCopyBuffer_`, `LastVramWriteSeq_`, `VramViewerMesh_` membres

### Fichier: R3000GpuComponent.h — Ajouté
- `SetVramTexture(UTexture2D*)` pour recevoir la texture partagée
- `EHdDefinition` enum pour HD scaling (720p/1080p/1440p/4K/Custom)
- `bUniformHdScale`, `HdDefinition`, `TargetWidth`, `TargetHeight` props

---

## 5. R3000VramViewerComponent (NOUVEAU)

- **`R3000VramViewerComponent.h`** (~116 lignes, NOUVEAU)
- **`R3000VramViewerComponent.cpp`** (~260 lignes, NOUVEAU)
- Possède la texture VRAM: `CreateVramTexture()`, upload chaque frame
- Partage via `SetVramTexture()` avec `GpuComponent` et `Gpu3DComponent`
- Debug viewer optionnel (plane VRAM): `bShowViewer`, `ViewerMaterial`, `ViewerScale`

---

## 6. R3000EmuComponent — Orchestration / Binding

### Avant (001)
```
EmuComponent → FindComponent<Gpu3DComponent>
  → Gpu3DComp->BindGpu(Bus->gpu())         // GPU primaire du bus
  → Gpu3DComp->SetVramTexture(GpuComp->GetVramTexture())  // VRAM du GpuComp
```
- Pas de VramViewer
- Pas de shadow GPU
- Gpu3D lit les draw lists du GPU primaire

### Après (002)
```
EmuComponent → FindComponent<VramViewerComponent>
  → GpuComp->SetVramTexture(VramComp->GetVramTexture())    // VRAM shared
  → Gpu3DComp->SetVramTexture(VramComp->GetVramTexture())  // VRAM shared

EmuComponent → FindComponent<Gpu3DComponent>
  → Gpu3DComp->BindGpu(Core->gpu())            // GPU primaire (pour fallback/frame count)
  → Gpu3DComp->BindGpu3D(Core->gpu_3d())       // Shadow GPU (source de données 3D)
```
- VramViewer distribue la texture VRAM
- Shadow GPU fournit les données 3D taguées

---

## 7. Symlink Git (FIX)

### Avant (001)
- Junction Windows absolue: `Private/src → E:\Projects\github\Live\R3000-Emu\src`
- `core.symlinks=false`, non trackée par git, invisible sur GitHub

### Après (002)
- Symlink relatif: `Private/src → ../../../../../../src`
- `core.symlinks=true`, mode `120000`, lien navigable sur GitHub
- Au clone Windows + Developer Mode: vrai symlink créé automatiquement

---

## 8. docs/GPU3D_COMPONENT.md

Ce document existe **dans les deux versions** mais décrit l'architecture **001** (GteCorrelationTable):
- Pipeline: GTE Snapshot → Correlation Table → GPU Lookup → Draw List → UE5
- File map référence `gte_correlation.h/.cpp` et `gpu.cpp` (push_triangle avec lookup)
- Les concepts (SXY FIFO, correlation key, draw offset) sont corrects mais pour l'ancien système

**Ce doc n'a PAS été mis à jour** pour le nouveau shadow GTE/GPU. Il est maintenant **obsolète**.

---

## 9. Résumé: ce qui pourrait expliquer des différences visuelles

| Facteur | Impact potentiel |
|---------|------------------|
| **Source données**: `Gpu3D` au lieu de `Gpu` | Les DrawCmd3D viennent d'un parser GP0 **différent** (shadow vs primaire). Si le shadow GPU parse différemment → données 3D différentes |
| **Tagging différentiel**: face_idx via encodage lattice | Remplace le lookup SXY. Si le décodage échoue → `origin_2d_hud` au lieu de `origin_3d` |
| **DrawCmd3D enrichi**: normals, face_idx, is_quad | Disponibles mais **non exploités** côté UE5 (pas de changement visuel attendu) |
| **10 matériaux**: 2D/3D séparés | Si un matériau n'est pas assigné dans Blueprint → section invisible |
| **2D centering**: display.width au lieu de 160 hardcodé | Change le centrage des éléments 2D si la résolution diffère |
| **SceneComponent**: MeshComp attaché à `this` | Le transform parent peut être différent si le composant n'est pas à l'origin |
| **VramViewer**: texture partagée | Devrait être identique au VRAM upload précédent, pas d'impact |

---

## 10. Résumé des changements par zone

```
Emulator core (src/)       : +3327 / -11  (shadow GTE/GPU, face cache, quad detection)
UE5 plugin (integrations/) : -466 / +549  (VramViewer, 10 mats, SceneComponent, symlink)
Build (CMakeLists.txt)     : +2           (new source files in CORE_SOURCES)
```

## 11. Problèmes connus (cette branche)

1. **Couleurs mosaïque** sur le logo PS en 3D — visible de côté, les faces arrière sont cullées (material single-sided)
2. **Éléments 2D géants** quand `bSkip2DElements=false` — les quads plein écran 2D créent des plans énormes en espace 3D
3. **Coords 2D du shadow GPU** n'ont PAS l'offset de dessin soustrait (le GPU primaire le soustrait, pas le shadow)
4. **Profondeur 2D**: utilise `Avg3DDepth` qui peut être 0 si aucun triangle 3D dans le frame
5. **GPU3D_COMPONENT.md** est obsolète (décrit l'ancien système de correlation)
