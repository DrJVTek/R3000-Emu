# Tasks: R3000-Emu (PS1 emulator CLI)

**Input**: `specs/main/spec.md`, `specs/main/plan.md`, `.specify/memory/constitution.md`  
**Output**: CLI `r3000_emu` + loaders (ELF/PS-X EXE) + BIOS boot + CDROM + GPU dump + GTE (COP2) + logs/trace  
**Tests**: manuels (programmes guest + BIOS) comme défini dans `specs/main/spec.md`

## Phase 1: Setup (Build + repo hygiene + constitution alignment)

- [x] T001 Vérifier que `CMakeLists.txt` compile bien toutes les sources actuelles (fichier: `CMakeLists.txt`)
- [x] T002 Build Windows: `cmake -S . -B build && cmake --build build -j` (référence doc: `README.md`)
- [x] T003 [P] Mettre à jour le “Constitution Check” dans `specs/main/plan.md` pour refléter la constitution réelle (pas de “template”) (fichier: `specs/main/plan.md`)
- [x] T004 [P] Vérifier que `README.md` reflète les options CLI réellement supportées par `src/main.cpp` (usage + exemples) (fichiers: `README.md`, `src/main.cpp`)

---

## Phase 2: Foundational (Blocking prerequisites: no fallback + MMIO/IRQ correctness)

**⚠️ CRITICAL**: cette phase est non-négociable (constitution: “pas de simulation/placeholder”, “pas de fallback silencieux”, “diagnostics ne changent pas l’émulation”).

- [x] T005 Corriger/valider les erreurs loader pour inclure le chemin (`could not open '<path>'`) et un message explicite (fichier: `src/loader/loader.cpp`)
- [x] T006 Corriger/valider “unknown file format” pour inclure `auto|elf|psxexe` et message explicite (fichier: `src/loader/loader.cpp`)
- [x] T007 Valider qu’un segment ELF/PSX EXE hors RAM échoue proprement (pas d’overflow silencieux) (fichier: `src/loader/loader.cpp`)
- [x] T008 Supprimer tout fallback implicite CD (taille d’image non multiple de 2048/2352 => erreur) (fichier: `src/cdrom/cdrom.cpp`)
- [x] T009 Valider que `--debug-bios` est strictement diagnostic (n’altère pas l’émulation) et documenter précisément (fichiers: `src/main.cpp`, `README.md`)
- [x] T010 Valider les sémantiques d’IRQ I_STAT/I_MASK (ack, masquage) + edge-trigger des IRQ devices (fichier: `src/r3000/bus.cpp`)
- [x] T011 Implémenter SPU “bring-up” non-placeholder: `SPUCNT/SPUSTAT` (apply delay + busy) et transferts “manual write” FIFO/addr/ctrl, selon PSX-SPX (fichiers: `src/r3000/bus.h`, `src/r3000/bus.cpp`)
- [x] T012 Implémenter DMA4 (SPU) minimal mais réel: déclencher transferts DMA write/read quand `SPUCNT` est en mode DMA, et refléter `SPUSTAT.bit8/bit9/bit10` (fichier: `src/r3000/bus.cpp`)

**Checkpoint**: BIOS ne boucle plus sur `SPUSTAT` (on observe `SPUSTAT` changer: bits0-5 appliqués + bit10 busy qui retombe).

---

## Phase 3: User Story 1 - Exécuter un ELF “guest” et voir des logs (Priority: P1) 🎯 MVP

**Goal**: charger `hello.elf`, exécuter, avoir trace + sortie guest visible.

- [ ] T013 [US1] Vérifier que `examples/hello/build.ps1` génère `examples/hello/build/hello.elf` (fichier: `examples/hello/build.ps1`)
- [ ] T014 [US1] Valider la sortie guest (SYSCALL host et/ou MMIO print) + logs configurables (fichiers: `src/r3000/cpu.cpp`, `src/r3000/bus.cpp`, `README.md`)
- [ ] T015 [US1] Valider l’acceptance “chemin invalide” sur `--load=`: message explicite + exit code non-zéro, sans crash (fichiers: `src/main.cpp`, `src/loader/loader.cpp`)

---

## Phase 4: User Story 4 - Boot BIOS PS1 depuis la ROM (Priority: P1)

**Goal**: booter un BIOS depuis `0xBFC00000`, ROM lisible à `0x1FC00000` (alias), sans hacks BIOS.

- [ ] T016 [US4] Valider reset PC et mapping ROM (fichiers: `src/main.cpp`, `src/r3000/bus.h`, `src/r3000/bus.cpp`, `src/r3000/cpu.cpp`)
- [ ] T017 [US4] Valider que la memory map minimale couvre scratchpad + MMIO requis par BIOS (pas “retourne 0”), en implémentant les registres réellement touchés (fichiers: `src/r3000/bus.h`, `src/r3000/bus.cpp`)
- [ ] T018 [US4] Valider syscall `Enter/ExitCriticalSection` (IEc COP0) et propagation IRQ (fichier: `src/r3000/cpu.cpp`)

---

## Phase 5: User Story 2 - Charger un PS-X EXE et démarrer à l’entry point (Priority: P2)

**Goal**: charger un PS-X EXE, copier code à `t_addr`, zéro BSS, init PC/GP/SP.

- [ ] T019 [US2] Valider parsing header PS-X EXE + copie + init `entry_pc/gp/sp` (fichiers: `src/loader/loader.cpp`, `src/loader/loader.h`)
- [ ] T020 [US2] Ajouter logs de chargement (tailles/adresses) derrière `--log-level=debug` (fichiers: `src/loader/loader.cpp`, `src/main.cpp`)

---

## Phase 6: User Story 5 - Lire un CD (image) depuis le BIOS (Priority: P2)

