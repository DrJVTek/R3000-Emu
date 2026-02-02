# Tasks: UE5 Integration (R3000-Emu)

**Input**: Design documents from `specs/001-ue5-integration/`  
**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`  
**Tests**: tests “smoke” manuels (CLI + UE5) comme décrit dans `quickstart.md` (pas de TDD imposé ici)  

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: préparer la séparation core/CLI et le squelette UE5 sans casser l’existant.

- [x] T001 [P] Créer le dossier `integrations/ue5/` (et sous-dossiers) selon `specs/001-ue5-integration/plan.md`
- [x] T002 [P] Ajouter un `README.md` minimal dans `integrations/ue5/` expliquant comment l’intégration sera consommée (fichier: `integrations/ue5/README.md`)
- [x] T003 [P] Ajouter une CI/commande locale “build CLI” reproductible (doc uniquement) sans ajouter de nouveaux outils (fichier: `README.md`)

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: rendre le core réutilisable par la CLI et par UE5, sans hardcode, sans hacks, sans stubs.

**⚠️ CRITICAL**: aucune intégration UE5 sérieuse n’est possible si le core reste couplé à la CLI.

- [x] T004 Extraire une API core stable (instance lifecycle + step + inspect) sans dépendance UE (fichiers: `src/*` + nouveaux fichiers sous `src/` si nécessaire)
- [x] T005 Refactor la CLI (`src/main.cpp`) pour consommer l’API core (pas de duplication de logique) (fichier: `src/main.cpp`)
- [x] T006 Ajouter des structures/handles “EmulationInstance” (concept) et une config d’exécution (concept) côté core (fichiers: `src/**`)
- [x] T007 Garantir “no hardcoded non-overridable config” sur les chemins BIOS/CD/dumps côté core+CLI (fichiers: `src/main.cpp`, `src/**`)
- [x] T008 Valider que les flags “diagnostic” restent non-intrusifs (ne changent pas l’émulation) (fichiers: `src/main.cpp`, `src/r3000/cpu.*`, `src/r3000/bus.*`)

**Checkpoint**: la CLI compile et tourne (smoke test CLI) après ce refactor.

---

## Phase 3: User Story 1 - Intégrer le coeur emu dans UE5 (Priority: P1) 🎯 MVP

**Goal**: intégrer le core dans UE5, compiler/link, instancier sans crash.

**Independent Test**:
- CLI: compile + run un scénario existant (harness)
- UE5: un module/plugin compile et une scène peut instancier l’emu et exécuter des steps (sans GPU/SPU côté CLI)

### Implementation for User Story 1

- [x] T009 [US1] Créer un squelette d’intégration UE5 (plugin ou module) sous `integrations/ue5/` (fichiers: `integrations/ue5/**`)
- [x] T010 [US1] Ajouter un module Runtime UE qui compile le core depuis sources (pas de prebuilt par défaut) (fichiers: `integrations/ue5/**`)
- [x] T011 [US1] Exposer une façade UE5 minimale (create/destroy instance) qui appelle le core (fichiers: `integrations/ue5/**`)
- [x] T012 [US1] Ajouter un “smoke actor/component” UE pour instancier et détruire l’instance sans crash (fichiers: `integrations/ue5/**`)
- [x] T013 [US1] Documenter le chemin de build/usage UE (editor) + test “instanciation” (fichiers: `integrations/ue5/README.md`, `specs/001-ue5-integration/quickstart.md`)

**Checkpoint**: UE5 compile/link et peut instancier/détruire une instance d’émulation sans crash.

---

## Phase 4: User Story 2 - API stable pour piloter l’émulation (Priority: P2)

**Goal**: permettre depuis UE5 de charger BIOS/CD, stepper, inspecter (PC/counters) selon `contracts/emu-api.md`.

**Independent Test**: dans UE5, créer une instance, charger un BIOS, stepper N instructions, lire PC/counters, reset/destroy.

### Implementation for User Story 2

- [x] T014 [US2] Implémenter `LoadBios` via chemins UE (espaces/non-ASCII) et erreurs explicites (fichiers: `integrations/ue5/**`, `src/**`)
- [x] T015 [US2] Implémenter `InsertDisc` via chemins UE et erreurs explicites (pas de fallback) (fichiers: `integrations/ue5/**`, `src/cdrom/**`)
- [x] T016 [US2] Implémenter `Step(stepsCount)` et retour `{executed, reason}` exposé côté UE (fichiers: `integrations/ue5/**`, `src/r3000/cpu.*`)
- [x] T017 [US2] Implémenter `GetCpuState` (minimum `pc`) et `GetCounters` exposés côté UE (fichiers: `integrations/ue5/**`, `src/**`)
- [x] T018 [US2] Implémenter `Reset` cohérent (aligné CLI) et safe (fichiers: `integrations/ue5/**`, `src/**`)

**Checkpoint**: dans UE5, on peut load BIOS + stepper + lire PC + reset + destroy.

---

## Phase 5: User Story 3 - Observabilité UE-friendly (Priority: P3)

**Goal**: logs/trace/dumps accessibles dans UE (Output Log et/ou fichiers) sans dépendre d’un terminal.

**Independent Test**: dans UE5, observer au moins des logs (erreurs + événements run) et configurer une redirection fichier.

### Implementation for User Story 3

- [x] T019 [US3] Définir un sink de logs UE (Output Log) qui ne bloque pas l’émulation (politique claire) (fichiers: `integrations/ue5/**`)
- [x] T020 [US3] Ajouter `SetLogConfig(level,categories)` côté UE vers core/CLI (fichiers: `integrations/ue5/**`, `src/log/**`, `src/main.cpp`)
- [x] T021 [US3] Ajouter `SetOutputPaths({logPath,tracePath,gpuDumpPath})` côté UE (fichiers: `integrations/ue5/**`, `src/**`)
- [x] T022 [US3] S’assurer que l’observabilité reste “diagnostic-only” (pas d’effet sur l’émulation) (fichiers: `integrations/ue5/**`, `src/**`)

**Checkpoint**: logs visibles dans UE (ou fichier), et aucun crash même si le sink est lent (dégradation contrôlée).

---

## Phase 6: Polish & Cross-Cutting Concerns

**Purpose**: finir l’intégration et garantir non-régression CLI + hygiène repo.

- [x] T023 [P] Mettre à jour la doc racine pour pointer vers l’intégration UE5 (sans sur-documenter) (fichier: `README.md`)
- [x] T024 Nettoyer toute mention “GPU/SPU CLI” dans les docs de la feature si apparue (fichiers: `specs/001-ue5-integration/*.md`)
- [x] T025 [P] Vérifier que rien n’ajoute de chemins hardcodés non-overridable (revue ciblée) (fichiers: `src/**`, `integrations/ue5/**`)
- [ ] T026 Valider le quickstart complet (CLI smoke + UE5 smoke) et ajuster la doc si nécessaire (fichier: `specs/001-ue5-integration/quickstart.md`)

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: immédiat
- **Foundational (Phase 2)**: dépend de Phase 1, **bloque** toutes les user stories UE5
- **US1 (Phase 3)**: dépend de Phase 2
- **US2 (Phase 4)**: dépend de Phase 3
- **US3 (Phase 5)**: dépend de Phase 3 (et idéalement s’interface avec US2)
- **Polish (Phase 6)**: après les stories souhaitées

### User Story Dependencies

- **US1 (P1)**: core réutilisable + squelette UE5
- **US2 (P2)**: dépend de l’existence d’une façade UE + core pilotable
- **US3 (P3)**: dépend de l’intégration UE (US1) et touche la configuration/exposition (US2)

### Parallel Opportunities

- T001–T003 peuvent être faits en parallèle.
- Après T004–T008, une partie de l’intégration UE (T009–T013) peut se faire en parallèle avec la stabilisation doc (T003) tant que l’API core est figée.

---

## Implementation Strategy

### MVP First (US1)

1. Phase 1 → Phase 2 (core réutilisable, CLI non cassée)
2. Phase 3 (US1) → UE5 compile/link + instanciation
3. Stop, exécuter les smoke tests `specs/001-ue5-integration/quickstart.md`

