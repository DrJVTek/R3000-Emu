# MCP Display Analysis Tools Plan

## But

Definir un plan d'outils MCP centres sur la detection et l'analyse des chemins
d'affichage PS1, avant le reverse detaille du code.

Le point de depart n'est pas:

- "trouver `addPrim()`"
- "trouver `RCpolyGT4`"
- "retrouver les wrappers SDK"

Car dans beaucoup de jeux, surtout avec moteurs custom studio, ces couches sont:

- inlinees
- macro-isees
- remplacees par des helpers MIPS optimises
- ou completement reformulees autour d'un packet builder maison

Le vrai point de depart doit etre le pipeline runtime observable:

- GTE
- CPU writes RAM
- OT fill
- DMA2 / GP0 submit
- GPU packet stream

Ce document propose:

1. les outils MCP a prevoir
2. les outils les plus utiles a exposer directement au LLM
3. l'ordre d'implementation recommande
4. des exemples d'entrees/sorties JSON

---

## Principe directeur

Le LLM ne doit pas recevoir uniquement des outils bruts de reverse.

Il doit recevoir en priorite des observables "metier affichage", par exemple:

- quels PCs produisent les mots consommes par DMA2/GPU
- quels PCs executent les ops GTE structurantes
- quels PCs remplissent l'OT
- quels types de packets GPU sont effectivement soumis
- quelle chaine relie GTE -> packet builder -> OT -> submit

Autrement dit:

- Ghidra MCP sert a confirmer
- l'emulator MCP sert a decouvrir
- le LLM doit raisonner sur des resumes causaux, pas sur du bruit brut

Important:

- le LLM ne doit pas etre enferme dans un seul outillage
- il doit choisir entre emulator MCP et Ghidra selon la question
- certains problemes sont resolus plus vite par le runtime
- d'autres sont resolus plus vite par l'analyse statique

Le bon schema est:

- emulator MCP pour localiser
- Ghidra pour interpreter
- puis retour eventuel a l'emulator pour valider

Important aussi pour l'architecture:

- les phases d'analyse MCP doivent se faire principalement avec l'emulateur CLI
- le chemin de reference pour ces analyses est `r3000_emu.exe --mcp-stdio`
- les nouveaux tools doivent etre penses et verifies d'abord cote CLI MCP
- l'integration UE5 sert surtout ensuite a visualiser, valider le rendu, et
  exploiter plus tard le futur cache `mesh3d`

Important aussi pour la strategie d'orchestration:

- il ne faut pas raisonner en "one shot"
- l'orchestrateur doit faire plusieurs passes si le signal est faible
- il doit accumuler les preuves hors contexte LLM
- il doit partir du runtime pour localiser
- puis utiliser Ghidra pour comprendre
- puis revenir au runtime pour revalider si necessaire

Autrement dit, le bon schema n'est pas:

- une seule observation
- une seule ouverture Ghidra
- une seule classification

Le bon schema est:

1. reconnaissance runtime
2. ancrage statique Ghidra
3. exploration de branches
4. hypotheses de relation
5. reobservation runtime
6. synthese finale

---

## Regles De Decision LLM

### Utiliser d'abord l'emulator MCP quand la question est:

- "qu'est-ce qui est actif maintenant ?"
- "quel PC est vraiment chaud dans cette scene ?"
- "quel groupe de polys revient ?"
- "quelle racine de transform semble vivante ?"
- "est-ce stable sur plusieurs frames ?"
- "est-ce plutot 2D, 3D, mixte, submit-only ?"

### Utiliser d'abord Ghidra quand la question est:

- "quelle est la structure memoire exacte ?"
- "cette racine RAM pointe vers quoi ?"
- "est-ce une hierarchie, une table, une liste chainee ?"
- "ou sont les parents, enfants, bones, sous-objets ?"
- "ce helper OT ou packet builder custom fait quoi precisement ?"

### Utiliser les deux quand la question est:

- "comment construire une cle d'objet stable ?"
- "comment lier un groupe de polys a une matrice ?"
- "comment identifier une hierarchie exploitable pour un cache UE5 ?"
- "quel mode explicite faut-il creer et comment le valider ?"

