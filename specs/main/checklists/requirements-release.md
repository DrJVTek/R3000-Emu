# Requirements Release Checklist: R3000-Emu (PS1 emulator CLI)

**Purpose**: “Unit tests” de qualité des exigences (release gate) pour vérifier que `spec.md`/`plan.md`/`tasks.md` sont complets, non ambigus, cohérents, mesurables et alignés constitution (sans stubs/hacks).
**Created**: 2026-01-25
**Feature**: `specs/main/spec.md` (+ `specs/main/plan.md`, `specs/main/tasks.md`, `.specify/memory/constitution.md`)

**Audience**: auteur (avant implémentation)

## Constitution & Scope (non-négociable)

- [ ] CHK001 Le vocabulaire “stub / simulation / placeholder / hack BIOS” est-il absent (ou explicitement interdit) dans `spec.md`/`plan.md`/`tasks.md` ? [Constitution §2, Gap]
- [ ] CHK002 Le “Constitution Check” de `plan.md` reflète-t-il bien la constitution réelle (et pas “template”) ? [Plan §Constitution Check, Conflict]
- [ ] CHK003 Les exigences indiquent-elles explicitement que les diagnostics (`--debug-bios`, traces) **ne changent pas** l’émulation ? [Constitution §Observability, Spec §US7, Clarity]
- [ ] CHK004 Les comportements “non implémentés” sont-ils décrits comme **erreur explicite** (et non un retour 0 silencieux) ? [Constitution §2, Spec §Edge Cases, Ambiguity]
- [ ] CHK005 Les “defaults” (BIOS par défaut, chemins, etc.) sont-ils explicitement **override via CLI** (pas de hardcode non‑overridable) ? [Constitution §3, Spec §FR-010, Assumption]

## Requirement Completeness (FR/US)

- [ ] CHK006 Chaque FR (FR-001..FR-013) a-t-il au moins un critère testable (manuel OK) dans la spec (ou référencé par tasks) ? [Spec §FR-001..FR-013, Traceability]
- [ ] CHK007 Les formats `--format=auto|elf|psxexe` ont-ils des règles d’auto‑détection écrites (signatures / heuristiques) ou une exclusion explicite ? [Spec §FR-002, Gap]
- [ ] CHK008 La spec définit-elle clairement la RAM émulée (taille, mapping, adresses physiques vs virtuelles KSEG) ? [Spec §Key Entities, Gap]
- [ ] CHK009 La spec définit-elle les exceptions minimales attendues (RI, ADEL/ADES, INT, SYS) et dans quels cas on les lève ? [Spec §Edge Cases, Gap]
- [ ] CHK010 “Delay slots” (branch + load) sont-ils listés explicitement avec cas couverts / non couverts ? [Spec §FR-004, Ambiguity]
- [ ] CHK011 Le périmètre BIOS boot mentionne-t-il les MMIO indispensables (au moins: IRQ ctrl, timers, DMA, SPU bring-up, CDROM) ? [Spec §US4, Gap]
- [ ] CHK012 Le périmètre CDROM mentionne-t-il les secteurs 2048 vs 2352 avec offsets exacts (Mode1, Mode2 Form1) et cas d’erreur ? [Spec §US5, Spec §FR-011, Clarity]
- [ ] CHK013 Le périmètre GPU dump définit-il ce qui est capturé (GP0/GP1, DMA2 linked-list, format `[port,value]`) et ce qui est exclu ? [Spec §US6, Spec §FR-012, Clarity]
- [ ] CHK014 Le périmètre GTE indique-t-il au moins 1 commande obligatoire (ex: RTPS) + transferts COP2 requis ? [Spec §US3, Completeness]

## Requirement Clarity & Measurability (pas de “vague”)

- [ ] CHK015 Les termes “minimal”, “bring-up”, “PS1-ish”, “console-like” sont-ils définis via critères mesurables ? [Spec §US4/US5, Ambiguity]
- [ ] CHK016 Le succès BIOS (SC-004) est-il quantifié (ex: nombre d’instructions, absence de fault, progression observable) ? [Spec §SC-004, Measurability]
- [ ] CHK017 Le succès CD (SC-005) précise-t-il comment vérifier le “user data” (logs/CRC/print) ? [Spec §SC-005, Measurability]
- [ ] CHK018 Les erreurs “propres” ont-elles une définition mesurable (exit code, message, catégorie log) ? [Spec §SC-002, Clarity]
- [ ] CHK019 `--debug-bios` a-t-il une définition complète (options exactes, logs exacts, stop conditions exactes) ? [Spec §US7, Measurability]
- [ ] CHK020 Les critères d’acceptance de US1/US2/US3/US4/US5/US6/US7 sont-ils tous vérifiables sans interprétation ? [Spec §US1..US7, Measurability]

## Consistency & Terminology (drift)

