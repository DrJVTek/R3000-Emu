# Analyse Projet R3000-Emu + Lien PSXVR

Date: 2026-03-05

## Contexte

Cette analyse couvre:
- `E:\Projects\github\Live\R3000-Emu`
- Le lien runtime avec `E:\Projects\github\Live\PSXVR`

Objectif: documenter l'etat technique reel, les risques prioritaires, et les actions recommandees.

## Resume Executif

- Le couplage `R3000-Emu -> PSXVR` est direct via symlink de plugin.
- L'architecture core/UE5 est globalement bonne (separation propre, worker thread robuste).
- Les principaux risques sont dans l'accumulation de workarounds IRQ/events/timing qui peuvent masquer les vraies divergences non-HLE.
- Le repo contient beaucoup d'artefacts build UE5 versionnes/non ignores, ce qui complique les revues et la stabilite des branches.

## Couplage avec PSXVR

- `PSXVR/Plugins/R3000Emu` est un symlink vers:
  - `R3000-Emu/integrations/ue5/R3000Emu`
- Toute modification du plugin dans `R3000-Emu` impacte immediatement la version Unreal de `PSXVR`.

Reference:
- `E:\Projects\github\Live\PSXVR\Plugins\R3000Emu` (symlink)

## Architecture Observee

### Core emulateur

- Core central isole des dependances Unreal:
  - `src/emu/core.h`
  - `src/emu/core.cpp`
- Sous-systemes relies:
  - CPU/Bus: `src/r3000/*`
  - GPU/GTE/CDROM/SPU: `src/gpu/*`, `src/gte/*`, `src/cdrom/*`, `src/audio/*`

### Plugin UE5

- Compilation directe des sources emulateur via symlink `Private/src`:
  - `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/R3000EmuRuntime.Build.cs`
- Worker thread avec waitable timer Windows et boucle en dette de cycles:
  - `.../Private/R3000EmuComponent.cpp`

## Constat Detaille

### 1) Risque critique: workarounds IRQ/events dans le Bus

Des mecanismes de secours sont actifs dans le path runtime:

- Rescue events `BUSY -> READY`:
  - `src/r3000/bus.cpp` (bloc "RESCUE")
- Auto-activation de `I_MASK` si nul apres delai/VBlank:
  - `src/r3000/bus.cpp` (auto-enable a `0x0075`)

Impact:
- Peut debloquer artificiellement certains boots.
- Peut masquer une divergence d'emulation non-HLE (kernel BIOS/IRQ chain) au lieu de la corriger.
- Rend les comparaisons strictes (DuckStation/reference) plus ambigues.

### 2) Risque critique: incoherence fast-boot vs non-HLE

Le core force `set_hle_vectors(1)` en fast boot:
- `src/emu/core.cpp` (`fast_boot_from_cd`, `fast_boot_from_exe`)

Impact:
- En fast boot, le comportement final est HLE meme si la config utilisateur vise non-HLE.
- Incoherence potentielle avec les options exposees dans le composant UE.

### 3) Risque important: etat statique global dans `Core::step()`

Variables statiques de fichier:
- `g_boot_start`
- `g_boot_start_set`
- `g_step_count`

Fichier:
- `src/emu/core.cpp`

Impact:
- Etat partage entre instances/core reset.
- Milestones et telemetrie potentiellement pollues entre runs.

### 4) Risque important: hygiene repo UE5

Etat observe:
- `integrations/ue5/R3000Emu/Binaries`: ~2.13 GB
- `integrations/ue5/R3000Emu/Intermediate`: ~29.9 MB
- Plusieurs fichiers binaires/modifies apparaissent dans `git status`.

Impact:
- Diffs bruyants, review difficile.
- Risque de commits non intentionnels de binaires.
- Ralentit CI, clonage, et maintenance.

### 5) Risque moyen: drift documentation UE

`integrations/ue5/README.md` mentionne un chemin de build ancien (`R3000CoreFromRepo.cpp`), alors que le build actuel s'appuie sur le symlink `Private/src`.

Impact:
- Onboarding plus lent.
- Debug/build plus fragile quand un nouveau contributeur suit la doc.

## Points Positifs

- Bonne separation core/UE5.
- Worker thread UE5 bien structure:
  - creation timer high-res + fallback
  - arret propre (`StopWorkerThread`, `WaitForCompletion`, cleanup)
- Capacites de logs riches pour diagnostic (system/gpu/cdrom/io).
- Historique de debug documente (notamment `docs/DEBUG_UE5_STUCK.md`).

## Recommandations (ordre de priorite)

### P1 - Stabilisation technique (sans regression visible)

1. Encadrer les workarounds bus par flags de debug explicites (off par defaut en mode "accuracy").
2. Clarifier la politique HLE/non-HLE:
   - soit respecter strictement le mode choisi
   - soit signaler explicitement quand un mode force HLE (logs + doc + UI).
3. Rendre les milestones du core instance-locales (supprimer les statiques globaux).

### P2 - Hygiene repo + process

1. Ignorer explicitement les artefacts UE du plugin:
   - `integrations/ue5/R3000Emu/Binaries/`
   - `integrations/ue5/R3000Emu/Intermediate/`
2. Garder uniquement le code source plugin + metadata necessaires.
3. Ajouter une checklist pre-commit pour eviter les binaires UE.

### P3 - Documentation

1. Mettre a jour `integrations/ue5/README.md` pour refléter le flux reel (symlink `Private/src`).
2. Documenter clairement les matrices de test:
   - BIOS boot non-HLE
   - BIOS boot HLE
   - fast boot
   - dev kit

## Plan de Stabilisation Propose (patches courts)

1. Patch A - Hygiene repo (`.gitignore` + note de nettoyage artefacts UE)
2. Patch B - Milestones core: suppression des statiques globaux
3. Patch C - Gate des workarounds bus via options runtime
4. Patch D - Alignement HLE/non-HLE fast boot + logs explicites
5. Patch E - MEP doc UE integration + procedure de test CLI/UE

## Fichiers Clefs Inspectes

- `README.md`
- `CMakeLists.txt`
- `CLAUDE.md`
- `src/emu/core.h`
- `src/emu/core.cpp`
- `src/r3000/bus.h`
- `src/r3000/bus.cpp`
- `integrations/ue5/README.md`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/R3000EmuRuntime.Build.cs`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000EmuComponent.h`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000EmuComponent.cpp`

## Notes

- Analyse realisee sans modifier le comportement runtime.
- Aucun changement de code fonctionnel applique dans cette etape.