Dans ces cas:

1. l'emulator MCP reduit l'espace de recherche
2. Ghidra confirme la structure
3. l'emulator MCP revalide la conclusion en runtime

---

## Objectifs fonctionnels

Le systeme d'outils doit permettre de repondre rapidement a ces questions:

1. Y a-t-il de la 3D active sur cette fenetre runtime ?
2. Quels PCs GTE sont reels et structurants ?
3. Quels PCs ecrivent les packets finalement soumis au GPU ?
4. Quels PCs remplissent l'OT ?
5. L'OT est-elle remplie via helper SDK, macro custom, ou linked-list builder direct ?
6. Le flux courant est-il surtout:
   - 2D pure
   - 3D pure
   - mix 2D/3D
   - submit OT sans production locale visible
7. Quel "mode" explicite peut etre derive de ces preuves ?

---

## Niveaux d'outils

### Niveau 1 - Outils bas niveau

Ce sont les briques minimales.
Elles sont indispensables, mais insuffisantes seules pour piloter un LLM.

#### Emulator runtime/control

- `pause_emulation()`
- `resume_emulation()`
- `reset_emulation()`
- `get_cpu_state()`
- `get_frame_state()`
- `set_breakpoint(address)`
- `clear_breakpoint(address)`
- `run_until_pc(address, timeout_ms)`
- `sample_pc(window_ms, max_samples)`
- `read_memory(address, size)`
- `dump_memory(address, size, path)`

#### Ghidra / statique

- `decompile_function(address)`
- `disassemble_function(address)`
- `get_function_by_address(address)`
- `get_xrefs_to(address)`
- `get_xrefs_from(address)`
- `rename_function(address, name)`
- `set_comment(address, text)`

### Niveau 2 - Outils metier affichage

Ce sont les outils les plus rentables pour l'analyse PS1.
Ils doivent etre consideres comme la surface MCP prioritaire pour le LLM.

#### A. Detection GTE

Objectif:

- trouver les zones 3D reelles
- eviter de lire du code hors sujet

Outils recommandes:

- `summarize_gte_activity(frame_range, opcode_filter?, top_n?)`
- `sample_gte_ops(window_ms, opcode_filter?, pc_range?)`

Signaux a remonter:

- top PCs par opcode
- top opcodes
- densite par frame
- bursts GTE utiles

Opcodes structurants par defaut:

- `RTPT`
- `RTPS`
- `NCLIP`
- `NCS`
- `NCT`
- `AVSZ3`
- `AVSZ4`

#### B. Detection packet builder GPU

Objectif:

- trouver les vrais producteurs de packets
- distinguer builder de primitive et simple submit tardif

Outils recommandes:

- `find_dma2_gpu_producers(frame_range, top_n?, include_partial_stores?)`
- `trace_gpu_packet_writers(address_range?, frame_range?)`

Signaux a remonter:

- top writer PCs
- nombre de mots ecrits puis consommes
- densite d'ecriture sequentielle
- correlation avec DMA2 linked-list GPU

#### C. Detection OT fill

Objectif:

- retrouver les insertions OT meme si elles ne passent pas par `addPrim()`
- couvrir les variantes custom type `SWL/SWR`

Outils recommandes:

- `find_ot_writers(frame_range, ot_base?, ot_size?, include_partial_stores?)`
- `trace_ot_chain(ot_base, bucket_index, max_links?)`
- `detect_ot_insert_pattern(pc_or_function)`

Signaux a remonter:

- top PCs ecrivant dans l'OT
- type de store (`SW`, `SWL`, `SWR`, `LWL`, `LWR`)
- pattern "old head -> prim.tag" puis "new prim -> ot.head"
- longueur moyenne de chaine

#### D. Detection packets GPU

Objectif:

- classer les packets effectivement soumis
- separer plus vite 2D, 3D, setup, blit, sprites

Outils recommandes:

- `summarize_gpu_packets(frame_range, include_examples?)`
- `trace_gpu_submit(frame_range, source='dma2|gp0|all')`

