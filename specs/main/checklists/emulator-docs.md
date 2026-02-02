# Documentation / Requirements Quality Checklist: R3000-Emu (PS1 Emulator)

**Purpose**: “Unit tests for English” — vérifier que les exigences et la documentation du projet sont complètes, claires, cohérentes et mesurables (sans tester l’implémentation).
**Created**: 2026-01-24
**Feature**: [README.md](../../../README.md) + `_bmad-output/planning-artifacts/bmm-workflow-status.yaml`

**Note**: Checklist générée pour améliorer la qualité des docs/exigences (pas pour valider que l’émulateur “marche”).

## Requirement Completeness

- [ ] CHK001 Est-ce que l’objectif du projet (émulateur PS1 éducatif, CLI) est explicitement défini ? [Doc: README.md §Intro]
- [ ] CHK002 Est-ce que le périmètre fonctionnel “CPU R3000” est listé (instruction set visé, exceptions, effets pipeline) ? [Gap]
- [ ] CHK003 Est-ce que le périmètre “GTE / COP2” est listé (commandes, transferts, LWC2/SWC2) ? [Gap]
- [ ] CHK004 Est-ce que les formats d’entrée supportés (ELF32 MIPS LE, PS-X EXE) sont explicitement listés avec contraintes/limitations ? [Doc: README.md §Run; Doc: README.md §Structure]
- [ ] CHK005 Est-ce que la stratégie de logs/trace est décrite comme une exigence (niveaux, catégories, destinations stdout/stderr) ? [Doc: README.md §Run]
- [ ] CHK006 Est-ce que la manière de “printf côté guest” est expliquée (MMIO vs SYSCALL, conventions d’appel, limites) ? [Doc: README.md §Toolchain; Gap]
- [ ] CHK007 Est-ce que la structure du repo est documentée avec responsabilités par dossier (CPU, GTE, loader, logger) ? [Doc: README.md §Structure]
- [ ] CHK008 Est-ce que les prérequis build (CMake, compilateur, OS) sont documentés (versions minimales) ? [Gap]
- [ ] CHK009 Est-ce qu’il existe une section “Known limitations / non-goals” (ex: pas de BIOS, pas de GPU/SPU, pas de timing exact) ? [Gap]
- [ ] CHK010 Est-ce qu’il existe une section “Glossaire” (KSEG0/KSEG1, delay slot, load delay, COP0, COP2/GTE) ? [Gap]

## Requirement Clarity (unambiguous wording)

- [ ] CHK011 Les termes “pédagogie live”, “trace lisible”, “work in progress” sont-ils définis avec critères concrets ? [Ambiguity, Doc: README.md §Intro; Doc: README.md §Structure]
- [ ] CHK012 La “trace 1 ligne par instruction” est-elle définie (format attendu, champs obligatoires/optionnels) ? [Gap]
- [ ] CHK013 Les options CLI `--log-level` et `--log-cats` ont-elles une sémantique définie (valeurs, défauts, interactions) ? [Doc: README.md §Run; Gap]
- [ ] CHK014 Le paramètre `--format=auto|elf|psxexe` a-t-il des règles d’auto-détection documentées ? [Gap]
- [ ] CHK015 La notion de “MIPS I / R3000” est-elle cadrée (endianness, alignement, instructions non supportées) ? [Gap]

## Requirement Consistency (terminology & promises)

- [ ] CHK016 Les docs sont-elles cohérentes sur l’état du GTE (fini vs WIP) et ce que “supporté” veut dire ? [Conflict, Doc: README.md §Structure; Doc: _bmad-output/planning-artifacts/bmm-workflow-status.yaml]
- [ ] CHK017 Les docs sont-elles cohérentes sur le périmètre cible (PS1 vs “bare metal MIPS”) ? [Ambiguity, Doc: README.md §Toolchain]
- [ ] CHK018 Les termes “PS-X EXE / PSX EXE” sont-ils utilisés de façon stable et expliqués une fois ? [Doc: README.md §Run; Gap]
- [ ] CHK019 Le rôle de BMAD est-il clairement distingué du code produit (outillage vs livrable) ? [Doc: README.md §BMAD]

## Acceptance Criteria Quality (measurable)

