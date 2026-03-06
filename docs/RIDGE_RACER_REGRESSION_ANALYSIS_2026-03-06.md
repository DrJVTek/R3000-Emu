# Ridge Racer - Analyse de Regression (GPU3D token flow)

Date: 2026-03-06
Branche: cortex-vr-worldmesh

## Contexte

Au debut de la procedure, le drapeau/menu etait visuellement correct (cas 2D), alors que le jeu complet 3D avait deja des trous.
Les iterations suivantes ont ajoute beaucoup de logique d'analyse et de cache (CPU/BUS/CORE), mais le probleme central Ridge Racer persiste: des polygons restent sans hint token.

## Symptomes observes (logs)

1. Frames menu/UI:
- `3d=0`, `2d=xxx` (normal pour 2D)
- pas d'erreur critique de token.

2. Frames course (zone critique):
- `tok_hint=313`
- `tok_miss=248`
- `miss_no_hint=248`
- `miss_decode_fail=248`
- valeurs stables frame apres frame.

3. Top PCs DMA2 nohint stables:
- `0x80026544`
- `0x80026530`
- `0x8002651C`
- `0x8002650C`
- `0x80025EA8`
- `0x80025E90`

Conclusion: l'echec est structurel et localise sur des routines CPU precises de construction OT/pack de commandes GPU.

## Ce qui a ete ajoute (resume)

- Pipeline token flow COP2->RAM->DMA2->Gpu3D.
- Analyse runtime (mode game/analysis), hotspots DMA2, profil persistant `.psx3dprof`.
- Tracker camera RAM.
- Extensions de propagation token CPU (rules + store inference).

## Regression probable (drapeau)

Point de vigilance principal:
- `Gpu3D` ignore actuellement `draw_env.offset_x/y` sur le path 2D dans `make_vertex()`.
- Ce choix a ete introduit pour supprimer un flicker lie au double-buffer, mais il peut casser des cas 2D qui dependaient du mapping offset.

Impact possible:
- decalage/format visuel du drapeau/menu par rapport a l'etat "100%" initial.

## Pourquoi ca ne marche toujours pas en course

Les misses a 248 ne viennent pas d'un manque de gestion de mode/caches.
Elles viennent du fait qu'on ne reconstruit pas encore la relation deterministic entre:
- les routines CPU de pack (`0x800265xx`, `0x80025Exx`)
- et les vertices/faces GTE sources.

Le token "generique" est insuffisant pour ces routines car:
- beaucoup de transformations bitwise/packing intermediaires,
- ecritures partielles/merge,
- structures OT indirectes.

## Revue de code (regression structurelle)

### Finding 1 - Le mode "analysis refresh" n'apprend pas de nouvelle logique (critique)

- Fichier: `src/emu/core.cpp` (`run_psx3d_analysis_refresh`, autour de `added_pcs`).
- Comportement actuel: un refresh ajoute seulement des PCs dans `psx3d_analyzed_pcs_` et fait `ack_refresh`.
- Effet: aucune nouvelle regle CPU/GPU n'est derivee ni appliquee; on marque "analyzed" sans enrichir la resolution des tokens.
- Symptom log associe: `analysis pass done ... added_pcs=0 total_analyzed=110` en boucle, avec `miss_no_hint=248`.

### Finding 2 - Le fallback monitor se coupe quand tous les hotspots sont deja connus (critique)

- Fichier: `src/emu/core.cpp` (bloc `fallback monitor`, condition `has_unknown_nohint_pc`).
- Comportement actuel: si les PCs top nohint sont deja dans `ANALYZED_PC`, aucun refresh fallback n'est re-declenche, meme si le ratio 2D reste mauvais.
- Effet: etat de "stagnation" permanent: on reste en echec mais le systeme se considere "fini".

### Finding 3 - Le profil persiste un etat "analyzed" qui masque les regressions (majeur)

- Fichier: `src/emu/core.cpp` (`try_load_psx3d_profile` + `run_psx3d_analysis_refresh`).
- Comportement actuel: au chargement, les PCs restent tags "analyzed" indefiniment; les nouvelles executions sur les memes PCs ne relancent pas d'analyse utile.
- Effet: impossible d'apprendre des variantes de call-path pour le meme PC (cas Ridge Racer OT pack).

## Ce qu'on a rate

1. Analyse specifique par PC critique (call-aware + memflow local) absente.
2. Pas encore de "regle compilee" par routine CPU qui mappe explicitement `store_word -> source vertex/face`.
3. Trop d'effort sur l'orchestration (refresh/profile) avant de fermer le cas deterministic des 6 PCs dominants.

## Strategie pour faire marcher (sans fallback rendu)

### Etape A - Instrumentation focalisee (must)

Pour chacun des PCs critiques (`0x80026544/30/1C/0C`, `0x80025EA8/90`), tracer pendant N frames:
- registre source du store,
- adresse cible RAM/OT,
- token present/absent,
- derniere origine memoire du registre (paddr + pc writer),
- contexte d'appel (hash deja present).

Objectif: obtenir un graphe deterministic local, pas statistique.

### Etape B - Regles par PC (compiled rules)

Generer une regle par PC critique:
- pattern d'instruction + offset + contexte d'appel,
- source token prioritaire,
- validation stricte (sinon no-op).

Ces regles doivent s'appliquer avant DMA2 et ecrire un token fiable dans la RAM OT.

### Etape C - Validation de convergence

Definition "ca marche":
- `miss_no_hint` et `miss_decode_fail` diminuent nettement (< 248 stable),
- `tok_miss` baisse,
- `3d` augmente sur frames 540+,
- pas de degradation du menu/drapeau.

### Etape D - Correctif architecture court terme (sans casser UE5)

1. Garder le hook system actuel (vblank + step).
2. Remplacer le statut binaire `ANALYZED_PC` par un etat versionne:
   - `last_analyzed_vblank`,
   - `analysis_level`,
   - `last_result_quality` (miss rate apres analyse).
3. Autoriser une reanalyse des memes PCs si la qualite reste mauvaise (`miss_no_hint/decode_fail` eleves).
4. En mode game, n'activer le surcout uniquement sur PCs critiques (top nohint + call_ctx), pas globalement.

## Decision immediate

- Garder le systeme d'analyse/caches en place (utile).
- Arreter les heuristiques generiques supplementaires tant que les 6 PCs critiques ne sont pas resolus par regles deterministes.
- Prioriser la fermeture du cas Ridge Racer sur ces PCs, puis generaliser la methode.

## Note

Cette analyse est volontairement orientee "post-mortem + plan d'action".
Elle doit servir de reference pour toute suite de dev (Claude/Codex) afin d'eviter d'ajouter de la complexite sans gain sur les misses critiques.