Signaux a remonter:

- tri / quad / rect / sprite / line
- textured / flat / gouraud / raw
- setup commands GP0 / GP1
- OT buckets actifs
- linked-list DMA vs GP0 direct

#### E. Correlation causale

Objectif:

- relier GTE, packet builder, OT fill, submit

Outils recommandes:

- `correlate_gte_to_gpu(frame_range, confidence_threshold?)`
- `find_draw_pipeline(frame_range, scene_hint?)`

Sortie attendue:

- zone GTE
- zone packet builder
- zone OT insertion
- zone submit
- score de confiance

### Niveau 3 - Outils haut niveau pour le LLM

Ces outils encapsulent la logique metier utile.
Ils servent a reduire la charge cognitive du LLM.

#### `classify_display_phase(frame_range)`

Retourne une estimation du type de phase:

- `mostly_2d`
- `mostly_3d`
- `mixed_2d_3d`
- `ot_submit_only`
- `uncertain`

avec:

- score de confiance
- resume des preuves

#### `find_display_hotspots(frame_range)`

Retourne directement:

- top GTE PCs
- top packet builder PCs
- top OT writer PCs
- top submit functions

#### `draft_mode_rule(frame_range or hotspot)`

Genere un brouillon de regle `.psx3dprof` ou equivalent:

- plages PC utiles
- mode pressenti
- notes de validation

#### `summarize_display_path(frame_range)`

Produit une synthese exploitable par humain ou LLM:

- "GTE burst here"
- "packets built there"
- "OT filled there"
- "submitted here"

---

## Outils a exposer directement au LLM

Si on doit limiter fortement la surface MCP, voici le noyau recommande.

### Priorite P0

- `summarize_gte_activity`
- `find_dma2_gpu_producers`
- `find_ot_writers`
- `summarize_gpu_packets`
- `trace_ot_chain`
- `get_linked_poly_groups`
- `get_transform_roots`
- `decompile_function`
- `disassemble_function`
- `get_xrefs_to`
- `get_xrefs_from`

### Priorite P1

- `correlate_gte_to_gpu`
- `find_draw_pipeline`
- `classify_display_phase`
- `draft_mode_rule`
- `get_group_transform_links`
- `get_mesh_cache_candidates`

### Priorite P2

- `detect_ot_insert_pattern`
- `trace_gpu_packet_writers`
- `trace_gpu_submit`
- `find_display_hotspots`
- `summarize_display_path`

---

## Pourquoi ces outils plutot que des primitives brutes

Un LLM travaille mal si on lui demande de:

- lire trop de RAM brute
- raisonner sur des milliers de PCs sans resume
- deviner seul si une zone est 2D, 3D, OT, ou simple bruit de boot

En revanche, il travaille bien si on lui donne:

- des top hotspots
- des histogrammes structures
- des echantillons de packets
- une correlation causale deja preparee

Le bon compromis est donc:

- garder les outils bas niveau pour confirmer
- pousser la decouverte via des outils metier intermediaires

---

## Exemples de signatures JSON

### 1. `summarize_gte_activity`

Input:

```json
{
  "frame_range": {"start": 120, "end": 140},
  "opcode_filter": ["RTPT", "RTPS", "NCLIP", "AVSZ4"],
  "top_n": 8
}
```

Output:

```json
{
  "frames": {"start": 120, "end": 140},
  "top_opcodes": [
    {"opcode": "RTPT", "count": 812},
    {"opcode": "NCLIP", "count": 406},
    {"opcode": "AVSZ4", "count": 401}
  ],
  "top_pcs": [
    {"pc": "0x800477A4", "opcode": "RTPT", "count": 512},
    {"pc": "0x800477EC", "opcode": "NCLIP", "count": 251}
  ],
  "summary": "Strong structural 3D activity concentrated in one helper region."
}
```

### 2. `find_dma2_gpu_producers`

Input:

```json
{
  "frame_range": {"start": 120, "end": 140},
  "top_n": 8,
  "include_partial_stores": true
}
```