- [ ] CHK020 Les exigences de compatibilité (quels binaires doivent tourner) ont-elles des critères mesurables (ex: exemple “hello” + attentes de sortie) ? [Doc: README.md §Toolchain; Gap]
- [ ] CHK021 La réussite d’un chargement ELF/EXE est-elle définie (PC/GP/SP init, zones mémoire, erreurs) avec critères observables ? [Gap]
- [ ] CHK022 Les exigences de logs sont-elles mesurables (ex: “toutes les exceptions impriment Cause/EPC/BadVAddr”) ? [Gap]
- [ ] CHK023 Les exigences “GTE” sont-elles testables via critères de sortie (valeurs attendues, tolérances, saturation) ? [Gap]

## Scenario Coverage (flows)

- [ ] CHK024 Le “happy path” est-il écrit de bout en bout (build → compile guest → run emulator → observer output) ? [Doc: README.md §Build; Doc: README.md §Toolchain]
- [ ] CHK025 Les scénarios alternatifs sont-ils couverts (utiliser MSYS2 vs toolchain ZIP, build Debug vs Release) ? [Doc: README.md §Toolchain; Gap]
- [ ] CHK026 Les scénarios d’échec sont-ils décrits (fichier introuvable, format inconnu, segment hors RAM, instruction illégale) ? [Gap]
- [ ] CHK027 Les exigences sur le comportement en cas d’exception CPU sont-elles définies (vecteur, EPC, reprise/arrêt) ? [Gap]

## Edge Case Coverage (boundary conditions)

- [ ] CHK028 Les exigences d’alignement mémoire sont-elles explicites (lecture/écriture non alignée : exception vs comportement) ? [Gap]
- [ ] CHK029 Les exigences sur les adresses virtuelles PS1 (KSEG0/KSEG1) sont-elles documentées (mapping, limites) ? [Gap]
- [ ] CHK030 Les exigences sur les “delay slots” (branch delay, load delay) sont-elles explicites et illustrées ? [Gap]
- [ ] CHK031 Les exigences sur les overflows (ADD/ADDI) sont-elles explicitement définies (exception vs wrap) ? [Gap]
- [ ] CHK032 Les exigences sur les registres COP0 minimaux (Cause/EPC/BadVAddr/Status) sont-elles listées ? [Gap]

## Non-Functional Requirements (NFR)

- [ ] CHK033 Les exigences de performance sont-elles précisées (ex: “X MIPS sur machine Y”, ou “mode trace ralentit”) ? [Gap]
- [ ] CHK034 Les exigences de portabilité (MSVC/clang, Windows/Linux) sont-elles explicites ? [Gap]
- [ ] CHK035 Les exigences de lisibilité (style Allman, commentaires didactiques) sont-elles formalisées comme règles ? [Gap]
- [ ] CHK036 Les exigences d’observabilité (préfixes logs, stderr vs stdout, niveaux) sont-elles explicites ? [Doc: README.md §Run; Gap]

## Dependencies & Assumptions

- [ ] CHK037 Les dépendances externes (CMake, toolchain `mipsel-none-elf-gcc`, MSYS2) sont-elles listées avec instructions reproductibles ? [Doc: README.md §Build; Doc: README.md §Toolchain]
- [ ] CHK038 Les hypothèses de mémoire (taille RAM émulée, zones réservées) sont-elles documentées ? [Gap]
- [ ] CHK039 Les hypothèses sur l’absence de BIOS/hardware PS1 (GPU/SPU/CDROM) sont-elles explicites ? [Gap]

## Ambiguities & Conflicts (things to clarify)

- [ ] CHK040 Le doc précise-t-il clairement “ce qui est PS1-spec” vs “choix didactique / simplification” ? [Gap]
- [ ] CHK041 Les docs disent-elles explicitement si l’objectif est exactitude architecturale ou “assez bon pour la démo” (et comment trancher) ? [Ambiguity, Doc: _bmad-output/planning-artifacts/bmm-workflow-status.yaml]

## Notes

- Cochez avec `[x]` quand une exigence est clarifiée/ajoutée aux docs.
- Les items marqués `[Gap]` indiquent une exigence/documentation manquante (à écrire).

