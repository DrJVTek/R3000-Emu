# Feature Specification: UE5 Integration

**Feature Branch**: `[001-ue5-integration]`  
**Created**: 2026-01-25  
**Status**: Draft  
**Input**: User description: "il faut que ce code soit intégrable dans un projet UE5"

## User Scenarios & Testing *(mandatory)*

<!--
  IMPORTANT: User stories should be PRIORITIZED as user journeys ordered by importance.
  Each user story/journey must be INDEPENDENTLY TESTABLE - meaning if you implement just ONE of them,
  you should still have a viable MVP (Minimum Viable Product) that delivers value.
  
  Assign priorities (P1, P2, P3, etc.) to each story, where P1 is the most critical.
  Think of each story as a standalone slice of functionality that can be:
  - Developed independently
  - Tested independently
  - Deployed independently
  - Demonstrated to users independently
-->

### User Story 1 - Intégrer le coeur emu dans UE5 (Priority: P1)

En tant que développeur UE5, je veux intégrer le coeur R3000-Emu dans un projet Unreal Engine 5, afin de l’utiliser comme brique “runtime” (sans dépendances host non maîtrisées).

**Why this priority**: C’est la contrainte principale: si l’intégration UE5 échoue, le reste des fonctionnalités n’est pas exploitable côté Unreal.

**Independent Test**: Un projet UE5 peut référencer le module/livrable et compiler; l’éditeur démarre et peut instancier l’intégration sans crash.

**Acceptance Scenarios**:

1. **Given** un projet UE5 vierge, **When** j’ajoute la brique R3000-Emu au projet, **Then** la compilation et le linking UE5 réussissent sans modifications “ad-hoc” au moteur.
2. **Given** un build UE5 (Debug/Development), **When** j’exécute une scène qui instancie l’intégration, **Then** le runtime ne crashe pas et expose au moins un signal de vie (log/texte).
3. **Given** le repo R3000-Emu, **When** je compile la CLI, **Then** elle compile et reste fonctionnelle (non-régression) afin de servir de harness de test.

---

### User Story 2 - API stable pour piloter l’émulation (Priority: P2)

En tant que développeur UE5, je veux une API stable pour créer/détruire une instance d’émulation, charger un BIOS/une image CD/une ROM, et stepper l’exécution, afin de l’intégrer dans la boucle UE (tick/threads/outils).

**Why this priority**: Après l’intégration compile/link, il faut pouvoir piloter l’émulation de façon déterministe depuis UE.

**Independent Test**: Depuis UE5, on peut créer une instance, charger une ressource, exécuter N steps, puis arrêter proprement.

**Acceptance Scenarios**:

1. **Given** une instance d’émulateur, **When** je charge un BIOS et exécute N steps, **Then** l’état progresse et je peux récupérer des compteurs/logs.
2. **Given** une instance d’émulateur en cours, **When** je détruis l’instance, **Then** toutes les ressources sont libérées sans fuite/crash.

---

### User Story 3 - Observabilité UE-friendly (Priority: P3)

En tant que développeur UE5, je veux récupérer les logs/trace/dumps (GPU dump, outtext) via des points d’intégration adaptés, afin de diagnostiquer les problèmes sans dépendre d’une console hôte.

**Why this priority**: UE5 n’est pas toujours lancé depuis un terminal; il faut des sorties exploitables dans l’éditeur et en build.

**Independent Test**: Les logs principaux sont visibles dans l’environnement UE (ou redirigés vers un fichier configuré), et le dump GPU peut être écrit dans un chemin fourni par l’intégration.

**Acceptance Scenarios**:

1. **Given** une exécution dans UE5, **When** l’émulateur loggue des événements, **Then** ils sont récupérables via l’intégration (ex: callback/collecte) ou via un fichier configuré.

---

[Add more user stories as needed, each with an assigned priority]

### Edge Cases