Output:

```json
{
  "top_writers": [
    {
      "pc": "0x800264B0",
      "words_consumed_by_dma2": 14832,
      "sequential_write_ratio": 0.94,
      "notes": "Likely packet builder"
    }
  ],
  "summary": "One dominant RAM writer feeds most GPU DMA words in the selected window."
}
```

### 3. `find_ot_writers`

Input:

```json
{
  "frame_range": {"start": 120, "end": 140},
  "ot_base": "0x80112000",
  "ot_size": 8192,
  "include_partial_stores": true
}
```

Output:

```json
{
  "top_writers": [
    {
      "pc": "0x80043F38",
      "writes": 1632,
      "partial_store_ratio": 0.78,
      "pattern": "custom_addprim_like"
    }
  ],
  "summary": "OT insertion appears dominated by partial-store helper logic."
}
```

### 4. `summarize_gpu_packets`

Input:

```json
{
  "frame_range": {"start": 120, "end": 140},
  "include_examples": true
}
```

Output:

```json
{
  "packet_counts": {
    "tri_flat": 42,
    "tri_textured": 18,
    "quad_textured": 266,
    "rect_sprite": 12,
    "setup": 9
  },
  "submit_mode": {
    "dma2_linked_list": 0.93,
    "gp0_direct": 0.07
  },
  "examples": [
    {
      "kind": "quad_textured",
      "source_pc": "0x800264B0",
      "bucket": 83
    }
  ]
}
```

### 5. `find_draw_pipeline`

Input:

```json
{
  "frame_range": {"start": 120, "end": 140},
  "scene_hint": "ridge_racer_attract_demo"
}
```

Output:

```json
{
  "gte_zone": {
    "pcs": ["0x800477A4", "0x800477EC"],
    "confidence": 0.94
  },
  "packet_builder_zone": {
    "pcs": ["0x800264B0", "0x80026518"],
    "confidence": 0.97
  },
  "ot_insert_zone": {
    "pcs": ["0x80043F38"],
    "confidence": 0.91
  },
  "submit_zone": {
    "pcs": ["0x800190F0"],
    "confidence": 0.88
  },
  "summary": "Classic 3D path: GTE helper -> packet builder -> OT insertion -> DrawOTag/submit."
}
```

---

## Workflows LLM recommandes

### Workflow A - "Trouver si une scene est vraiment 3D"

1. `classify_display_phase(frame_range)`
2. `summarize_gte_activity(frame_range)`
3. `summarize_gpu_packets(frame_range)`
4. `find_draw_pipeline(frame_range)`
5. ouvrir seulement les fonctions chaudes dans Ghidra

### Workflow B - "Partir de DrawOTag"

1. identifier le submit runtime
2. `find_ot_writers(frame_range, ot_base, ot_size)`
3. `trace_ot_chain(ot_base, bucket_index, max_links)`
4. `find_dma2_gpu_producers(frame_range)`
5. `correlate_gte_to_gpu(frame_range)`
6. confirmer par `decompile_function(address)`

### Workflow C - "Cas moteur custom sans SDK visible"

1. ignorer les noms SDK au debut
2. `find_dma2_gpu_producers(...)`
3. `find_ot_writers(..., include_partial_stores=true)`
4. `detect_ot_insert_pattern(pc_or_function)`
5. `summarize_gte_activity(...)`
6. `find_draw_pipeline(...)`

### Workflow D - "Orchestrateur autonome profond"

But:

- laisser le LLM choisir ses outils
- mais dans une boucle de decision structuree

Sequence recommandee:

1. faire une passe runtime initiale
   - inventorier les hotspots GTE
   - inventorier OT / DMA2 / draw signal
   - noter les groupes, roots et patterns utiles

2. produire une memoire de notes compacte
   - pas de dump brut massif
   - seulement les preuves importantes et leurs relations

3. ouvrir Ghidra de facon ciblee
   - fonctions candidates
   - xrefs
   - callers/callees
   - wrappers Sony 3D ou builders suspects

