# Contracts: Emulation Control API (UE5 Integration)

**Date**: 2026-01-25  
**Feature**: `specs/001-ue5-integration/spec.md`

## Intent

Définir un contrat minimal et stable pour piloter l’émulation depuis UE5 (create/destroy, load, step, inspect, observability).

## Concepts

- **Handle**: identifiant opaque représentant une `EmulationInstance`.
- **Result**: succès/erreur explicite (pas de fallback silencieux).
- **Diagnostics**: options d’observabilité; ne doivent pas modifier l’exécution.

## API Surface (contract-level)

### Lifecycle

- **CreateInstance(config, sinks) → handle or error**
  - MUST créer une instance isolée.
  - MUST échouer proprement si allocation impossible.

- **DestroyInstance(handle) → ok or error**
  - MUST être safe si appelé plusieurs fois sur le même handle (idempotent ou erreur explicite, mais pas de crash).

### Loading

- **LoadBios(handle, path) → ok or error**
  - MUST échouer proprement si fichier inaccessible/format invalide.

- **InsertDisc(handle, path) → ok or error**
  - MUST échouer proprement si image invalide (pas de fallback implicite).

- **Reset(handle) → ok or error**
  - MUST remettre l’état à un reset cohérent (comme le CLI).

### Execution

- **Step(handle, stepsCount) → {executed, reason} or error**
  - MUST exécuter exactement `stepsCount` instructions sauf arrêt (fault/halt/stop-on-pc).
  - `reason` ∈ {Completed, Halted, Faulted, StoppedOnPc, MaxStepsHit}

- **RunFor(handle, budget) → {executed, reason} or error**
  - “budget” est un contrat d’exécution côté UE (par frame/tick); le détail (time vs step budget) sera décidé en implémentation.

### Inspection

- **GetCpuState(handle) → {pc, ...} or error**
  - Le minimum requis est `pc`.
  - Peut être étendu (regs) tant que stable et documenté.

- **GetCounters(handle) → {stepsExecuted, ...} or error**

### Observability

- **SetLogConfig(handle, level, categories) → ok or error**
- **SetOutputPaths(handle, {logPath?, tracePath?, gpuDumpPath?}) → ok or error**
  - MUST supporter chemins UE (espaces/non-ASCII).
  - MUST échouer explicitement si le chemin est invalide/non writable.

- **SetLogCallback(handle, callback) → ok or error**
  - Le callback peut être nul pour désactiver.
  - MUST ne pas bloquer l’émulation de façon non bornée (politique de backpressure à définir).

## Errors (contract)

Toutes les erreurs MUST:
- être explicites (message + code)
- ne pas laisser l’instance dans un état “à moitié initialisé” sans moyen de recovery (Reset/Destroy)

