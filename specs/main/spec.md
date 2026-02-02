# Feature Specification: R3000-Emu (PS1 emulator CLI)

**Feature Branch**: `[main]`  
**Created**: 2026-01-24  
**Status**: Draft  
**Input**: User description (résumé) : émulateur éducatif PS1 (R3000 + GTE), CLI avec logs/trace, charge des binaires externes (ELF/PS-X EXE) pour démos live.

## User Scenarios & Testing *(mandatory)*

### User Story 1 - Exécuter un ELF “guest” et voir des logs (Priority: P1)

En tant que présentateur live, je veux compiler un petit programme MIPS (ELF) et le lancer dans l’émulateur, afin d’obtenir une démo reproductible (trace + sorties “printf” côté guest).

**Why this priority**: C’est la boucle “valeur” la plus directe pour le live (build → run → output), sans dépendre d’un BIOS/ISO.

**Independent Test**: Construire `examples/hello` puis lancer `r3000_emu` avec `--load=... --format=elf`, et observer une sortie visible (trace + messages guest).

**Acceptance Scenarios**:

1. **Given** un binaire `hello.elf` valide, **When** je lance `r3000_emu --load=hello.elf --format=elf`, **Then** l’image est chargée en RAM, le PC est initialisé, et l’exécution produit au moins une sortie guest observable.
2. **Given** un chemin invalide, **When** je lance `--load=...`, **Then** l’émulateur échoue proprement avec un message d’erreur (sans crash).

---

### User Story 2 - Charger un PS-X EXE et démarrer à l’entry point (Priority: P2)

En tant qu’utilisateur, je veux pouvoir charger un binaire PS-X EXE minimal et démarrer à son entry point, afin de tester la compatibilité “PS1-ish” des adresses et du layout mémoire.

**Why this priority**: Montre un format PS1 concret (au-delà d’ELF bare-metal) et valide les conventions d’adressage.

**Independent Test**: Disposer d’un PS-X EXE minimal, le charger via `--format=psxexe` et vérifier que le loader place le code/BSS au bon endroit.

**Acceptance Scenarios**:

1. **Given** un PS-X EXE valide, **When** je lance `r3000_emu --load=... --format=psxexe`, **Then** la section code est copiée à `t_addr`, la BSS est zérotée, et le PC/GPR init sont configurés.

---

### User Story 3 - Utiliser le GTE (COP2) depuis un programme guest (Priority: P3)

En tant que présentateur, je veux montrer que les instructions COP2 (GTE) sont déléguées à un module séparé, afin d’expliquer l’architecture PS1 (CPU + coprocesseur math).

**Why this priority**: Le GTE est une partie “signature” PS1, utile pour la pédagogie.

**Independent Test**: Exécuter un programme guest qui fait au moins des transferts COP2 (MTC2/MFC2, LWC2/SWC2) et une commande GTE, puis observer des effets mesurables (registres ou sorties).

**Acceptance Scenarios**:

1. **Given** un guest qui exécute des transferts COP2 et une commande GTE, **When** je lance l’émulateur, **Then** l’exécution ne déclenche pas d’exception RI et les registres GTE reflètent des changements attendus.

---

### User Story 4 - Boot BIOS PS1 depuis la ROM (Priority: P1)

En tant que présentateur, je veux pouvoir **booter un BIOS PS1** (fichier `.bin`) depuis le reset vector, afin de passer à une exécution “console-like” (même si le hardware n’est pas encore complet).

**Why this priority**: C’est le chemin naturel vers “boot CD” ensuite, et ça force à mettre une memory map PS1 minimale (BIOS ROM + I/O stubs).

**Independent Test**: Lancer `r3000_emu --bios=<bios.bin> --max-steps=N` et vérifier qu’on fetch des instructions depuis `0xBFC00000` et qu’on ne meurt pas immédiatement sur un fault I/O basique.

**Acceptance Scenarios**:

1. **Given** un fichier BIOS valide, **When** je lance `r3000_emu --bios=...`, **Then** le PC initial est `0xBFC00000` et la ROM est lisible à `0x1FC00000` (alias).
2. **Given** un BIOS qui touche des registres I/O de base, **When** il fait des reads/writes, **Then** le bus répond (stub) sans “out_of_range” immédiat.

---

### User Story 5 - Lire un CD (image) depuis le BIOS (Priority: P2)

En tant que présentateur, je veux pouvoir **insérer une image CD** (idéalement **2352 bytes/secteur** pour CD-XA / Mode2)
afin de commencer le chemin “boot CD”.

**Why this priority**: C’est la prochaine grosse brique après le BIOS: CDROM + ISO9660/XA.

**Independent Test**: Lancer `r3000_emu --cd=<image>` en mode BIOS et vérifier que les accès MMIO CDROM ne faultent pas,
et qu’une lecture de secteur peut retourner des bytes (via FIFO) quand le BIOS/driver envoie une commande de lecture.

**Acceptance Scenarios**:

1. **Given** une image disque valide (2048 ou 2352), **When** on l’insère (`--cd=...`), **Then** le device peut lire un secteur LBA et exposer 2048 bytes “user data”.
2. **Given** une image 2352 (CD-XA), **When** on lit un secteur Mode2, **Then** on extrait correctement les 2048 bytes de data (Form1) depuis l’offset standard.

---

### User Story 6 - Capturer les commandes GPU pour l’affichage Unreal (Priority: P2)

En tant que présentateur, je veux pouvoir **capturer le flux de commandes GPU** (GP0/GP1) pendant l’exécution,
afin de le rejouer / afficher côté Unreal (pont “SWINREAL”).