4. noter explicitement ce qu'on comprend
   - lien direct ou indirect entre sortie GTE et packet builder
   - vertex fill ou poly fill
   - helper OT, wrapper Sony, macro custom, ou builder intermediaire

5. refaire une ou plusieurs passes runtime
   - revalider les hypotheses
   - avancer la scene si necessaire
   - essayer plusieurs observations si le signal est faible

6. ne classifier qu'a la fin d'un minimum de convergence
   - si les preuves restent faibles, retourner "insufficient evidence"
   - ne pas inventer une conclusion forte

---

## Backlog d'implementation recommande

### Phase 1 - Valeur immediate

Implementer d'abord:

1. `summarize_gte_activity`
2. `find_dma2_gpu_producers`
3. `find_ot_writers`
4. `summarize_gpu_packets`
5. `trace_ot_chain`
6. `get_linked_poly_groups`
7. `get_transform_roots`

Ces outils suffisent deja a:

- separer beaucoup de cas 2D / 3D
- identifier les vrais hotspots d'affichage
- eviter de lire du code non pertinent
- commencer a raisonner au niveau "objet probable"

### Phase 2 - Correlation causale

Implementer ensuite:

1. `correlate_gte_to_gpu`
2. `find_draw_pipeline`
3. `detect_ot_insert_pattern`
4. `get_group_transform_links`

Ces outils permettent de transformer les observations en "mode" explicite.

### Phase 3 - Surface LLM premium

Implementer enfin:

1. `classify_display_phase`
2. `find_display_hotspots`
3. `draft_mode_rule`
4. `summarize_display_path`
5. `get_mesh_cache_candidates`
6. `get_scene_vector_snapshot`

Ces outils sont ceux qui rendent le mieux un LLM petit ou local.

---

## Vision Vectorielle Pour Le LLM

But:

- ne pas donner au LLM une image raster
- ne pas ajouter de renderer au CLI
- donner au LLM une "vision de scene" structuree et exploitable

Principe:

- la vision utile pour le CLI MCP doit etre vectorielle / causale
- elle doit decrire la scene comme un ensemble de groupes, objets probables,
  racines de transforms et relations
- elle doit etre suffisamment compacte pour etre lue vite par un LLM

Le premier tool cible est:

- `emu.get_scene_vector_snapshot`

Ce tool doit fusionner ce qu'on sait deja sur:

- la draw list courante
- les linked poly groups
- les transform roots
- les group transform links
- les mesh cache candidates

Sans chercher a "resoudre" toute la scene.

### Objectif fonctionnel

Permettre au LLM de repondre plus vite a:

- "quels sont les gros groupes visibles dans cette scene ?"
- "quel groupe ressemble a un objet stable ?"
- "quels groupes semblent partager une meme racine de transform ?"
- "ou est la masse 3D dominante dans l'ecran ?"
- "est-ce qu'on voit une structure parent/enfant probable ?"

### Entree recommandee

```json
{
  "max_groups": 24,
  "max_roots": 12,
  "include_hud": false,
  "include_raw_triangles": false
}
```

Notes:

- tous les champs sont optionnels
- `include_hud=false` par defaut pour privilegier le monde 3D
- `include_raw_triangles=true` ne doit etre active que pour du debug avance

### Sortie recommandee

```json
{
  "frame_id": 412,
  "display": {
    "width": 320,
    "height": 240,
    "pal": false
  },
  "scene_summary": {
    "dominant_kind": "active_3d",
    "dominant_source_pc": 2147646640,
    "dominant_root_addr": 1049216,
    "dominant_group_count": 5,
    "hud_present": true,
    "confidence": 0.83
  },
  "groups": [
    {
      "group_id": "pc:0x800264B0_face:173",
      "source_pc": 2147646640,
      "face_idx": 173,
      "kind": "object_candidate",
      "tri_count": 84,
      "quad_halves": 40,
      "screen_bbox": {"min_x": 91, "min_y": 52, "max_x": 238, "max_y": 187},
      "screen_center": {"x": 164.5, "y": 119.5},
      "screen_extent": {"w": 147, "h": 135},
      "ot_z_range": {"min": 68, "max": 76},
      "material_signature": "t84_s0_r0",
      "seen_frames": 37,
      "stable_score": 24,
      "promotion_score": 81,
      "root_addr": 1049216,
      "root_last_pc": 2147647024,
      "root_confidence": 0.71,
      "parent_hint": null,
      "children_hints": ["pc:0x800264B0_face:201"],
      "notes": "large stable 3D group near screen center"
    }
  ],
  "roots": [
    {
      "addr": 1049216,
      "region": "ram",
      "score": 1187,
      "frame_hits": 18,
      "gte_reg_mask": 7340032
    }
  ],
  "summary": "One dominant central 3D object candidate, likely linked to one main transform root, with separate HUD activity."
}
```

