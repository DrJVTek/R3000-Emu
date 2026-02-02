# Implementation Plan: R3000-Emu (PS1 emulator CLI)

**Branch**: `[main]` | **Date**: 2026-01-24 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/main/spec.md`

## Summary

Le projet fournit un exécutable CLI `r3000_emu` qui charge un binaire externe (ELF32 MIPS little-endian ou PS-X EXE),
initialise l’état CPU (PC/GP/SP) et exécute un interpréteur R3000 (MIPS I). Les logs/trace sont configurables via la
ligne de commande pour la pédagogie live. Le GTE (COP2) est implémenté dans un module dédié, appelé depuis le CPU via
les opcodes COP2 et transferts associés.  
En plus, un mode “boot BIOS” charge une ROM BIOS PS1, mappe `0x1FC00000` (alias `0xBFC00000`) et fournit des stubs I/O
minimaux pour laisser le BIOS exécuter ses premières étapes.
Une première brique CDROM est ajoutée: lecture d’images (2048 ou 2352 bytes/secteur, 2352 requis pour CD-XA / Mode2),
et stub MMIO minimal du contrôleur CD (à compléter).

## Technical Context

**Language/Version**: C++ (style “C+”: stdio/printf-like, pas d’iostream)  
**Primary Dependencies**: N/A (C/C++ standard library)  
**Storage**: N/A  
**Testing**: tests manuels via programmes guest (ex: `examples/hello`)  
**Target Platform**: Windows (MSVC) + toolchain MIPS `mipsel-none-elf-gcc` pour compiler le guest  
**Project Type**: single project (CMake)  
**Performance Goals**: suffisante pour démo live (pas de JIT, pas de cycle-accuracy)  
**Constraints**: lisibilité (Allman), commentaires didactiques, logs configurables, séparation CPU/GTE  
**Scale/Scope**: CPU R3000 + GTE + loaders (ELF/PSX EXE), pas de GPU/SPU/CDROM pour l’instant
  - Ajout: BIOS boot (ROM + scratchpad + I/O stubs).
  - Début CDROM: lecteur d’image + MMIO stub, ISO/XA à venir.

## Constitution Check

Le fichier `.specify/memory/constitution.md` est la **constitution réelle** du projet (non négociable).
Implications directes pour ce plan :

- Pas de simulation/placeholder (“stub”), pas de TODO-only, pas de “demo mode”.
- Pas de hacks BIOS (patch mémoire / retours forcés / skip de boucles).
- Les diagnostics (`--debug-bios`, trace, logs) ne doivent **jamais** changer le comportement d’émulation.
- Les MMIO/IRQ doivent suivre les docs PS1 (PSX-SPX/no$psx) dès que le BIOS en dépend.

## Project Structure

### Documentation (this feature)

```text
specs/main/
├── spec.md
├── plan.md
└── checklists/
    └── emulator-docs.md
```

### Source Code (repository root)

```text
src/
├── cdrom/     # CDROM: lecteur d’image + MMIO minimal
├── r3000/      # CPU core + bus mémoire
├── gte/        # COP2 / GTE (module séparé)
├── loader/     # ELF32 MIPS LE + PS-X EXE minimal
├── log/        # logger “C+”
└── main.cpp    # CLI (parsing args + run loop)

examples/
└── hello/      # programme guest minimal (ELF) + build.ps1
```

**Structure Decision**: Le code reste en `src/` (projet unique) pour minimiser la friction et garder le repo lisible en live.

