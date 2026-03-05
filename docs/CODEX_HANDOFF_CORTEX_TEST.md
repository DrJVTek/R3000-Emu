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