**Goal**: insérer une image CD et permettre des lectures via MMIO CDROM; gérer 2048 et 2352 (XA/Mode2) sans “assume”.

- [ ] T021 [US5] Valider `--cd=`: insert disc + erreurs propres (formats 2048/2352/.cue) (fichiers: `src/main.cpp`, `src/cdrom/cdrom.cpp`, `src/cdrom/cdrom.h`)
- [ ] T022 [US5] Valider extraction 2048 bytes depuis 2352 Mode1 (offset 16) et Mode2 Form1 (offset 24h/32) selon doc (fichier: `src/cdrom/cdrom.cpp`)
- [ ] T023 [US5] Valider MMIO CDROM `0x1F801800..803`: banking, FIFOs, IRQ, commandes nécessaires au BIOS (`GetStat`, `Setloc`, `ReadN/ReadS`, `GetID`, etc.) (fichiers: `src/cdrom/cdrom.cpp`, `src/cdrom/cdrom.h`, `src/r3000/bus.cpp`)

**Checkpoint**: après la phase SPU, le BIOS atteint l’init CDROM et génère des accès `0x1F80180x`.

---

## Phase 7: User Story 6 - Capturer les commandes GPU pour l’affichage Unreal (Priority: P2)

**Goal**: dumper GP0/GP1 en paires `[port,value]` via `--gpu-dump=...`.

- [ ] T024 [US6] Valider que les writes GPU (MMIO + DMA2 linked-list) appellent `Gpu::mmio_write32()` et alimentent le dump (fichiers: `src/r3000/bus.cpp`, `src/gpu/gpu.cpp`)
- [ ] T025 [US6] Garantir flush/close correct du dump à la fin du programme (fichiers: `src/gpu/gpu.h`, `src/gpu/gpu.cpp`, `src/main.cpp`)
- [ ] T026 [US6] Documenter le format exact du dump et un exemple de lecture côté outil externe (fichier: `README.md`)

---

## Phase 8: User Story 7 - Preset CLI pour debug BIOS (Priority: P2)

**Goal**: `--debug-bios` configure automatiquement trace/logs/stops (sans modifier l’émulation).

- [ ] T027 [US7] Vérifier que `--debug-bios` force `--pretty` + `log-level=debug` + cats incluant `exec,exc,mem` (fichiers: `src/main.cpp`, `README.md`)
- [ ] T028 [US7] Vérifier stop condition “BIOS→RAM NOP” (diagnostic) (fichiers: `src/r3000/cpu.cpp`, `src/main.cpp`)
- [ ] T029 [US7] Documenter précisément l’interaction `--debug-bios` vs options d’instrumentation (sans HLE/hack implicite) (fichiers: `src/main.cpp`, `README.md`)

---

## Phase 9: User Story 3 - Utiliser le GTE (COP2) depuis un programme guest (Priority: P3)

**Goal**: exécuter transferts COP2 + une commande GTE sans exception RI; observer changements registres.

- [ ] T030 [US3] Valider COP2 transferts (MFC2/MTC2/CFC2/CTC2) + LWC2/SWC2 (fichiers: `src/r3000/cpu.cpp`, `src/gte/gte.*`)
- [ ] T031 [US3] Valider au moins une commande GTE via `gte_.execute()` (ex: RTPS/MVMVA/NCLIP) (fichiers: `src/r3000/cpu.cpp`, `src/gte/gte.cpp`)
- [ ] T032 [US3] Ajouter un exemple guest dédié COP2/GTE et l’intégrer au README (fichiers: `examples/gte_demo/*`, `README.md`)

---

## Phase 10: Polish & Cross-Cutting Concerns

- [ ] T033 [P] Aligner `README.md` avec l’usage imprimé par `print_usage()` (éviter drift) (fichiers: `README.md`, `src/main.cpp`)
- [ ] T034 [P] Ajouter/renforcer messages d’erreur “edge cases” (segments hors RAM, image CD invalide, bios oversize) (fichiers: `src/loader/loader.cpp`, `src/cdrom/cdrom.cpp`, `src/main.cpp`)
- [ ] T035 Nettoyer toute terminologie “stub/simulation” dans plan/spec/tasks et la remplacer par “minimal mais correct” / “non implémenté => erreur explicite” (fichiers: `specs/main/spec.md`, `specs/main/plan.md`, `specs/main/tasks.md`)
- [ ] T036 [P] Ajouter un flag diagnostic `--stop-on-pc=0xXXXXXXXX` à la doc/usage (et préciser “diagnostic-only”) (fichiers: `README.md`, `src/main.cpp`, `src/r3000/cpu.h`, `src/r3000/cpu.cpp`)
- [ ] T037 Identifier la boucle BIOS actuelle (ex: `PC=0x80061CDC`) via `--stop-on-pc` + `--trace-io`, puis implémenter uniquement le MMIO/IRQ manquant (sans hack BIOS) (fichiers: `src/r3000/bus.cpp` et/ou `src/gpu/gpu.*` et/ou `src/cdrom/cdrom.*` et/ou `src/r3000/cpu.cpp`)

---

## Dependencies & Execution Order

- **Phase 1** → **Phase 2**: nécessaire avant toute validation user-story.
- **Phase 2**: débloque l’étape BIOS qui attend le SPU (poll SPUSTAT).
- **US1 (Phase 3)**: MVP (ELF + output).
- **US4 (Phase 4)**: dépend de Phase 2 (IRQ/MMIO correct).
- **US5 (Phase 6)**: dépend de Phase 2 + US4 (BIOS progression) pour atteindre CDROM init réel.

## Parallel Opportunities

- Les tâches marquées **[P]** peuvent être faites en parallèle (fichiers séparés, peu de dépendances).