- Que se passe-t-il si l’environnement UE5 interdit l’accès disque (sandbox/permissions) ? (logs/dumps)
- Que se passe-t-il si les chemins contiennent des caractères non ASCII / espaces ? (BIOS/CD/dumps)
- Que se passe-t-il si l’intégration est utilisée en build “shipping” (observabilité limitée) ?
- Que se passe-t-il si plusieurs instances tournent en parallèle (séparation des logs/ressources) ?

## Scope

### In Scope

- Intégration du coeur R3000-Emu dans un projet UE5 (compilation + linking + instanciation runtime).
- API de contrôle (création/destruction, chargement d’artefacts, exécution par steps).
- Observabilité exploitable dans UE (logs/trace/dumps redirigeables).

### Out of Scope

- Implémentation complète/accurate de tout le hardware PS1 (objectif: intégration, pas “full emulator”).
- Rendu vidéo final UE (le pont GPU dump peut exister, mais pas de renderer complet imposé ici).
- Support “GPU/SPU complet” dans la **CLI** (la CLI reste un harness de tests; le rendu/son est attendu côté UE5).

## Requirements *(mandatory)*

<!--
  ACTION REQUIRED: The content in this section represents placeholders.
  Fill them out with the right functional requirements.
-->

### Functional Requirements
- **FR-001**: Le système MUST fournir une intégration UE5 qui compile/link sur Windows (toolchain UE) sans modification du moteur.
- **FR-002**: Le système MUST permettre de créer/détruire une instance d’émulation depuis UE5 de façon sûre (sans crash).
- **FR-003**: Le système MUST permettre de charger un BIOS et/ou des images disque/ROM depuis des chemins fournis par l’utilisateur UE5.
- **FR-004**: Le système MUST permettre d’exécuter un nombre déterministe de steps et de récupérer un état minimal (ex: PC, counters).
- **FR-005**: Le système MUST exposer une observabilité utilisable dans UE (logs/trace/texte), sans dépendre d’un terminal hôte.
- **FR-006**: Le système MUST respecter la constitution du repo (pas de stubs/hacks BIOS implicites) même en contexte UE.
- **FR-007**: Le système MUST rester compatible avec la **CLI existante** (la CLI doit continuer de compiler et de pouvoir exécuter les scénarios de test manuels).
  - Note: ce contrat de compatibilité n’implique pas que la CLI fournisse un rendu GPU ou une sortie audio SPU.

### Key Entities *(include if feature involves data)*
- **UE Integration**: couche d’adaptation qui référence le coeur R3000-Emu et expose une API UE-friendly (création instance, chargement, stepping, logs).
- **Emulation Instance**: une instance isolée avec son état CPU/bus/devices et ses sinks d’observabilité.
- **Artifact Paths**: chemins vers BIOS/CD/dumps fournis par le projet UE5.

## Assumptions & Dependencies

- UE5 est une version 5.x “courante” (ex: 5.3+), Windows.
- L’intégration ne doit pas introduire de hacks BIOS implicites (constitution du repo).
- Les chemins (BIOS/CD/dumps) sont fournis par le projet UE (pas de chemins hardcodés non-overridable).

## Success Criteria *(mandatory)*

<!--
  ACTION REQUIRED: Define measurable success criteria.
  These must be technology-agnostic and measurable.
-->

### Measurable Outcomes
- **SC-001**: Un projet UE5 peut compiler/link l’intégration sur Windows sans patch du moteur et sans erreurs de build.
- **SC-002**: Une scène UE5 peut instancier l’émulateur et exécuter au moins 1 million de steps sans crash.
- **SC-003**: Les logs essentiels (erreurs + événements de run) sont consultables depuis UE (ou un fichier configurable) sur 100% des runs.
- **SC-004**: Les ressources (fichiers/dumps/instances) se libèrent correctement à la destruction (pas de crash lors de fermeture éditeur).
- **SC-005**: La CLI compile et exécute les tests manuels existants (ex: `--load=...`, `--debug-bios`) sur 100% des runs de validation.
  - Note: pas d’exigence “rendu GPU” / “audio SPU” côté CLI.
