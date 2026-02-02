# Research: UE5 Integration (R3000-Emu)

**Date**: 2026-01-25  
**Feature**: `specs/001-ue5-integration/spec.md`  

## Goals Recap

- Intégrer le coeur R3000-Emu dans un projet Unreal Engine 5 (Windows) sans hacks/placeholder.
- Minimiser la friction de build (toolchains UE/UBT) et isoler l’intégration UE du core.

## Key Findings (UE docs)

- Epic recommande que, pour un plugin, il soit “plus pratique” d’inclure le third-party **dans le dossier du plugin** plutôt que d’utiliser `Engine/Source/ThirdParty` (plugin-local vendor).  
  Source: Epic doc “Integrating Third-Party Libraries”, section intro + “Module Setup”.  
- Un module third-party “sans source” se déclare via un `.build.cs` avec `Type = ModuleType.External;`, et on renseigne `PublicIncludePaths`, `PublicAdditionalLibraries`, etc.  
  Source: Epic doc “Integrating Third-Party Libraries”, section “Module Setup”.  
- UBT expose des propriétés de module utiles si on doit aligner les flags (ex: `bEnableExceptions`, `bUseRTTI`).  
  Source: Epic doc “Module Properties in Unreal Engine”.
- UE fournit des macros pour isoler les includes third-party des warnings UE (`THIRD_PARTY_INCLUDES_START/END`) et des soucis d’alignement/packing (`PRAGMA_PUSH_PLATFORM_DEFAULT_PACKING`, etc.).  
  Source: Epic doc “Integrating Third-Party Libraries”, section “Troubleshooting”.

## Decisions

### Decision D1: Livrer l’intégration sous forme de plugin/module UE5 dans `integrations/ue5/`

**Decision**: Créer un dossier `integrations/ue5/` dans ce repo qui contient une intégration UE5 (plugin ou module de projet) et référence le core R3000-Emu.

**Rationale**:
- Respecte l’hygiène du repo (pas de fichiers UE à la racine).
- Colle à la recommandation Epic: third-party placé dans le plugin/projet (plutôt que dans l’Engine).  

**Alternatives considered**:
- Mettre l’intégration sous `Engine/Source/ThirdParty`: rejeté (couplage fort, moins portable pour un plugin).

### Decision D2: Compiler le core depuis les sources dans UE (par défaut), garder l’option “External prebuilt” ouverte

**Decision**: Par défaut, faire compiler le core R3000-Emu **depuis ses sources** via UBT (module UE runtime) pour éviter les problèmes de compatibilité toolchain/CRT.

**Rationale**:
- UE/UBT contrôle flags, warnings-as-errors, packing, etc.
- Le core n’utilise pas RTTI ni exceptions (vérifié: pas de `dynamic_cast`, pas de `throw`, pas de `typeid`), donc on évite d’avoir à activer `bUseRTTI` / `bEnableExceptions` côté module.

**Alternatives considered**:
- Prebuilt static lib compilée via CMake + `ModuleType.External`: possible (pattern Epic), mais augmente la friction (binaries par config/UE version) et les risques de mismatch CRT.

### Decision D3: Encadrer les includes third-party pour réduire les warnings et risques d’ABI

**Decision**: L’intégration UE utilisera les macros UE quand nécessaire:
- `THIRD_PARTY_INCLUDES_START/END` autour des headers non-UE
- `PRAGMA_PUSH_PLATFORM_DEFAULT_PACKING` / `PRAGMA_POP_PLATFORM_DEFAULT_PACKING` si des structs publics exposent des types 8 bytes

**Rationale**:
- UE traite beaucoup de warnings en erreurs; ces macros sont prévues pour ça.

## Test Assets

- **BIOS**: `bios/ps1_bios.bin` (SCPH-7502 v4.1 EU, 1997-12-16)
- **Test Game**: `E:\Projects\PSX\roms\Ridge Racer (U).cue` (14 tracks, CD audio)

## Boot Status (2026-01-27)

Le BIOS boot correctement:
1. Démarre à 0xBFC00000 (ROM BIOS)
2. Copie kernel vers RAM (0x80062xxx)
3. Initialise hardware: GPU (12k+ commandes), SPU, DMA, Timers
4. VBlank IRQ fonctionne (~338 Hz avec période 100k cycles)
5. CDROM Test(0x20) envoyé et réponse lue (date BIOS: 94/09/19 v.C)

**Issue**: Le BIOS n'envoie pas GetID après Test. Probablement en attente:
- Input controller/pad (SIO non émulé)
- Ou timeout Sony logo animation

Prochaines étapes pour boot complet:
- Émulation SIO minimale (controller timeout)
- Ou fast-boot / skip logo

## Implementation Notes (to feed Phase 1)

- Le core devra être découpé en "lib core" (API stable) + "CLI". Dans le repo, côté CMake, on pourra créer une cible library (ex: `r3000_core`) et faire linker `r3000_emu` dessus; côté UE, on compile/emballe la même base de sources derrière une API UE-friendly.
- Si on doit un jour utiliser une DLL (peu probable pour le core), Epic documente `RuntimeDependencies.Add(...)` + `PublicDelayLoadDLLs.Add(...)` pour staging/chargement.

