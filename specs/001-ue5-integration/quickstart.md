# Quickstart: UE5 Integration (R3000-Emu)

**Date**: 2026-01-25  
**Feature**: `specs/001-ue5-integration/spec.md`

## What this will enable

- Compiler le core R3000-Emu dans un projet UE5.
- Instancier une instance d’émulation et exécuter des steps.
- Visualiser des logs (Output Log UE et/ou fichier) sans dépendre d’un terminal.

## Expected repo layout (after implementation)

```text
integrations/ue5/
└── R3000Emu/  # plugin UE5 (R3000Emu.uplugin)
```

## Minimal test plan (after implementation)

### CLI smoke test

1. Compiler la CLI (`cmake -S . -B build` puis `cmake --build build -j`).
2. Lancer un scénario existant (ex: `--load=...` ou `--debug-bios` selon ton setup).
3. Vérifier: pas de crash + logs attendus.
4. Note: pas d’exigence de **rendu GPU** ni de **sortie audio SPU** côté CLI (harness uniquement).

### UE5 integration smoke test

1. Ouvrir un projet UE5 (Windows).
2. Ajouter le plugin `integrations/ue5/R3000Emu/` au projet UE (ex: via submodule ou copie dans `Plugins/`).
3. Compiler l’éditeur.
4. Lancer une scène qui:
   - crée une instance
   - charge un BIOS (chemin fourni par config UE)
   - exécute 1 000 000 steps
   - détruit l’instance
5. Vérifier:
   - pas de crash
   - logs visibles (UE Output Log ou fichier configuré)

