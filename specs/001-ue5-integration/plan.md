# Implementation Plan: UE5 Integration

**Branch**: `[001-ue5-integration]` | **Date**: 2026-01-25 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `/specs/001-ue5-integration/spec.md`

## Summary

Rendre le coeur R3000-Emu **intégrable dans un projet Unreal Engine 5** sans “hacks” (constitution), avec :
- une séparation claire **core emu** vs **CLI**
- une API de contrôle (create/destroy, load, step, inspect minimal)
- une observabilité compatible UE (logs/trace/dumps redirigeables)
- et une **non-régression CLI** (la CLI reste le harness privilégié pour certains tests).

## Technical Context

**Language/Version**: C++17 (repo actuel: `CMAKE_CXX_STANDARD 17`)  
**Primary Dependencies**:
- Core: N/A (std + code maison)
- Integration: Unreal Engine 5.x (côté projet UE)
**Storage**: fichiers (BIOS/CD/dumps) fournis par l’intégration UE (pas de chemin hardcodé non overridable)  
**Testing**:
- Manuel: build/boot CLI existant
- Build-check: compilation/link de l’intégration UE5 (éditeur)
**Target Platform**: Windows (MSVC toolchain UE, Win64)  
**Project Type**: monorepo C++ (CMake pour CLI) + dossier d’intégration UE (plugin/module)  
**Performance Goals**:
- Suffisant pour exécuter \( \ge 10^6 \) steps sans crash dans une scène UE (cf. spec SC-002)
**Constraints**:
- Constitution: pas de stubs/placeholder, pas de hacks BIOS, diagnostics non-intrusifs, pas de hardcode non-overridable
- Compat: conserver la CLI comme harness de test (build + run) en parallèle d’UE5
  - La CLI n’a pas vocation à fournir le rendu GPU ou la sortie audio SPU; ces aspects sont attendus côté UE5.
- Repo hygiene: éviter d’ajouter des fichiers root inutiles; regrouper l’intégration UE dans un sous-dossier dédié
**Scale/Scope**:
- Objectif: intégration UE5 + API + observabilité
- Pas d’objectif “full emulator accurate” dans cette feature

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

- **Existing code/docs first**: l’intégration doit réutiliser le core existant (pas de duplication inutile).
- **No simulation / placeholder code**: pas de “fake hardware” ajouté pour satisfaire l’intégration UE.
- **No hardcoded, non-overridable config**: chemins et options MUST être injectables (config UE/paths).
- **Diagnostics do not change emulation**: les sinks logs/trace ne doivent pas modifier l’exécution.
- **MMIO/IRQ correctness**: aucune “optimisation UE” ne doit contourner la logique core (pas de hacks BIOS).
- **Repo hygiene**: pas de nouveaux fichiers à la racine hors nécessité; intégrer sous `integrations/ue5/`.

## Project Structure

### Documentation (this feature)

```text
specs/001-ue5-integration/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/
└── tasks.md
```

### Source Code (repository root)

```text
src/
├── cdrom/
├── gpu/
├── gte/
├── loader/
├── log/
├── r3000/
└── main.cpp              # CLI (sera refactoré pour utiliser une lib core)

integrations/
└── ue5/                  # plugin/module UE5 + glue (à ajouter dans cette feature)
```

**Structure Decision**: garder le core en `src/` (minimiser les moves), créer une cible “core” réutilisable par la CLI et par l’intégration UE5, et isoler UE5 sous `integrations/ue5/` pour respecter l’hygiène du repo.

## Complexity Tracking

**No violations identified** at planning stage.
