# Data Model: UE5 Integration

**Date**: 2026-01-25  
**Feature**: `specs/001-ue5-integration/spec.md`

> Note: ceci décrit les entités *conceptuelles* utiles pour les contrats d’API et la planification. Ce ne sont pas des classes/structures imposées.

## Entities

### 1) EmulationInstance

**Represents**: une instance isolée d’émulation (CPU + bus + devices + RAM/ROM + périphériques partiels).

**Key attributes**:
- Identité (handle) unique
- État d’exécution (running/paused/stopped)
- Compteurs (steps exécutés)
- État CPU minimal exposable (PC, éventuellement quelques registres pour debug)
- Sinks d’observabilité configurés (logs/trace/dumps)

**Constraints**:
- Création/destruction sûres (pas de fuite/crash)
- Multi-instances possibles (séparation stricte des ressources et sorties)

### 2) ArtifactPaths

**Represents**: chemins vers les artefacts d’entrée/sortie.

**Key attributes**:
- BIOS path (optionnel selon le mode)
- CD image path (optionnel)
- Output paths (logs/trace/gpu dump) (optionnels)

**Constraints**:
- Aucun chemin hardcodé non-overridable
- Support chemins avec espaces et non-ASCII

### 3) ObservabilitySink

**Represents**: “où vont” les logs/trace/dumps.

**Variants**:
- File sink (écriture fichier via chemin fourni)
- Callback sink (hook côté UE pour afficher dans Output Log / UI)

**Constraints**:
- Ne doit pas changer le comportement d’émulation (diagnostic-only)
- Backpressure: si le sink est lent, il doit dégrader l’observabilité sans casser l’émulation (ex: drop/flush policy explicitement définie)

### 4) EmulationConfig

**Represents**: paramètres d’exécution.

**Key attributes**:
- Limites (max steps, stop-on-pc si activé)
- Niveau/catégories de logs
- Options de runtime (ex: activer dumps)

**Constraints**:
- Les options “diagnostic” doivent rester non-intrusives