### Champs minimaux par groupe

Chaque groupe doit idealement exposer:

- `group_id`
- `source_pc`
- `face_idx`
- `tri_count`
- `screen_bbox`
- `screen_center`
- `ot_z_range`
- `seen_frames`
- `stable_score`
- `root_addr` si un lien existe
- `root_confidence`

### Derivations utiles

Pour aider le LLM, le tool doit calculer:

- centre ecran du groupe
- taille apparente du groupe
- tri_count relatif a la scene
- rang de dominance visuelle
- groupe central / lateral / haut / bas
- groupe proche du HUD ou clairement separe

Ces derivees sont plus utiles qu'une liste brute de triangles.

### Heuristiques assumees

Le tool reste heuristique.

Il peut utiliser:

- tri_count
- bbox ecran
- recurrence temporelle
- `source_pc`
- `face_idx`
- `ot_z`
- lien heuristique groupe -> racine

Il ne doit pas pretendre:

- connaitre avec certitude l'objet source
- reconstruire une hierarchie finale
- identifier des bones sans validation supplementaire

### Relation avec Ghidra

Ce tool est un point d'entree pour Ghidra, pas un remplacement.

Il doit aider le LLM a choisir:

- quelle fonction packet builder ouvrir
- quelle racine RAM verifier
- quel groupe parait central
- quelle relation parent/enfant vaut la peine d'etre confirmee

### Outils derives a prevoir ensuite

Une fois `get_scene_vector_snapshot` en place, les outils suivants deviennent naturels:

1. `emu.get_object_candidates`
2. `emu.get_hierarchy_candidates`
3. `emu.get_scene_delta`
4. `emu.get_scene_salience_summary`
5. `emu.step_with_pad_observation`

Role:

- `get_object_candidates`
  - vue plus orientee "objets promouvables" pour le futur cache `mesh3d`
- `get_hierarchy_candidates`
  - vue plus orientee liens parent/enfant, limbs, bones-like
- `get_scene_delta`
  - comparaison de deux snapshots pour savoir ce qui bouge, apparait, disparait

### Recommandation pratique

Ne pas commencer par:

- une image
- un export massif de triangles
- un faux renderer CLI

Commencer par:

- un snapshot scene compact
- quelques groupes dominants
- quelques racines de transform
- une phrase de synthese

C'est cette forme qui donne au LLM une vraie "vision vectorielle" exploitable.

---

## Recommandations de design

### 1. Toujours retourner un resume court

Chaque outil devrait retourner:

- donnees structurees
- plus une phrase de synthese exploitable

Le LLM lit plus vite:

- "one dominant packet builder PC"
- "mostly 2D sprite traffic"
- "OT insertion done via partial-store helper"

qu'un long blob de nombres bruts.

### 2. Toujours retourner des scores de confiance

Beaucoup de scenes seront mixtes ou ambigues.
Il faut assumer l'incertitude au niveau API.

### 3. Support natif des stores partiels

Point critique pour PS1:

- `SWL`
- `SWR`
- `LWL`
- `LWR`

Sinon on rate des helpers OT custom importants.

### 4. Garder la possibilite d'ouvrir le brut

Les outils metier doivent accelerer.
Ils ne doivent pas masquer l'acces:

- a la RAM
- au packet trace brut
- au desassemblage cible

