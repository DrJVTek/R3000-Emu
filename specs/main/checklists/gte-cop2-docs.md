# Documentation / Requirements Quality Checklist: GTE (COP2) & CPU↔GTE Contract

**Purpose**: “Unit tests for English” — valider que les exigences/docs autour du GTE (COP2) et de son contrat avec le CPU sont complètes, claires, cohérentes et mesurables (sans tester l’implémentation).
**Created**: 2026-01-24
**Feature**: [spec.md](../spec.md) + [plan.md](../plan.md) + [README.md](../../../README.md)

**Assumptions (defaults)**:
- Audience: reviewer/lecteur du repo (live + PR review)
- Depth: standard
- Scope: documentation du “contrat” COP2/GTE, et ce que la feature promet (pas le détail micro-optimisé)

## Requirement Completeness

- [ ] CHK001 Le document décrit-il clairement ce que couvre “GTE support” (commandes CO, transferts MFC2/MTC2/CFC2/CTC2, LWC2/SWC2) ? [Gap, Spec §FR-006]
- [ ] CHK002 La liste des commandes GTE “supportées” est-elle explicitement donnée (noms + IDs/opcodes), ou bien une référence précise est-elle fournie ? [Gap, Spec §US3]
- [ ] CHK003 La doc décrit-elle le modèle de registres GTE côté “data” et “control” (noms, index, packing) à un niveau utilisable ? [Gap]
- [ ] CHK004 Les préconditions d’exécution sont-elles définies (endianness, alignement, conventions d’adresse PS1) pour les accès LWC2/SWC2 ? [Gap, Spec §US3]
- [ ] CHK005 Les limites/“non-goals” GTE sont-elles listées (ex: timing cycle-accurate, flags exacts, saturations perfect) ? [Gap, Plan §Scale/Scope]
- [ ] CHK006 Le contrat CPU↔GTE est-il explicite: qui décode quoi, où se fait la délégation, et quel est le comportement en cas de commande inconnue ? [Gap, Spec §FR-006]

## Requirement Clarity (unambiguous wording)

- [ ] CHK007 Le terme “module séparé du CPU” est-il défini (frontière API, ownership des registres, responsabilités) ? [Clarity, Spec §FR-006; Plan §Constraints]
- [ ] CHK008 Les docs définissent-elles clairement ce que “exécuter une commande GTE” veut dire (entrées/sorties observables) ? [Ambiguity, Spec §US3]
- [ ] CHK009 Les conventions des bits de contrôle (sf/lm/mx/v/tx) sont-elles décrites (même si simplifiées), ou explicitement exclues ? [Gap]
- [ ] CHK010 La doc précise-t-elle comment interpréter “valeurs attendues” pour un exemple GTE (tolérances, saturation/clamp) ? [Gap, Spec §SC-003]

## Requirement Consistency (terminology & promises)

- [ ] CHK011 “GTE séparé” est-il cohérent entre README / spec / plan (mêmes mots, même promesse) ? [Consistency, Spec §FR-006; Doc: README.md §Structure; Plan §Constraints]
- [ ] CHK012 Les docs ne promettent-elles pas implicitement une exactitude PS1 totale tout en déclarant des simplifications ? [Conflict, Plan §Performance Goals; Plan §Scale/Scope]
- [ ] CHK013 Les termes COP2 / GTE / “commande CO” sont-ils utilisés de façon stable et expliqués une fois ? [Consistency, Gap]

## Acceptance Criteria Quality (measurable)

- [ ] CHK014 Le spec définit-il un test/critère minimal “GTE vivant” (ex: un guest qui fait transferts + 1 commande + vérifie un registre) ? [Gap, Spec §US3]
- [ ] CHK015 Les critères d’échec sont-ils définis (ex: commande non reconnue → exception RI vs no-op + log) ? [Gap, Spec §Edge Cases]
- [ ] CHK016 Les docs définissent-elles un exemple reproductible (petit programme guest) et ce qui est observé (trace ou print) ? [Gap, Spec §SC-001]

## Scenario Coverage (flows)

- [ ] CHK017 Le “happy path” GTE est-il décrit (setup registres → commande → lecture résultats) ? [Gap]
- [ ] CHK018 Les scénarios alternatifs sont-ils listés (différentes commandes, ou différentes sources: MTC2 vs LWC2) ? [Gap]
- [ ] CHK019 Les scénarios d’erreur sont-ils couverts (GTE reg index hors plage, opcode inconnu, valeurs out-of-range) ? [Gap]

## Edge Case Coverage (boundary conditions)

- [ ] CHK020 Les exigences sur “load delay” pour MFC2 (résultat disponible quand ?) sont-elles explicites, ou explicitement hors scope ? [Gap, Spec §FR-004]
- [ ] CHK021 Les exigences sur “branch delay slot” quand une instruction COP2 est dans le delay slot sont-elles explicitement traitées ou exclues ? [Gap, Spec §FR-004]
- [ ] CHK022 Les exigences sur les flags (FLAG register) sont-elles décrites (même partiellement), ou explicitement exclues ? [Gap]
- [ ] CHK023 Les exigences sur la saturation/clamp des IR/MAC/SZ/SXY sont-elles spécifiées (au moins les règles générales) ? [Gap]

## Non-Functional Requirements (NFR)

- [ ] CHK024 La doc précise-t-elle le niveau de fidélité visé pour le GTE (pédagogie vs accuracy), avec une règle de décision ? [Gap, Plan §Performance Goals]
- [ ] CHK025 Les exigences de lisibilité/commentaires pour le GTE sont-elles explicites (style, explications, références) ? [Gap, Plan §Constraints]
- [ ] CHK026 Les exigences de logs observables (préfixes, stderr vs stdout) couvrent-elles aussi le chemin GTE/COP2 ? [Gap, Spec §FR-005]

## Dependencies & Assumptions

- [ ] CHK027 Les dépendances implicites GTE (ex: matrice/éclairage, fixed-point conventions) sont-elles documentées comme hypothèses ? [Assumption, Gap]
- [ ] CHK028 La doc précise-t-elle si l’exactitude dépend d’un BIOS/PSX SDK, ou si tout est standalone ? [Gap, Plan §Scale/Scope]

## Ambiguities & Conflicts (things to clarify)

- [ ] CHK029 Le spec dit-il explicitement “qu’est-ce qui est PS1-spec” vs “choix didactique” pour le GTE ? [Gap]
- [ ] CHK030 Le spec définit-il ce que “supporter le GTE” veut dire pour la démo live (minimum) vs pour une compatibilité plus large (stretch) ? [Gap]

## Notes

- Cochez avec `[x]` quand une exigence est clarifiée/ajoutée aux docs.
- Les items `[Gap]` sont des points manquants dans les docs/exigences (à écrire).

