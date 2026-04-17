# Mesh3D Cache Prerequisites (2026-04-17)

## But

Fixer clairement ce qu'il faut observer et stocker avant de construire un vrai
cache `mesh3d` oriente UE5.

Le point important est:

- oui, le cache `mesh3d` sera necessaire
- non, il ne faut pas le coder "a l'aveugle"

Avant de figer un format de cache objet/mesh, il faut d'abord savoir:

- comment identifier un mesh logique
- comment le distinguer d'un simple flux de triangles frame-based
- comment separer geometrie, transform, hierarchie et effets temporaires

Ce document sert de pont entre:

- `docs/GTE_GPU_MODE_DISCOVERY_METHOD_2026-03-08.md`
- `docs/OBJECT_SPACE_MESH_CACHE_STRATEGY_2026-03-07.md`
- `docs/MCP_DISPLAY_ANALYSIS_TOOLS_PLAN.md`

---

## Positionnement

Le cache `mesh3d` vise un niveau plus haut que:

- `DrawCmd3D`
- `FrameDrawList`
- reconstruction "par triangle"

Il vise un niveau "objet logique / mesh logique", exploitable ensuite par UE5.

Le pipeline cible devient:

- GTE / CPU / RAM / DMA2 / GPU
- reconstruction triangles / quads
- regroupement en objets logiques
- separation geometrie vs transform
- cache `mesh3d`
- adaptation UE5

Important:

- toutes ces etapes ne doivent pas etre resolues uniquement par l'emulator MCP
- certaines seront plus vite clarifiees par Ghidra
- le LLM doit rester libre d'utiliser:
  - l'emulator MCP pour observer
  - Ghidra pour expliquer la structure
  - puis le runtime pour revalider

Contrainte d'implementation a garder:

- les phases d'analyse MCP se feront principalement avec l'emulateur CLI
- le backend d'analyse de reference est donc le CLI MCP, pas UE5
- UE5 reste important, mais surtout pour la visualisation, la validation rendu
  et plus tard l'exploitation du cache `mesh3d`

Le principe utile est:

- runtime pour trouver
- Ghidra pour comprendre
- runtime pour confirmer

---

## Ce qu'on doit savoir avant de coder le cache

### 1. Identite stable d'un mesh logique

Question:

- comment reconnaitre que deux groupes de triangles, sur deux frames
  differentes, representent "le meme objet" ?

Informations necessaires:

- groupes de `face_idx` qui reapparaissent ensemble
- recurrence du meme `source_pc` GTE / packet builder
- signature stable de topologie
- signature stable de matiere / UV / flags GPU

Sans ca:

- le cache fusionnera des objets differents
- ou fragmentera un meme objet en trop de morceaux

### 2. Separation geometrie vs transform

Question:

- est-ce que le changement observe d'une frame a l'autre vient d'une nouvelle
  geometrie, ou seulement d'une nouvelle transform ?

Informations necessaires:

- racines de matrices candidates
- stabilite des `MTC2` observes
- relation entre groupes de polys et transform root
- evolution temporelle des vertices / bbox / OT depth

Sans ca:

- le cache figera des meshes qui devraient rester dynamiques

### 3. Groupes de polys lies

Question:

- quelles primitives appartiennent au meme objet logique ?

Informations necessaires:

- co-occurrence temporelle des `face_idx`
- relation `quad_half` / GT4 / paires d'edges / strips
- coherence `ot_z`
- coherence texture / CLUT / texpage / source builder

Sans ca:

- on ne sait pas assembler le bon mesh

### 4. Hierarchie et structure

Question:

- plusieurs groupes de polys partagent-ils une meme racine, ou une relation
  parent/enfant ?

Informations necessaires:

- familles de racines de matrices
- offsets / strides RAM recurrents
- sous-groupes qui bougent ensemble avec deltas stables
- indices d'objets composes, bones, limbs, sous-meshes

Sans ca:

- on aura peut-etre un cache mesh, mais pas un cache scene/object-space utile

Cette partie est typiquement une zone ou Ghidra peut etre plus efficace que le
runtime seul, surtout si:

- les bones sont dans une table explicite
- les parents/enfants passent par des structs ou listes chainees
- les matrices sont stockees via des offsets fixes
- l'objet logique est reconstruit a partir d'une structure C claire

---

## Infos minimales a collecter

Le futur cache `mesh3d` depend au minimum de ces categories de donnees.

### A. Producteurs runtime

- top PCs GTE structurants
- top writers DMA2/GPU
- top writers OT
- mode de submit (DMA2 linked-list / GP0 direct)

### B. Infos de liaison triangle -> objet

- `face_idx`
- `face_idx_secondary` ou equivalent
- `quad_half`
- `ot_z`
- mode de reconstruction actif

### C. Infos de transform

- `camera_candidates` / `transform root candidates`
- masque de registres GTE touches
- `last_pc`
- recurrence par frame
- region memoire (RAM / scratch)
- layout memoire confirme si necessaire dans Ghidra

### D. Infos temporelles