### 5. Penser "scene window", pas seulement "global run"

Presque tous les outils doivent accepter:

- `frame_range`
- ou `window_ms`
- ou `pc_scope`

Car une scene menu, HUD, attract demo, et gameplay n'ont pas le meme profil.

---

## Decision pratique

Si on doit optimiser pour la mission actuelle "classification des chemins
d'affichage PS1", alors:

- la priorite n'est pas d'ajouter plus d'outils Ghidra
- la priorite est d'ajouter des outils emulator MCP centres sur:
  - activite GTE
  - producteurs DMA2/GPU
  - remplissage OT
  - typologie des packets

Ghidra reste essentiel, mais en second rideau.

Mieux formule:

- l'emulator MCP doit etre le premier filet de tri
- Ghidra doit rester librement selectable par le LLM des que la question
  devient structurelle
- il ne faut pas chercher a resoudre de force au runtime un probleme que
  Ghidra peut clarifier plus vite

Le LLM doit d'abord pouvoir repondre:

- "ou est la vraie zone 3D ?"
- "ou est la vraie zone packet builder ?"
- "ou est la vraie insertion OT ?"

Puis seulement:

- "quelle fonction Ghidra dois-je ouvrir ?"

---

## Etat Actuel (2026-04-17)

Premier lot effectivement implemente cote CLI MCP:

- `emu.get_gte_trace_summary`
- `emu.get_dma2_nohint_summary`
- `emu.get_draw_list_summary`
- `emu.get_camera_candidates`
- `emu.get_linked_poly_groups`
- `emu.get_transform_roots`
- `emu.get_group_transform_links`
- `emu.get_mesh_cache_candidates`
- `emu.get_pad_state`
- `emu.set_pad_state`
- `emu.tap_pad_buttons`
- `emu.tap_pad_named_buttons`
- `emu.get_scene_vector_snapshot`
- `emu.get_scene_delta`
- `emu.get_object_candidates`
- `emu.get_scene_salience_summary`
- `emu.get_hierarchy_candidates`
- `emu.get_focus_candidate`
- `emu.step_with_pad_observation`

Ces outils restent:

- des observables runtime
- des regroupements heuristiques
- des aides a la decision pour le LLM

Etat plus recent:

- `DrawCmd3D` transporte maintenant `source_pc` quand il est connu
- `emu.get_linked_poly_groups`, `emu.get_group_transform_links` et
  `emu.get_mesh_cache_candidates` exposent aussi `source_pc`
- la cle de regroupement temporelle ne repose donc plus uniquement sur `face_idx`,
  mais sur `(source_pc, face_idx)` quand `source_pc` est disponible
- ce champ sert de pont pratique vers Ghidra pour ouvrir plus vite le bon builder,
  la bonne zone GTE ou la bonne hierarchie candidate
- des tools MCP de pad existent aussi cote CLI pour aider a naviguer dans les menus
  et faire avancer une scene d'analyse sans script externe
- une premiere "vision vectorielle" existe aussi cote CLI avec
  `emu.get_scene_vector_snapshot`
- `emu.get_scene_delta` permet de savoir ce qui bouge ou reste stable entre deux
  observations successives
- `emu.get_object_candidates` donne une vue plus orientee "objets promouvables"
- `emu.get_scene_salience_summary` donne une vue plus orientee "quoi regarder
  maintenant"
- `emu.get_hierarchy_candidates` donne une premiere vue parent/enfant heuristique
- `emu.get_focus_candidate` donne un "meilleur focus courant" unique pour aider
  la prise de decision
- `emu.step_with_pad_observation` compacte la boucle action -> observation -> delta
  -> salience en un seul appel MCP
- les tools d'observation scene exposent maintenant aussi un bloc `timing` avec:
  - `frame_delta_since_last_observation`
  - `wall_ms_since_last_observation`
  ce qui aide le LLM a raisonner sur l'evolution entre deux appels MCP

Ils ne remplacent pas:

- l'ouverture ciblee dans Ghidra
- la validation humaine
- la future implementation du vrai cache `mesh3d` UE5
