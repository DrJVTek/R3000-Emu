# PSX Render Loop Playbook — Types A-F

> **Version** : 1.0 — 2026-04-17
> **Destinataires** : humain (référence) + LLM (system prompt injecté lors des sessions d'analyse)
> **Contexte** : classifier les boucles de rendu 3D des jeux PS1 pour reconstituer la géométrie object-space (matrices + vertices) en vue d'un rendu VR natif sur Meta Quest 3.

---

## §1 — Introduction et philosophie

### Pourquoi classifier

La PS1 n'a pas de GPU 3D. Le CPU + le GTE (coprocesseur 2) projettent les vertices 3D en coordonnées écran 2D, puis le GPU rasterise ces polygones 2D. À la sortie du pipeline, **toute information 3D originale est perdue** — le GPU ne voit que des `POLY_GT4`, `POLY_FT3`, etc. avec des coordonnées écran.

Pour faire du VR sur Quest 3, il faut reconstituer :
- Les **matrices de transformation** (caméra, objet, hiérarchie)
- Les **vertices object-space** (avant projection)
- La **hiérarchie logique** (quel groupe de triangles appartient à quel objet)

La seule source de vérité est le **code MIPS du jeu lui-même**, observable via GTE+bus+GPU emulator. Chaque jeu a son propre style de boucle de rendu — **pas de solution universelle**. D'où la nécessité d'une **classification par pattern**.

### Pourquoi un catalogue fermé A-F

Le champ des boucles de rendu PS1 est vaste mais **pas infini**. Les 6 types ci-dessous couvrent ~95% des jeux commerciaux, avec des **variantes** pour les cas spécifiques. Quand un jeu ne rentre dans aucun type → on étend le catalogue (voir §7 historique).

Les noms de type (A/B/C/D/E/F) sont des **labels humains de haut niveau** (concept). Les noms techniques (`ot_classic_rtpt`, `paired_edge_rtpt_gt4`, etc.) sont les **valeurs `ModeKind` C++** (implémentation). Un Type peut avoir plusieurs `ModeKind` variants.

### Comment ce doc est destiné à être utilisé

- **Humain** : lecture linéaire §1 → §7 pour comprendre le vocabulaire commun. Référence rapide §3 quand tu lis un jeu inconnu.
- **LLM** : injecté comme system prompt. Quand l'utilisateur demande "classifie ce jeu", le LLM applique l'arbre de décision §4 dans l'ordre + utilise le vocabulaire §3 pour produire une réponse structurée.

---

## §2 — Glossaire PSX rendering pipeline

Minimum vital pour lire la suite. Chacun ~1 ligne.

| Terme | Définition |
|---|---|
| **GTE** | Geometry Transform Engine, COP2 du R3000. Fait les transformations matricielles + projections perspective. |
| **RTPS** | `COP2 cfc=0x01`. Projette **1 vertex** (V0) en SXY + depth. 5-15 cycles selon état. |
| **RTPT** | `COP2 cfc=0x30`. Projette **3 vertices** (V0,V1,V2) en une passe. Plus rapide pour triangles. |
| **NCLIP** | Back-face culling sur 3 SXY déjà projetés. |
| **NCS/NCT/NCDS** | Normal Color Single/Triple/Depth-shaded — lighting via GTE. |
| **AVSZ3/AVSZ4** | Average Z sur 3 ou 4 SXY — calcul OT bucket. |
| **MTC2** | `COP2 register write` — CPU charge un vertex / matrice dans le GTE. |
| **MFC2** | `COP2 register read` — CPU lit SXY / SZ / flag depuis GTE. |
| **CTC2** | `COP2 control register write` — charge la matrice RT/TR ou les offsets. |
| **SXY0/1/2** | Registres GTE 12-14, FIFO des coordonnées écran projetées. |
| **OT** | Ordering Table. Tableau de ~512-2048 "tag words" indexés par Z. Le rasterizer parcourt l'OT dans l'ordre → tri en Z par bucket (painter's algorithm). |
| **Tag word** | Mot de 32 bits : 24 bits = adresse du next node, 8 bits = nombre de words GPU qui suivent. |
| **AddPrim** | Helper PSYQ SDK : `prim->tag = ot[z]; ot[z] = (adr(prim) << 8) \| size;` — insertion au head de bucket z. |
| **DMA2** | DMA channel 2 (GPU). Parcourt l'OT en linked-list et envoie les packets au GP0. |
| **GP0** | Registre GPU de commandes de dessin. Reçoit les packets via DMA2 ou direct CPU. |
| **POLY_F3/F4** | Triangle/Quad flat-shaded (couleur uniforme). |
| **POLY_G3/G4** | Triangle/Quad Gouraud (couleur par vertex). |
| **POLY_FT3/FT4** | Texturé flat. |
| **POLY_GT3/GT4** | Texturé Gouraud. |
| **PSYQ SDK** | SDK officiel Sony. Wrappers : `AddPrim`, `ClearOTag`, `SetGeomOffset`, `SetGeomScreen`, `RotMatrix`, etc. Adresses typiques en BIOS + lib ~0x80140000-0x80150000. |
| **TMD** | Tool-Mesh-Data, format pré-compilé PSYQ. Contient polygones + matrices embarquées. |
| **`DivPolygon*`** | Famille de helpers PSYQ pour subdiviser un polygone en plus petits (évite warping de texture). |
| **`RCpolyGT4`** | PSYQ helper récurrent pour un quad Gouraud textured avec clipping. |

---

## §3 — Six types canoniques

**Remarque importante** : les deux modes `ModeKind` déjà présents dans `psx3d_profile_store.h` (`paired_edge_rtpt_gt4`, `subdivided_ft4_intpl_rtpt`) sont des **variantes** de Type A et Type E respectivement. Ce document les englobe dans la taxonomie plus large.

---

### Type A — OT classic RTPT

**Nom court** : *ot_classic_rtpt*
**Fréquence estimée** : ~60-70% des jeux commerciaux

#### Signature MIPS

```asm
; charge les 3 vertices dans GTE
lwc2   $0, 0(v0)      ; MTC2 V0 = vertex[0].xyz
lwc2   $2, 0(v1)      ; MTC2 V1 = vertex[1].xyz
lwc2   $4, 0(v2)      ; MTC2 V2 = vertex[2].xyz
; projection
cop2   0x280030       ; RTPT (3 vertices en une passe)
; lit SXY
mfc2   t0, $12        ; SXY0
mfc2   t1, $13        ; SXY1
mfc2   t2, $14        ; SXY2
; calcule Z moyen pour OT bucket
cop2   0x28002D       ; AVSZ3
mfc2   t3, $7         ; OTZ
; écrit dans le struct poly en RAM
sw     t0, 16(poly)
sw     t1, 20(poly)
sw     t2, 24(poly)
; insertion OT via AddPrim
addu   a0, ot, t3     ; &ot[z]
addu   a1, poly       ; &poly
jal    AddPrim        ; tag swap
```

#### Signature observable (emu MCP)

- `get_scene_vector_snapshot` : `dominant_kind` ≈ `"POLY_GT3"` ou `"POLY_GT4"`, groupes contigus
- `get_gte_trace_summary` : top opcodes = `RTPT` + `AVSZ3` (ratio ~3:1), pas de `NCLIP` dominant
- `get_dma2_nohint_summary` : linked-list DMA2 longue (500+ nodes/frame), tag words bien formés
- `find_ot_writers` : écriture séquentielle par bucket Z, pattern `old_head → poly.tag ; poly → ot.head`
- Hotspot PC : zone du jeu (souvent 0x80010000-0x80080000), bouclant serré sur la même fonction

#### Données recouvrables pour VR

- ✅ Object-space vertices : **oui**, via MTC2 avant RTPT (interception trivial)
- ✅ Matrice RT/TR : **oui**, via CTC2 au début de chaque groupe
- ✅ Hiérarchie basique : parfois, si le code charge une nouvelle matrice avant chaque sous-objet
- ✅ Identité de l'objet : via le dominant source_pc (fonction appelante)

**Rien d'irrémédiable**. Type le plus simple à reconstituer.

#### Variante : `paired_edge_rtpt_gt4` (Ridge Racer flag)

Spécialisation où le builder **réutilise** un ou plusieurs vertices d'une itération à la suivante, et écrit un GT4 à partir de **deux edges consécutives** (prev + current) :

```
packet = [ prev_edge_p0, current_edge_p0, prev_edge_p1, current_edge_p1 ]
```

Référence concrète (Ridge Racer) :
- Builder : `DrawFlag` (0x80026110)
- Hot packet store : 0x800264B0
- GTE helper : `GTE_Calc_Flag` (0x800477A4)
- OT helper : `AddOT` (0x80043F38)
- Grid 20×28 = 560 quads/frame

#### Jeux connus

- Ridge Racer (flag + HUD), Wipeout, Crash Bandicoot (la plupart), Tekken menus
- 90% des démos PSYQ SDK

#### `ModeKind` associé

- **`ot_classic_rtpt`** (à ajouter) — cas canonique
- **`paired_edge_rtpt_gt4`** (existe déjà) — variante edge-reuse

#### Quirks typiques

- `force_gte_geom_offset_zero` (libgs games)
- Aucun autre par défaut

---

### Type B — Direct DMA submission

**Nom court** : *direct_dma_submission*
**Fréquence estimée** : ~5-10%

#### Signature MIPS

Pas d'OT. Le jeu construit un buffer de packets en **ordre d'émission** et déclenche DMA2 en mode **continu** (pas linked-list) :

```asm
; setup DMA2 GPU OTC continu
lui    t0, 0x1F80      ; DMA base
li     t1, packet_buf
sw     t1, 0x10A0(t0)  ; DMA2 madr
li     t2, num_packets
sw     t2, 0x10A4(t0)  ; DMA2 bcr
li     t3, 0x01000201  ; continuous mode
sw     t3, 0x10A8(t0)  ; DMA2 chcr
```

Pas d'AddPrim. Le sorting (si présent) se fait **par le CPU avant DMA**.

#### Signature observable

- `get_dma2_nohint_summary` : DMA2 en mode `mode=2` ou continuous, pas de tag chain
- `find_ot_writers` : **vide ou très faible** — aucun tag swap
- `get_gte_trace_summary` : RTPT présent mais moins dense que Type A

#### Données recouvrables pour VR

- ✅ Object-space vertices : oui (MTC2)
- ✅ Matrices : oui (CTC2)
- ⚠ **Ordre de rendu** : le jeu peut compter sur un ordre de submission spécifique pour la transparence/layering. Si on re-sort pour VR, cet ordre peut casser.

#### Jeux connus

- Ridge Racer particle system (non confirmé, à valider)
- Quelques moteurs custom Sega/Konami
- Jeux 2D-heavy qui bypassent OT pour perfs

#### `ModeKind` associé

- **`direct_dma_submission`** (à ajouter)

---

### Type C — Chained polygon stream

**Nom court** : *chained_polygon_stream*
**Fréquence estimée** : ~3-5% (mais présent dans gros jeux !)

#### Signature MIPS

Le jeu pré-construit en mémoire une **chaîne linked-list de packets**, où le `tag word` (offset +0 de chaque poly struct) pointe directement vers le **next poly**. Pas de bucket OT au milieu — le stream est l'OT :

```asm
; chaque poly struct contient :
; [+0x00] tag = (next_poly >> 2) | (size_in_words)
; [+0x04..] payload GPU
; builder inline écrit le tag et incrémente le pointeur
sw     next_ptr, 0(cur_poly)   ; tag = next addr
addiu  cur_poly, cur_poly, SIZE
```

Signatures caractéristiques :
- Les `SW` dans le struct sont **inlinés**, pas de helper `AddPrim` appelé
- Le `GTE_Calc_*` helper est souvent custom, pas du PSYQ standard

#### Signature observable

- `get_linked_poly_groups` : groupes de polygones avec `source_pc` identique, enchaînés par adresse
- `find_ot_writers` : writes à `offset=0` **du struct lui-même**, pas d'une OT séparée
- `trace_ot_chain` : chaîne très longue (peut être >1000 nodes) avec un seul head et pas de bucket Z
- Hotspot PC : dans la fonction du moteur de rendu custom, souvent moteur propriétaire

#### Données recouvrables pour VR

- ✅ Vertices : oui (MTC2 avant projection)
- ⚠ Matrices : **dépend** du moteur — certains moteurs bakent la matrice au build-time dans le poly
- 🚨 **Identité d'objet** : difficile — les polys d'un même mesh sont linked-list mais sans marker explicite. Nécessite d'analyser les `source_pc` et `transform_root` signatures.

#### Jeux connus

- **Tekken (Stage67 investigation en cours)** — candidat fort
- Soul Reaver (à valider)
- Moteurs Namco in-house (Tekken, Ridge Racer Turbo ?)

#### `ModeKind` associé

- **`chained_polygon_stream`** (à ajouter)

---

### Type D — Skinned CPU transform

**Nom court** : *skinned_cpu_transform*
**Fréquence estimée** : ~5-10% (surtout jeux character-focused)

#### Signature MIPS

Le CPU fait une **multiplication matricielle logicielle** (bone × vertex) avant d'envoyer au GTE. Le GTE fait uniquement la **projection finale** (RTPS ou RTPT), pas la transformation de caractère :

```asm
; CPU skinning loop
for each vertex :
  for each bone_influence :
    v_skinned += bone_matrix[b] * vertex_rest * weight
  mtc2   v_skinned, V0     ; résultat CPU → GTE
  cop2   0x180001          ; RTPS (pas RTPT — 1 vertex à la fois)
  mfc2   sxy, 12
  sw     sxy, poly_offset
```

Signature :
- Ratio **MUL/DIV CPU élevé** avant chaque RTPS
- **RTPS dominant, pas RTPT** (car vertices arrivent 1 par 1 après skinning)
- La matrice GTE active est souvent l'identité ou juste la caméra (pas d'object-space transform)

#### Signature observable

- `get_gte_trace_summary` : top opcode = `RTPS` (>>`RTPT`)
- `get_transform_roots` : les candidats matrice caméra ont peu de changements intra-frame — le jeu se repose sur la projection simple
- Instructions `MULT`, `MULTU`, `DIV`, `DIVU` très denses juste avant MTC2

#### Données recouvrables pour VR

- ✅ Vertices post-skinning : oui (MTC2)
- 🚨 **Matrice bone originale** : irrécupérable directement — seul le vertex déjà skinné est visible côté GTE. Pour le VR, on peut soit :
  - Rendre tel quel (VR limité à ce que la projection finale donne)
  - Intercepter **avant** le skinning, ce qui nécessite un hook PC spécifique au jeu (identifier la fonction `skin_vertex()`)
- 🚨 **Hiérarchie squelettale** : idem, à reconstruire en analysant les bone tables en RAM

#### Jeux connus

- Tomb Raider (skinning de Lara)
- Soul Reaver
- MediEvil
- Final Fantasy VII (battle characters)

#### `ModeKind` associé

- **`skinned_cpu_transform`** (à ajouter)

---

### Type E — TMD-compiled / subdivided

**Nom court** : *tmd_compiled*
**Fréquence estimée** : ~5-10% (démos SDK, jeux early)

#### Signature MIPS

Le jeu appelle un **interpréteur de format TMD** du SDK PSYQ. Les helpers standard sont `DivPolygonX`, `DrawOTagEnv`, etc. Typiquement un appel par polygone :

```asm
la     a0, tmd_object
la     a1, view_matrix
jal    DivPolygonFT4      ; ou DivPolygonGT4, ...
; le helper fait le raster de tout l'objet en interne
```

Souvent combiné avec **subdivision** (`INTPL` GTE op) pour éviter le warping de texture sur grands polys.

#### Signature observable

- `get_gte_trace_summary` : `INTPL` présent (rare dans autres types), patterns répétés `RTPT + INTPL + RTPT + INTPL`
- PC hotspots concentrés autour des fonctions PSYQ SDK (0x80140000-0x8014FFFF sur Ridge Racer)
- `find_dma2_gpu_producers` : producers PC **multiples** (un par polygone du TMD), pas un builder unique

#### Variante : `subdivided_ft4_intpl_rtpt` (Ridge Racer gameplay)

Référence concrète (Ridge Racer) :
- Builder : `DivPloyFT4` (0x80047E38)
- Hot packet stores : 0x800482A8, 0x800482AC
- Pattern : outer/inner nested loops, `gte_intpl_b()` + `gte_rtpt_b()` répétés, clipping screen-bounds, écriture child GT4 packets
- Producer range (profile) : 0x80047E38-0x800482F4
- GTE range : 0x80047FFC-0x800482AC
- OT write range : 0x80048288-0x800482B0

#### Données recouvrables pour VR

- ✅ Vertices : oui (mais **après subdivision** — il faut potentiellement ré-agréger)
- ✅ Matrices : oui (chargées en début de TMD draw)
- ⚠ **Identité objet** : donnée par le TMD struct en RAM, mais le CPU dispatch peut re-call le même helper pour plusieurs objets. Hot PC seul ne suffit pas — il faut croiser avec `get_transform_roots`.

#### Jeux connus

- Ridge Racer (mode demo/gameplay, voir adresses ci-dessus)
- Démos PSYQ, petits jeux indé early-era
- MotorToon Grand Prix

#### `ModeKind` associé

- **`tmd_compiled`** (à ajouter, générique)
- **`subdivided_ft4_intpl_rtpt`** (existe déjà, variante subdivision)

---

### Type F — Billboard / radial

**Nom court** : *billboard_radial*
**Fréquence estimée** : ~10-20% (mais en **addition** aux autres, pas exclusif — les particles sont partout)

#### Signature MIPS

Un vertex projeté (ou pas — parfois juste une position monde), puis le CPU **construit manuellement** les 4 coins d'un quad perpendiculaire à la caméra :

```asm
mtc2   v, V0
cop2   0x180001         ; RTPS
mfc2   cx, 12           ; center SXY
; CPU calcule les 4 coins
sub    x0, cx_x, half_w
add    x1, cx_x, half_w
sub    y0, cx_y, half_h
add    y1, cx_y, half_h
sw     x0, poly+0
sw     y0, poly+2
; ... x1,y0 / x0,y1 / x1,y1
```

Signature distinctive : **ratio "1 RTPS → 4 SXY écrits"** au lieu de "1 RTPT → 3 SXY".

#### Signature observable

- `get_gte_trace_summary` : `RTPS` isolés, pas de `RTPT` ni `NCLIP` à proximité
- `get_scene_vector_snapshot` : beaucoup de `POLY_FT4` sprites-like, tailles similaires (effets particules)
- `find_dma2_gpu_producers` : un seul producer PC qui spams le même pattern

#### Données recouvrables pour VR

- ✅ Position monde du billboard : oui, si on intercepte **avant** le RTPS (MTC2 read)
- ✅ Taille écran : oui (écrit dans poly directement)
- 🚨 **Normal/orientation** : perdue — par définition le billboard face caméra
- 🚨 Pour VR stéréo, les billboards 2D cassent l'immersion. Il faut les détecter et les remplacer par des cards toujours-face-viewer dans UE5.

#### Jeux connus

- Système de particules de **tous** les jeux (Gran Turismo effets fumée, Tekken étincelles, ...)
- HUDs 2D
- Final Fantasy VII over-world character sprites

#### `ModeKind` associé

- **`billboard_radial`** (à ajouter)

---

## §4 — Arbre de décision

Le LLM l'applique **dans cet ordre** sur un snapshot `get_scene_vector_snapshot` + `get_gte_trace_summary` + `find_dma2_gpu_producers` donné.

```
┌───────────────────────────────────────────────────────────────┐
│ Q1. get_gte_trace_summary montre-t-il du RTPT dominant ?      │
│                                                               │
│   RTPT > 50% des opcodes GTE ?                                │
│   ├── OUI  → Type A, B, C ou E (goto Q2)                      │
│   └── NON  →                                                  │
│       ├── RTPS dominant (80%+) + MUL/DIV élevé avant MTC2     │
│       │   → Type D (skinned_cpu_transform). STOP.             │
│       ├── RTPS isolé + 4 SXY écrits par vertex projeté        │
│       │   → Type F (billboard_radial). STOP.                  │
│       └── 2D pure ou polling → PAS une render loop. Retry.    │
│                                                               │
│ Q2. find_ot_writers rapporte-t-il des writes OT normaux ?     │
│     (écriture à ot_base + z*4, pattern old_head→new_head)     │
│                                                               │
│   OUI → Q3                                                    │
│   NON :                                                       │
│     ├── DMA2 continuous (non-linked) → Type B. STOP.          │
│     └── DMA2 linked mais writes à offset=0 du poly lui-même   │
│         → Type C (chained_polygon_stream). STOP.              │
│                                                               │
│ Q3. get_gte_trace_summary montre-t-il INTPL ?                 │
│   OUI → Type E (tmd_compiled / subdivided). STOP.             │
│   NON → Q4                                                    │
│                                                               │
│ Q4. Pattern edge-reuse entre itérations successives ?         │
│     (2 vertices consécutifs réutilisés dans le packet d'après)│
│   OUI → Type A variante paired_edge_rtpt_gt4. STOP.           │
│   NON → Type A vanilla (ot_classic_rtpt). STOP.               │
└───────────────────────────────────────────────────────────────┘
```

### Output attendu du LLM

```json
{
  "type": "A",
  "mode_kind": "ot_classic_rtpt",
  "variant": "paired_edge_rtpt_gt4",
  "confidence": 0.85,
  "reasoning": "Q1: RTPT dominant 73%. Q2: OT writes au pattern standard old_head→new_head. Q3: pas d'INTPL. Q4: edge-reuse détecté — le packet k+1 réutilise les points de projection du packet k.",
  "evidence_pcs": ["0x80026110 (DrawFlag)", "0x800264B0 (hot packet store)"],
  "suggested_rule": {
    "mode": "paired_edge_rtpt_gt4",
    "link_rule": "packet_edge_pairs",
    "producer_pc_ranges": [["0x80026100", "0x80026500"]],
    "gte_pc_ranges": [["0x800477A4", "0x80047820"]],
    "ot_fill_pc_ranges": [["0x80043F38", "0x80043F80"]]
  }
}
```

---

## §5 — Ce qui n'EST PAS une render loop (faux positifs)

Quand le PC hotspot est dans une de ces zones, **rejeter** la classification et re-sampler plus tard :

### 5.1 Polling loops

- `VSync wait` — décrémenter un compteur jusqu'à 0 en attendant la VBlank IRQ
- `CDROM data ready wait` — spin-loop sur un bit d'état
- `hretrace` polling — Ridge Racer : `FUN_8004e824` à 0x8004e9xx, observé comme hotspot dominant pendant le boot/menu

**Marqueur** : séquence `ADDIU v,-1` / `SW/LW v,[sp+N]` / `BNE v,!=0,-0x30` (boucle compteur serrée).

### 5.2 IRQ handlers BIOS

- Adresses 0x00001xxx (RAM basse, vecteur IRQ BIOS)
- Marqueur : PC non préfixé par 0x80 (pas en KSEG0)

### 5.3 Audio / SPU mixing

- Rarement en hotspot CPU car le SPU gère lui-même
- Mais si un jeu fait du streaming XA-ADPCM software, le décodeur CPU peut apparaître
- Marqueur : beaucoup de `LH`/`SH` 16-bit + shifts, pas de GTE

### 5.4 HUD / font rendering 2D

- `POLY_FT4` avec UV qui bougent par char
- Pas de GTE, pas d'OT sortie Z (souvent Z=0 ou fixe)
- **Distinction** : regarder le `dominant_kind` + densité GTE. HUD = 0 GTE.

### 5.5 Memory copy / setup

- `ClearOTag` appelé avant chaque frame pour wiper l'OT
- Marqueur : écritures séquentielles à ot_base sans lecture avant

---

## §6 — Comment utiliser ce doc

### Pour toi (humain)

1. **Chaque nouveau jeu** : démarrer par `emu.get_scene_vector_snapshot` + `emu.get_gte_trace_summary` sur une fenêtre active (gameplay, pas intro).
2. Appliquer l'arbre §4 à la main. Ça prend ~3 min.
3. Si le résultat est clair → sauvegarder le `ModeRule` dans le `.psx3dprof` du jeu.
4. Si ambigu → cross-ref Ghidra pour lire la fonction hot (ouvrir `FUN_<hot_pc>`). Le pseudo-code devrait immédiatement révéler si c'est `AddPrim`-based ou custom builder.
5. Si nouveau pattern (ne rentre dans aucun des 6 types) → documenter ici §7 + proposer un nouveau `ModeKind`.

### Pour le LLM (system prompt)

Quand on te donne un snapshot d'un jeu inconnu :

1. Applique l'arbre de décision §4 **dans l'ordre**, pas dans le désordre.
2. Cite explicitement quelle question (Q1, Q2, Q3, Q4) te fait brancher.
3. Si un signal manque (ex : pas de `find_ot_writers` dans ton input), demande-le avant de conclure.
4. Produis la sortie JSON §4. Ajoute `confidence` calibré : 0.9+ si tous les signaux alignés, 0.5-0.7 si contradictions, <0.5 demande à l'humain.
5. **Ne jamais inventer un `mode_kind`** qui n'est pas dans ce doc — si aucun ne colle, dis `"mode_kind": "unknown"` + `"suggested_new_pattern": "..."`.

### Extensibilité

Quand tu découvres un pattern qui ne rentre pas :

1. Ajoute une section §3.G (ou H, I...) ici, avec le même format
2. Ajoute le nom à §4 arbre de décision
3. Propose un nouveau `ModeKind::xxx` à ajouter dans `psx3d_profile_store.h:13-18`
4. Commit avec message `docs(playbook): add Type G <name> for <game>`

---

## §7 — Historique et versioning

| Date | Auteur | Changement |
|---|---|---|
| 2026-04-17 | Claude + user | v1.0 — création initiale. Types A-F + variantes `paired_edge_rtpt_gt4` (Ridge Racer flag) et `subdivided_ft4_intpl_rtpt` (Ridge Racer gameplay) intégrées. Taxonomie basée sur l'existant `psx3d_profile_store.h` + `docs/GTE_GPU_MODE_DISCOVERY_METHOD_2026-03-08.md` + `docs/RIDGE_RACER_ANALYSIS.md`. |

### `ModeKind` déjà présents dans le code (v1.0)

```cpp
// psx3d_profile_store.h:13-18
enum class ModeKind : uint8_t {
    unknown = 0,
    paired_edge_rtpt_gt4,        // Type A variant
    subdivided_ft4_intpl_rtpt,   // Type E variant
};
```

### `ModeKind` à ajouter après validation v1.0

```cpp
enum class ModeKind : uint8_t {
    unknown = 0,
    // Type A
    ot_classic_rtpt,             // NEW — vanilla PSYQ AddPrim
    paired_edge_rtpt_gt4,        // existing — Ridge Racer flag variant
    // Type B
    direct_dma_submission,       // NEW
    // Type C
    chained_polygon_stream,      // NEW — Tekken candidat
    // Type D
    skinned_cpu_transform,       // NEW — Tomb Raider/Soul Reaver
    // Type E
    tmd_compiled,                // NEW — generic TMD
    subdivided_ft4_intpl_rtpt,   // existing — Ridge Racer gameplay variant
    // Type F
    billboard_radial,            // NEW — particles/HUD
};
```

---

## §8 — Références

- `docs/GTE_GPU_MODE_DISCOVERY_METHOD_2026-03-08.md` — méthode step-by-step pour découvrir un nouveau mode
- `docs/RIDGE_RACER_ANALYSIS.md` — Type A (flag) + Type E (gameplay) concrets sur SCUS-94300
- `docs/MCP_DISPLAY_ANALYSIS_TOOLS_PLAN.md` — catalogue des outils MCP observer
- `docs/MESH3D_CACHE_PREREQUISITES_2026-04-17.md` — identité stable de mesh logique
- `src/emu/psx3d_profile_store.h` — enum `ModeKind` source de vérité
- `src/gpu/gte_correlation.h` — `GteSnapshot` + `GteSxyKey` structures qui alimentent les snapshots
- nocash psx-spx — spec officielle GTE/GPU : <https://problemkaputt.de/psx-spx.htm>
- PSYQ SDK docs (libgpu/libgte man pages) — pour le vocabulaire des helpers