- frame de premiere apparition
- frame de derniere apparition
- nombre de frames observees
- taux de stabilite
- frequence d'invalidation

### E. Infos material / packet

- textured / flat
- semi-transparent
- raw texture
- CLUT / texpage / profondeur texture
- source builder ou famille de packets

### F. Infos structurelles confirmees

Quand le runtime devient ambigu, il faut pouvoir confirmer dans Ghidra:

- structure exacte des matrices
- stride des objets / sous-objets
- presence d'une table de bones
- liens parent/enfant
- nature exacte d'une liste de polys ou d'un packet builder

---

## Etat actuel du code

On n'a pas encore le cache `mesh3d`, mais on a deja une partie des signaux
necessaires.

### Deja presents

- `GTE trace`
- `DMA2 nohint summary`
- `provenance hotspots`
- `DrawCmd3D`
- `source_pc`
- `face_idx`
- `quad_half`
- `ot_z`
- `camera_candidates`
- profile / mode discovery

### Nouveaux outils MCP deja ajoutes

- `emu.get_gte_trace_summary`
- `emu.get_dma2_nohint_summary`
- `emu.get_draw_list_summary`
- `emu.get_camera_candidates`
- `emu.get_linked_poly_groups`
- `emu.get_transform_roots`
- `emu.get_group_transform_links`
- `emu.get_mesh_cache_candidates`

Ces outils ne construisent pas encore le cache.
Ils servent a accumuler les observables dont le cache aura besoin.

Etat plus recent:

- `DrawCmd3D` porte maintenant `source_pc` quand la provenance est connue
- les groupes et candidats exposes au MCP peuvent donc etre relies plus facilement
  a Ghidra
- le regroupement temporel est meilleur quand un meme `face_idx` est reutilise par
  plusieurs producteurs differents
- ce n'est pas encore une cle objet finale, mais c'est un meilleur precurseur pour
  le futur cache `mesh3d`

Et ils ne dispensent pas d'utiliser Ghidra quand:

- la hierarchie ne ressort pas clairement du runtime
- une racine de transform doit etre interpretee
- une table d'objets ou de bones doit etre validee

---

## Ce qui manque encore avant le cache UE5

### 1. Groupes de faces stables

Il faut un niveau au-dessus du triangle:

- quels `face_idx` reviennent ensemble
- quels groupes restent coherents sur plusieurs frames
- quels groupes ne changent que par transform

### 2. Correlation groupe -> transform root

Il faut savoir:

- quel groupe de polys depend de quelle racine de matrice
- avec quelle confiance

### 3. Detection d'objets composes

Il faut savoir:

- si un groupe est un objet autonome
- ou un sous-ensemble d'un objet plus grand
- ou un sous-ensemble de type bone/limb

### 4. Politique de promotion / invalidation

Il faut des regles explicites:

- quand un groupe devient candidat au cache
- quand il est promu
- quand il est invalide

---

## Plan recommande avant le cache

### Phase 1 - Consolider l'observation

Utiliser et etendre les outils MCP pour obtenir:

- hotspots GTE fiables
- hotspots DMA2/OT fiables
- candidats matrices fiables
- resume draw list par scene utile
- premiers candidats de promotion pour cache

### Phase 2 - Ajouter des outils "groupes logiques"

Prochains outils recommandes:

- `emu.get_linked_poly_groups`
- `emu.get_transform_roots`
- `emu.get_group_transform_links`
- `emu.get_mesh_cache_candidates`

Objectif:

- sortir du niveau triangle pur
- commencer a raisonner au niveau "objet probable"
- mesurer recurrence et stabilite

### Phase 2b - Confirmation structurelle dans Ghidra

Quand une zone commence a ressembler a:

- objet stable
- hierarchie
- bones
- liste d'instances

alors il faut ouvrir Ghidra de facon ciblee pour confirmer:

- layout memoire
- parent/enfant
- tables et strides
- helpers de construction

### Phase 3 - Valider quelques cas reels

Avant toute implementation du cache:

- Ridge Racer
- un cas plus 2D/HUD mixte
- un cas avec hierarchie / bones si possible

Objectif:

- verifier que les cles de regroupement sont bonnes
- verifier que la separation geometrie/transform tient

### Phase 4 - Concevoir le cache `mesh3d`

Seulement apres validation:

- definir la cle de cache
- definir le payload logique PS1
- definir l'adaptation UE5
- definir les dirty flags et invalidations

Cette conception doit se baser sur:

- l'observation runtime
- plus la structure memoire confirmee dans Ghidra

---

## Decision pratique

Le cache `mesh3d` n'est pas remis en question.

Il reste:

- necessaire
- architecturalement important
- probablement incontournable pour UE5 / VR

Mais la priorite immediate n'est pas encore le cache lui-meme.

La priorite immediate est:

- collecter les bons signaux
- valider les bons regroupements
- comprendre les racines de transform et les groupes de polys

Le bon enchainement est donc:

- observation
- regroupement
- validation
- cache

et non:

- cache d'abord
- interpretation apres
