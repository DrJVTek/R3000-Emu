# Handoff Claude - Branch `cortex-test`

Date: 2026-03-05

## Objectif en cours

Prototype "dataflow token" (sans fallback) pour reconstruire le lien GTE->GPU3D sans
injecter de coordonnees taggees dans le pipeline principal.

Flux cible:
- COP2 (MFC2/SWC2) -> token face
- token propage vers RAM
- DMA2 lit RAM + token
- Gpu3D utilise le token pour `face_idx`

## Etat actuel (progressif)

### Branche
- Branche locale creee: `cortex-test`

### Modifs deja faites

1. `src/r3000/bus.h`
- Ajout `#include <vector>`
- Ajout constante `kNoFaceToken`
- Ajout API:
  - `set_ram_face_token(uint32_t paddr, uint32_t token)`
  - `ram_face_token(uint32_t paddr) const`
- Ajout stockage:
  - `std::vector<uint32_t> ram_face_tokens_`

2. `src/gpu/gpu_3d.h`
- Ajout constante `kNoFaceHint`
- Ajout API:
  - `gp0_with_face_hint(uint32_t word, uint32_t face_hint)`
- Ajout buffer parallele:
  - `cmd_face_hint_[16]`

3. `src/r3000/cpu.h`
- `PendingLoad` etendu avec `face_token`
- Ajout constante CPU `kNoFaceToken`
- Ajout tableau registre->token:
  - `gpr_face_token_[32]`

4. `src/r3000/bus.cpp`
- Version marker passe a `v26 (face_token_flow)`
- Initialisation `ram_face_tokens_` dans le constructeur
- Implementations ajoutees:
  - `set_ram_face_token(...)`
  - `ram_face_token(...)`
- Clear token sur ecritures RAM (`write_u8/u16/u32`)
- DMA2 GPU:
  - propagation token vers shadow GPU via `gp0_with_face_hint(...)`
  - block mode + linked-list mode couverts
- MMIO direct GPU (`1F801810`) route vers `gp0_with_face_hint(..., kNoFaceToken)`

5. `src/gpu/gpu_3d.cpp`
- `gp0()` devient wrapper de `gp0_with_face_hint(..., kNoFaceHint)`
- buffer parallele `cmd_face_hint_` rempli pendant la collecte GP0
- `gp0_start_command(...)` etendu avec `face_hint`
- `gp0_polygon()`:
  - collecte `face_hints[]` sur les mots XY des vertices
  - derive `face_idx` par majority vote des hints
  - prototype strict: pas de fallback decode differential dans ce chemin

6. `src/r3000/cpu.cpp`
- version marker `v8 (face_token_flow)`
- reset:
  - init `gpr_face_token_[]`
  - init `pending_load_.face_token`
- `set_reg()` clear token destination
- `commit_pending_load()` propage `face_token` vers registre
- `next_pending_load.face_token` initialise dans `step()`
- helpers memoire:
  - `store_u8/u16`: clear token RAM
  - `store_u32`: ecrit token RAM associe
- ajout helper decode token depuis SXY tagge (`decode_face_token_from_tagged_sxy`)
- LW:
  - recupere token RAM et l'attache au pending load
- SW:
  - ecrit token du registre source en RAM
- COP2:
  - MFC2 lit toujours le GTE primaire
  - token derive depuis shadow pour regs SXY (12..15)
  - SWC2 ecrit toujours valeur primaire, token derive shadow

### Build
- Build local OK:
  - `cmake --build build -j 4`
  - sortie: `lib/Debug/r3000_emu.exe`

### Correctif UE5 flicker double-buffer (Y instable)

7. `src/gpu/gpu_3d.cpp`
- Correctif applique sur la conversion des vertices 2D (`make_vertex`):
  - avant: `se11(raw) + draw_env.offset`
  - maintenant: `se11(raw)` uniquement (offset ignore pour le shadow 3D path)
- Raison:
  - `draw_env.offset_y` alterne selon la banque VRAM (double-buffer) et provoque
    un decalage Y frame-a-frame dans la scene UE5 (flicker).
  - Ce composant 3D attend des coords ecran stables, pas des coords VRAM ciblees.
- Build revalide apres patch:
  - `cmake --build build -j 4` OK