- [ ] CHK021 `plan.md` et `spec.md` sont-ils cohérents sur le scope (GPU/SPU/CDROM inclus ou exclus) ? [Plan §Scale/Scope, Spec §US5/US6, Conflict]
- [ ] CHK022 “I/O stub” est-il remplacé par “implémentation minimale mais correcte” ou “non implémenté => erreur explicite” partout ? [Constitution §2, Spec/Plan wording, Consistency]
- [ ] CHK023 Les noms des options CLI (`--pretty`, `--log-level`, `--log-cats`, `--trace-io`, etc.) sont-ils cohérents entre spec, tasks, README ? [Spec §FR-005/US7, Tasks, Gap]
- [ ] CHK024 Les termes “phys/virt”, “KUSEG/KSEG0/KSEG1”, “alias BIOS 0xBFC00000/0x1FC00000” sont-ils utilisés de façon cohérente ? [Spec §US4/FR-008, Consistency]
- [ ] CHK025 Les exigences “pas de fallback” (CD size) sont-elles cohérentes entre spec et tasks (pas de “assume”) ? [Constitution §2, Spec §Edge Cases, Tasks, Consistency]

## Scenario Coverage (Primary / Alternate / Error / Recovery)

- [ ] CHK026 US1 couvre-t-il explicitement: succès, fichier manquant, format inconnu, segment hors RAM ? [Spec §US1, Spec §Edge Cases, Coverage]
- [ ] CHK027 US2 couvre-t-il explicitement: header invalide, BSS hors RAM, adresses non alignées, comportement sur erreurs ? [Spec §US2, Gap]
- [ ] CHK028 US4 couvre-t-il explicitement: BIOS absent/oversize, MMIO manquant (quels registers), et stratégie “erreur explicite” ? [Spec §US4, Gap]
- [ ] CHK029 US5 couvre-t-il explicitement: image invalide, secteur-size indétectable, LBA hors disque, et erreurs attendues ? [Spec §US5, Coverage]
- [ ] CHK030 US6 couvre-t-il explicitement: chemin dump invalide, permissions, format versionné, fin de fichier (flush) ? [Spec §US6, Gap]
- [ ] CHK031 US7 couvre-t-il explicitement: interaction des flags (priorité/override), et ce qui ne doit jamais être activé implicitement (HLE/patch) ? [Spec §US7, Constitution §2, Coverage]

## Non-Functional Requirements (NFR) & Observability

- [ ] CHK032 Les catégories de logs (ex: exec/mem/exc/io/cdrom) sont-elles définies, avec règle de filtrage attendue ? [Spec §FR-005, Gap]
- [ ] CHK033 Les logs nécessaires aux “stop conditions” sont-ils garantis visibles même si filtres de catégories changent ? [Spec §US7, Gap]
- [ ] CHK034 Les exigences indiquent-elles clairement ce qui doit être deterministic/reproductible (seed, logs, steps) ? [Constitution §Observability, Gap]
- [ ] CHK035 La spec précise-t-elle ce qui est “pas cycle-accurate” mais néanmoins correct (ex: timers) et l’impact attendu ? [Plan §Performance/Scope, Ambiguity]

## Dependencies & Assumptions

- [ ] CHK036 Les toolchains externes (MSVC, mipsel-none-elf-gcc) sont-elles listées avec versions / instructions claires (sans ambiguité) ? [Plan §Target Platform, Spec §US1, Coverage]
- [ ] CHK037 Les sources de vérité hardware (PSX-SPX/no$psx) sont-elles référencées pour chaque bloc critique (SPU/CDROM/DMA/IRQ) ? [Constitution §Workflow, Gap]
- [ ] CHK038 L’hypothèse “BIOS doit progresser sans hacks” est-elle reflétée partout (et pas contredite par plan/tasks) ? [Constitution §2, Conflict]
- [ ] CHK039 L’hypothèse “pas de fallback CD” est-elle explicitée (comportement exact + message) ? [Constitution §2, Spec §Edge Cases, Clarity]

## Traceability & Acceptance Criteria Quality

- [ ] CHK040 Chaque story a-t-elle un “Independent Test” complet (commande + ce qu’on observe + logs attendus) ? [Spec §US1..US7, Traceability]
- [ ] CHK041 Chaque SC (SC-001..SC-007) mappe-t-il clairement à US/FR et à une preuve observable (log/dump/exit code) ? [Spec §SC-001..SC-007, Traceability]
- [ ] CHK042 Les tâches `tasks.md` ont-elles toutes un lien clair vers une FR/US (ou “polish” justifié) ? [Tasks, Traceability]
- [ ] CHK043 Les tâches critiques (SPU bring-up, IRQ semantics, CDROM banking/IRQ) ont-elles des checkpoints observables définis ? [Tasks §Phase 2/US5, Measurability]
- [ ] CHK044 Les conflits repérés (`plan.md` vs constitution, “stub” wording) sont-ils explicitement résolus avant implémentation ? [Constitution, Plan, Spec, Conflict]

## Notes

- Check items off as completed: `[x]`
- Ce checklist ne teste pas le code: il teste si les exigences sont assez bonnes pour être implémentées sans surprises.