**Why this priority**: Ça permet de montrer quelque chose à l’écran sans implémenter tout le GPU en rendu software dans l’émulateur.

**Independent Test**: Lancer l’émulateur avec `--gpu-dump=...` et vérifier que le fichier grossit quand un programme écrit sur GP0/GP1.

**Acceptance Scenarios**:

1. **Given** un programme qui écrit des commandes GPU, **When** je lance avec `--gpu-dump=file.bin`, **Then** on obtient une suite de paires `[port,value]`.

---

### User Story 7 - Preset CLI pour debug BIOS (Priority: P2)

En tant que présentateur, je veux un flag CLI unique (ex: `--debug-bios`) qui active un preset de debug (logs + trace + stop conditions),
afin de suivre facilement ce qui est exécuté pendant le boot BIOS et le boot CD.

**Why this priority**: Ça évite de retaper `--log-level/--log-cats/--pretty` en live, et rend le diagnostic reproductible.

**Independent Test**: Lancer `r3000_emu --debug-bios --cd=<game.cue> --max-steps=N` et vérifier que la trace et les logs attendus sont actifs.

**Acceptance Scenarios**:

1. **Given** `--debug-bios`, **When** je lance l’émulateur, **Then** le logger passe en `debug` et active au moins `exec,exc,mem`, et `--pretty` est actif.
2. **Given** `--debug-bios`, **When** le BIOS saute vers des NOPs en RAM, **Then** l’émulateur s’arrête sur le stop condition “BIOS→RAM NOP” avec un mini-trace.

### Edge Cases

- Que se passe-t-il si `--format=auto` ne reconnaît pas le fichier ? (message d’erreur, codes de retour)
- Que se passe-t-il si un segment ELF/PSX EXE dépasse la RAM émulée ? (erreur propre)
- Que se passe-t-il sur accès mémoire non aligné (LW/LH/SW/SH) ? (exception attendue vs “undefined”)
- Que se passe-t-il si le guest exécute une instruction non supportée ? (exception RI + logs)
- Que se passe-t-il si le BIOS accède à du hardware non stub (GPU/SPU/CDROM) ? (fault explicite vs stub “0” + log)
- Que se passe-t-il si l’image CD n’est pas multiple de 2048/2352 ? (erreur / mode fallback)
- Que se passe-t-il si l’image n’est pas XA-compatible (pas de Mode2) ? (comportement documenté)

## Requirements *(mandatory)*

### Functional Requirements

- **FR-001**: Le système MUST fournir un exécutable CLI `r3000_emu` capable de charger un binaire externe en RAM et d’exécuter à partir d’un entry point.
- **FR-002**: Le système MUST supporter `--load=<path>` et `--format=auto|elf|psxexe` pour le chargement (auto-détection quand possible).
- **FR-003**: Le système MUST émuler le CPU R3000 (MIPS I) avec un modèle d’exécution interprété (fetch/decode/execute).
- **FR-004**: Le système MUST gérer les “delay slots” pertinents (branch delay slot, load delay slot) de manière documentée.
- **FR-005**: Le système MUST exposer une trace/log configurable via CLI (`--pretty`, `--log-level`, `--log-cats`).
- **FR-006**: Le système MUST supporter les accès COP2 (GTE) via un module séparé du CPU (architecture claire).
- **FR-007**: Le système MUST fournir au moins un mécanisme “printf/trace guest” observable depuis l’hôte (ex: SYSCALLs dédiés et/ou MMIO).
- **FR-008**: Le système MUST supporter un mode “boot BIOS” qui charge une ROM BIOS et démarre à `0xBFC00000` (alias `0x1FC00000` physique).
- **FR-009**: Le bus MUST implémenter une memory map PS1 minimale pour le boot BIOS (au moins BIOS ROM + scratchpad + I/O stub), afin d’éviter des faults immédiats.
- **FR-010**: Le système SHOULD booter un BIOS par défaut depuis `bios/ps1_bios.bin` si aucun argument n’est donné (override via `--bios=`).
- **FR-011**: Le système MUST pouvoir insérer une image CD et lire des secteurs (2048 et 2352). 2352 est requis pour CD-XA / Mode2.
- **FR-012**: Le système SHOULD permettre de dumper les writes GPU (GP0/GP1) pour un renderer externe (Unreal).
- **FR-013**: Le système SHOULD fournir un preset `--debug-bios` qui configure automatiquement trace/logs/stops pour le boot BIOS.

### Key Entities *(include if feature involves data)*

- **Loaded Image**: paramètres retournés par un loader (entry PC, GP, SP) + état “présent/absent”.
- **Guest RAM**: mémoire linéaire émulée (taille fixe) dans laquelle les binaires sont chargés.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Un `hello.elf` minimal peut être chargé et exécuté, produisant une sortie guest visible sur la console hôte.
- **SC-002**: En cas d’erreur de chargement (fichier absent, format inconnu), l’émulateur termine proprement avec un message explicite.
- **SC-003**: Les options CLI documentées dans `README.md` correspondent à des comportements définis (niveaux/catégories de logs, mode trace).
- **SC-004**: Un BIOS PS1 peut démarrer et exécuter un nombre non-trivial d’instructions sans “mem fault” immédiat sur la ROM ou les zones I/O de base (avec `--max-steps` pour arrêter proprement).
- **SC-005**: Une image 2352 bytes/secteur peut être lue et fournir 2048 bytes “user data” (Mode1 ou Mode2 Form1).
- **SC-006**: Un dump GPU (`--gpu-dump`) contient des paires `[port,value]` quand le code écrit dans les registres GPU.
- **SC-007**: `--debug-bios` active une trace lisible + logs de debug sans options additionnelles.