8. `src/gpu/gpu_3d.cpp` (trous / polys manquants)
- Selection `face_idx` amelioree dans `gp0_polygon()`:
  - avant: vote majoritaire brut des hints
  - maintenant: vote majoritaire **avec priorite aux tokens presents en face cache**
  - si aucun token hint n'est en cache: on garde le meilleur token hint brut
- Objectif:
  - reduire les primitives qui retombent en 2D a cause d'un token majoritaire stale/non present.
- Contrainte respectee:
  - toujours sans fallback decode differential dans ce chemin.

9. Reduction du bruit logs (warnings)
- `integrations/ue5/.../R3000Gpu3DComponent.cpp`:
  - logs lifecycle/rebuild passes de `Warning` a `Log/Verbose`
  - logs debug detailes `GPU3D` passes de `warn` a `info`
- `src/gpu/gpu_3d.cpp`:
  - logs `GPU3D_DIAG` / `GPU3D_VBLANK` / init passes de `warn` a `info`
- But:
  - garder `Warning` UE5 uniquement pour vrais problemes (ex: materiaux manquants)
  - rendre les vrais warnings lisibles pour diagnostiquer les trous.

10. Fix format texture 2D menu (bit RAW sur polygons)
- Cause probable identifiee:
  - pour GP0 polygons texturés (`20h-3Fh`), le bit `raw texture` (cmd bit0)
    n'etait pas reporte dans `flags`.
  - contrairement a `gp0_rect()`, `gp0_polygon()` n'ajoutait pas `flags |= 4`.
- Impact:
  - le shader peut moduler a tort par la couleur vertex alors que le primitive est raw
    (no modulation), ce qui fausse fortement le rendu (teinte/format percu incorrect).
- Correctif applique:
  - `src/gpu/gpu.cpp` -> `gp0_polygon()` lit `raw` et set `flags |= 4`
  - `src/gpu/gpu_3d.cpp` -> meme fix pour coherence du shadow path
- Build:
  - `cmake --build build -j 4` OK

11. Fix 2D rect path dans `Gpu3D` (cause probable fond menu faux dans composant 3D)
- Contexte:
  - Le user a precise que le probleme est en 2D dans `R3000Gpu3DComponent`.
  - Comparaison `Gpu::gp0_rect()` vs `Gpu3D::gp0_rect()` trouvait des divergences.
- Divergences corrigees dans `src/gpu/gpu_3d.cpp`:
  - ajout support bit `raw texture` (cmd bit0) -> `flags |= 4`
  - UV rect: passage de wrap implicite (`uint8(u0+w)`) a clamp `[0..255]` comme `Gpu`
  - guard sur tailles invalides `w<=0 || h<=0`
- Effet attendu:
  - decode texture/CLUT rects 2D aligne sur le pipeline 2D normal, reduisant les erreurs
    type "16 couleurs lues comme 256" dans le composant 3D.
- Build:
  - `cmake --build build -j 4` OK

11b. Ajustement UV rect `Gpu3D` (drapeau coupe en bas)
- Observation user:
  - apres le fix rect, le drapeau apparaissait "a moitie" (bas manquant).
- Cause probable:
  - clamp UV `[0..255]` sur `gp0_rect()` shadow path coupait certains sprites qui
    comptent sur wrap 8-bit.
- Correctif:
  - rollback vers UV wrap 8-bit naturel (`uint8_t` cast) pour `u1/v1`.
- Fichier:
  - `src/gpu/gpu_3d.cpp`

17. Fix demi-quads manquants (drapeau): token V3 pour `face_B`
- Symptome:
  - "moitie de polygons" manquante sur le drapeau (second triangle de quad).
- Cause probable:
  - `push_quad()` utilisait encore surtout un decode legacy via coords taggees pour trouver `face_B`.
  - en mode token-flow, ce decode n'est souvent plus valide -> fallback degeneré du 2e tri.
- Correctif:
  - ajout d'un hint explicite `face_idx_v3_hint` dans `push_quad(...)`
  - `gp0_polygon()` passe le hint du mot XY de `V3` (`face_hints[3]`)
  - `push_quad()` priorise ce hint pour resoudre `face_B` (edge-strip + fallback non-edge)
- Fichiers:
  - `src/gpu/gpu_3d.h`
  - `src/gpu/gpu_3d.cpp`

12. Mode camera/mesh dans `R3000Gpu3DComponent`
- Ajout `TrackingMode` (enum):
  - `LegacyFollowOwner` (defaut, comportement actuel)
  - `WorldLocked` (detach mesh en world-space pour free-roam camera)
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

13. Fix trous: propagation token sur acces memoire partiels CPU
- Cause probable:
  - `LWL/LWR/SWL/SWR` ne propageaient pas correctement `face_token`
  - `SB/SH` n'ecrivaient pas le token du registre source
  - `LB/LBU/LH/LHU` ne recuperaient pas de token RAM
- Correctifs (`src/r3000/cpu.cpp`):
  - `store_u8/store_u16` acceptent maintenant un `face_token`
  - `SB/SH` ecrivent `gpr_face_token_[rt]`
  - `SWL/SWR` ecrivent via `store_u32(..., gpr_face_token_[rt])`
  - `LWL/LWR` set `next_pending_load.face_token` depuis `ram_face_token(base)`
  - `LB/LBU/LH/LHU` set `next_pending_load.face_token` depuis `ram_face_token(addr)`
- Build:
  - `cmake --build build -j 4` OK

14. Mode "warnings only" pour `R3000Gpu3DComponent`
- Ajout compile-time define local dans:
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`
- Define:
  - `R3000_GPU3D_WARNINGS_ONLY` (defaut = 1)
- Effet:
  - logs `UE_LOG` niveau `Log/Verbose` de ce composant compiles out
  - logs `emu::logf` niveau info (`GPU3D`) compiles out
  - les `Warning/Error` restent visibles
- Pour reactiver les logs bruyants:
  - passer `R3000_GPU3D_WARNINGS_ONLY` a `0`.

15. Switch transform 3D optionnel dans `R3000Gpu3DComponent`
- Ajout property Blueprint/C++:
  - `bApplyGteTransform` (defaut `true`)
- Comportement:
  - `true`: mode actuel, applique `RT*V + TR` avant mapping UE
  - `false`: utilise directement `verts_3d` bruts (mode exploration/world debug)
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

16. Mode experimental "pseudo world-space" (Gpu3D)
- Ajout property:
  - `bApproxWorldFromFrameRef` (defaut `false`)
- Principe:
  - on reconstruit d'abord en camera-space (`RT*V+TR`)
  - puis conversion approx en "world" via inversion d'une transform de reference de frame
    (`p_w = R_ref^T * (p_c - T_ref)`)
- Remarque:
  - c'est experimental (pas une decomposition camera/objet parfaite)
  - utile pour test free-roam plus naturel quand `TrackingMode=WorldLocked`
- Fichiers:
  - `integrations/ue5/.../Public/R3000Gpu3DComponent.h`
  - `integrations/ue5/.../Private/R3000Gpu3DComponent.cpp`

## IMPORTANT - travail interrompu en cours

Prototype compile, mais validation fonctionnelle non faite (CLI/UE5).
Il reste des changements potentiels a faire dans:

- `src/r3000/bus.cpp`
  - eventuellement instrumenter stats token-hit/miss par frame pour debug

- `src/gpu/gpu_3d.cpp`
  - verifier le comportement des quads Ridge (majority vote peut etre insuffisant)
  - ajouter logs de qualite token (hints valides par primitive)

- `src/r3000/cpu.cpp`
  - etendre eventuellement propagation token a d'autres ops de move/merge (SWL/SWR, etc.)
  - verifier que clear token agressif ne casse pas des chemins utiles

## Etat git notable avant cette tache

Le repo etait deja "dirty" avec:
- `.gitignore` modifie
- suppressions staged d'artefacts UE5 binaries
- docs ajoutees

Ne pas reset hard.

## Intention technique (rappel)

Ce prototype vise un chemin deterministic "token flow" pour Ridge Racer:
- pas de dependance principale au decode des coordonnees taggees
- pas de fallback heuristique pour ce test

Si le hit rate token est insuffisant, la prochaine iteration devra etendre la propagation
token sur davantage d'instructions de copie/transfo CPU.
