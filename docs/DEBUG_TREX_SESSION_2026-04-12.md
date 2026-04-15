# Debug Session — TREX dino double-buffer + near-clip polygons

**Date**: 2026-04-12  
**Branch**: `codex/psx-naming-surface-refactor`  
**Commit**: `6692694`  
**Game**: TREX.EXE (SCEE Demo One Europe, loaded via `--load` devkit mode)

---

## Partie 1 — Double-buffer collapse (RÉSOLU)

### Symptôme initial
Le dino sautait entre deux positions Y (haut/bas) à chaque frame dans UE5.
Deux problèmes distincts découverts et fixés :

### Bug 1 : PSX2DRenderComponent utilisait display_x/y dans l'origin
`PSX2DRenderComponent.cpp:312-319` calculait `OriginY = display_y + height/2`.
`display_y` alterne entre 0 et 256 à chaque frame (double-buffer VRAM).
→ Le mesh sautait de 256 pixels chaque frame.

**Fix** : `OriginX = width/2`, `OriginY = height/2`, toujours centré. `display_x/y` n'entre plus
dans le calcul. Gros bloc commentaire de policy "DOUBLE-BUFFER COLLAPSE" documenté.

### Bug 2 : TREX utilise SetGeomOffset pour le double-buffer (pas GP0 0xE5)
Découvert via **Ghidra** sur le binaire TREX.EXE :
- `FUN_8014047c` = `SetGeomOffset(x, y)` → écrit cop2 OFX/OFY
- `FUN_8013ade0` (appelé à chaque frame après VSync) → switch OFX/OFY selon buffer index
- TREX fait son shift de back-buffer via les registres GTE, pas via GP0(0xE5)

Conséquence : les coords GP0 brutes arrivent au GPU avec le shift Y **déjà incrusté** par la projection GTE RTPS.

**Fix** : quirk `force_gte_geom_offset_zero` dans `gte.cpp::rtps_internal` et `gte_3d.cpp::rtps_internal`.
Quand le flag est actif, RTPS utilise `ofx = ofy = 0` au lieu de `ctrl_[C_OFX/C_OFY]`.
→ Les SXY projetés restent en logical screen space indépendamment du buffer.

### Système de profils psx3dprof (nouveau)
- Format fichier : `QUIRK force_gte_geom_offset_zero 1`
- Parser/serializer dans `psx3d_profile_store.cpp`
- Propagation auto : `Core::try_load_psx3d_profile()` → `gte.set_force_geom_offset_zero()`
- UE5 : profiles dans `<Project>/Saved/PSXProfiles/<game_id>.psx3dprof`
- Le game_id strip les extensions `.EXE/.PSX/.PS1/.PSEXE` via `psx3d_normalize_boot_game_id`
- Fichier `profiles/psx3d/TREX.psx3dprof` créé avec le quirk

### PsxParam — passage de params aux EXE PSX (nouveau)
- TREX supporte un mode **attract** (démo auto sans pad) via `$a1 → {mode, timeout_sec}`
- `mode=1` = attract, `timeout` en secondes PAL (×50 vbl)
- Implémenté : `Core::set_psx_params()` écrit dans scratchpad RAM `0x1F800200`
- CLI : `--psxparam=1,30`
- UE5 : UPROPERTY `PsxParam = "1,60"` sur PSXEmulatorComponent
- Découvert via Ghidra : `FUN_8012673c` lit `*($a1)` et `*($a1+4)`

---

## Partie 2 — Near-clip polygon bug (EN COURS)

### Symptôme
Quand le dino est au plus proche de la caméra (fin de séquence attract) :
- **(a)** Des triangles **front-face** de la tête disparaissent (trou béant)
- **(c)** Des triangles se **déforment** (étirent en pointes)
- Progressif : plus le dino s'approche, plus de polys sont affectés
- "Comme coupé au laser au niveau polygons"
- Sur vrai PS1 et DuckStation : rendu correct. Chez nous : trou.

### Ce qu'on a PROUVÉ (instrumentation GTE v3)

| Test | Résultat | Conclusion |
|---|---|---|
| `gte_divide` overflow (`sz3*2 <= h`) | **0 hits** | Pas de near-clip overflow |
| NCLIP cull avec vertices saturés | **0 hits** | NCLIP ne rejette pas à cause de saturation |
| AVSZ3 OTZ=0 | **0 hits** | Aucun poly tombe à profondeur 0 |
| AVSZ3 OTZ values | 14500-15400 | Profondeurs normales |
| FLAG register | Toujours 0x00000000 | Aucun bit d'erreur GTE |
| DRAWLIST_SUMMARY cmds | 1276 au close-up | Tous les polys sont dans le draw list |
| DRAWLIST_SUMMARY coords | `[-200..601, -377..119]` | Coords extrêmes mais pas saturées |
| push_triangle span check | **Supprimé** | Plus de rejet de triangles larges |

### Ce qu'on a ÉLIMINÉ

- ❌ GTE gte_divide bug → matche DuckStation exactement (vérifié ligne par ligne)
- ❌ GTE NCLIP formula → identique à DuckStation
- ❌ GTE push_sxy saturation → identique (-1024..+1023)
- ❌ GTE AVSZ3/OTZ → valeurs correctes, rien ne tombe à 0
- ❌ FLAG bit overflow → aucun flag levé
- ❌ push_triangle span > 1023 reject → supprimé (causait des trous, confirmé par logs)
- ❌ Limite de nombre de polys → aucune limite trouvée (std::vector sans cap, UE5 OK pour 3828 verts)
- ❌ Shadow GTE push_sxy desync → fixé (saturation alignée avec primary)

### Comparaison DuckStation (source local)

Code GTE comparé fichier par fichier depuis `E:/Projects/github/Live/duckstation/src/core/gte.cpp` :

| Fonction | Résultat |
|---|---|
| `UNRDivide` (gte_divide) | Identique (condition, CLZ shift, UNR table, reciprocal, cap 0x1FFFF) |
| `RTPS` (rtps_internal) | Identique (dot3, sign_extend_mac 44-bit, push_sz, push_sxy, MAC0 check) |
| `PushSXY` | Identique (saturation -1024..+1023, FLAG bits) |
| `PushSZ` | Identique (clamp 0..0xFFFF) |
| `NCLIP` | Identique (cross product formula, set_mac) |
| `AVSZ3` | Identique (ZSF3 × sum >> 12, OTZ clamp) |
| `BeginPolygonDraw` span check | **DuckStation n'a PAS de span > 1023 reject** dans le path normal (seulement dans le fallback FF8). Notre ancien check était trop agressif → supprimé. |

### Hypothèse actuelle (la plus probable)

**C'est un problème de SATURATION des coords SXY** — mais pas au sens "valeur = ±1024".
C'est au sens **"les points du triangle sont tellement écartés que le triangle ne couvre plus la bonne surface"**.

Quand le dino est très proche :
1. Un vertex de la tête projette à `V.x = 601` (loin à droite hors écran)
2. Un autre vertex projette à `V.x = -200` (loin à gauche)
3. Le troisième vertex est à `V.x = 50` (sur l'écran)

Sur le **hardware PS1** : le GPU rasterise ce triangle et **clippe au pixel** contre le drawing area.
Seule la partie visible (la tranche entre -200 et 320) est dessinée. Le résultat est correct.

Sur **DuckStation** : même chose — le rasterizer clip au drawing area.

Sur **notre UE5** : on pousse les 3 vertices tels quels dans un `ProceduralMeshComponent`.
UE5 dessine le TRIANGLE ENTIER sans clipping PS1. Le résultat :
- Le triangle s'étire sur une énorme surface → **déformation (c)**
- Si les 3 vertices projettent TOUS hors-écran du même côté (saturation à ±1024 ou valeurs extrêmes),
  le triangle couvre une zone qui ne devrait PAS être visible → **trou (a)** parce que ce triangle
  "bouche" des trous dans le mesh mais au mauvais endroit, ou parce que le triangle devient
  dégénéré (3 points quasi-alignés ou quasi-identiques après la projection extrême).

### Piste de fix pour la prochaine session

Deux approches possibles :

**Approche A — Screen-space triangle clipping dans PSX2DRenderComponent**
Avant d'injecter un triangle dans le mesh UE5, le clipper contre le rectangle d'affichage PS1
`[0..width, 0..height]` (en espace logique). Algorithme Sutherland-Hodgman standard :
un triangle clippé contre 4 bords produit 0 à 7 triangles. C'est ce que le GPU PS1 fait
internement au rasterizer. Plus correct, mais plus de code.

**Approche B — Clamp vertex coords au display rect**
Clamper chaque `V.x` à `[0..width]` et `V.y` à `[0..height]` avant de construire le mesh.
Ça distord le triangle mais le confine au rectangle visible. Plus simple mais moins correct
(change la forme du triangle au lieu de le clipper proprement).

**Approche C — PGXP-light dans le shadow GTE**
Le shadow GTE a déjà des valeurs unclamped pour SZ. On pourrait lui faire calculer SXY en
float (haute précision, pas de saturation) et utiliser ces SXY pour le rendu UE5 au lieu des
SXY saturés du primary. Ça donnerait des triangles qui ne saturent jamais, mais ça découple
notre rendu du hardware PS1 (même approche que DuckStation PGXP).

---

## Partie 3 — Nouvelle direction validée : backend GTE moderne expérimental

### Changement de diagnostic

Après revue du code et des symptômes, la piste la plus crédible n'est plus :
- "le composant UE clippe mal"
- ni "le GTE fidèle fait une erreur évidente sur RTPS/NCLIP/AVSZ"

La piste la plus crédible devient :

**nous interprétons trop littéralement les sorties SXY du GTE fidèle comme une géométrie
UE5 finale**, alors que sur la PS1 ces coordonnées servent d'abord au rasterizer GPU.

Autrement dit :
- le **GTE fidèle actuel** peut être correct
- mais notre **usage moderne** de ses coordonnées saturées ne l'est pas forcément

### Idée validée pour expérimentation

Créer un **nouveau backend GTE moderne** séparé du GTE fidèle :

- **Gte (actuel)** = backend fidèle PS1, inchangé
- **GteModern / GteFloat (nouveau)** = backend expérimental
  - calculs internes en `float` / `double`
  - conservation possible d'une vue "compat PS1" pour les registres exposés
  - conservation en parallèle de coordonnées projetées **non saturées**
  - utilisable uniquement pour le rendu moderne / debug / 3D / expériences UE5

Important :
- **on ne touche pas au GTE actuel**
- **on ne remplace pas le comportement PS1 de référence**
- on ajoute un backend alternatif, sélectionnable par option

### Pourquoi cette approche est la bonne

Si on enlève la saturation `SXY` directement dans le backend fidèle :
- on risque de casser le GPU 2D
- on risque de casser des jeux qui dépendent de la sémantique PS1 exacte
- on perd la référence hardware

Si on ajoute un backend expérimental séparé :
- on garde un chemin fidèle intact
- on peut comparer A/B facilement
- on peut tester si le dino se répare avec une projection "moderne"
- on peut réutiliser cette architecture plus tard pour la 3D reconstruite

### État du code aujourd'hui

Le projet a déjà une base partielle pour ça :

- [src/gte/igte.h] contient déjà l'interface `IGte`
- [src/gte/gte_3d.h] implémente déjà `IGte` via `Gte3D`
- mais le backend principal [src/gte/gte.h] (`Gte`) n'est pas encore branché derrière `IGte`
- le CPU utilise encore un membre concret `gte_` dans [src/r3000/cpu.h]
- et appelle directement `gte_.execute(...)` dans [src/r3000/cpu.cpp]

Conclusion :
- l'architecture est **à moitié prête**
- il faut terminer le refactor pour rendre le **GTE principal interchangeable**

### Plan de refactor recommandé

1. **Conserver `Gte` comme backend fidèle**
   - aucune modification de logique PS1
   - uniquement un éventuel alignement avec `IGte`

2. **Introduire `GteModern` (ou `GteFloat`)**
   - implémentation séparée de `IGte`
   - calculs internes en float/double
   - conservation de coordonnées projetées non saturées pour usage moderne

3. **Brancher le CPU sur une interface**
   - remplacer l'usage "backend concret only" par une sélection de backend
   - permettre le choix du backend GTE principal par option

4. **Ajouter une option CLI + UE5**
   - ex: `--gte-backend=faithful|modern`
   - ex: `EGteBackend` ou bool dédié dans `PSXEmulatorComponent`

5. **Tester TREX en A/B**
   - backend fidèle actuel
   - backend moderne float
   - comparer spécifiquement :
     - trou dans la tête
     - triangles pointes
     - stabilité double-buffer / close-up

### Hypothèse à valider avec cette expérimentation

Si le dino s'affiche correctement avec le backend moderne float :
- ce n'est pas forcément que le GTE fidèle était faux
- c'est surtout que **notre pipeline moderne a besoin d'une représentation non saturée**

Si le bug reste identique :
- la cause est plus probablement ailleurs
- GPU / raster interpretation / winding / composant UE restent ouverts

### Décision de travail

Décision validée :

- **ne pas modifier le GTE fidèle actuel**
- **ajouter un backend alternatif expérimental**
- **le rendre sélectionnable à la volée**
- **utiliser TREX comme test principal**

### État d'implémentation (fait dans cette session)

Le refactor d'architecture est maintenant en place :

- `Gte` implémente désormais `IGte`
- le **GTE principal CPU** est maintenant sélectionnable par backend
- nouveau type de backend : `faithful | modern`

---

## Partie 8 — Stack corruption watchpoint (`STACKWATCH`)

### Objectif

Le front prioritaire n'est plus le clip/near, mais le crash CPU où `ra` finit à `0x00000001`
au retour de `FUN_8014e034`.

L'épilogue confirmé dans Ghidra est :

```asm
8014e1b8: lw ra,0x1c(sp)
8014e1bc: lw s0,0x18(sp)
8014e1c0: addiu sp,sp,0x20
8014e1c4: jr ra
```

Avec le `CrashTrace`, le `sp` utilisé par `lw ra,0x1c(sp)` est `0x801FFF30`.
Donc le slot `ra` à surveiller est :

- virtuel : `0x801FFF4C`
- physique : `0x001FFF4C`

### Instrumentation ajoutée

Un watchpoint dédié `STACKWATCH` a été ajouté côté core :

- indépendant du vieux `RAMWATCH`
- armé par défaut sur `0x001FFF4C..0x001FFF50`
- tag dédié : `STACKWATCH`
- log de :
  - type d'accès (`WR8/WR16/WR32`)
  - `pc`, `addr`, `phys`, `size`
  - `val_before`, `val_after`
  - hit de la valeur cible (`0x00000001`)

Le core mémorise aussi le **dernier writer** ayant touché ce mot.

### Dump auto au crash

Quand un `ifetch_adel_repeat` se produit, le CPU déclenche maintenant automatiquement :

- dump du voisinage stack :
  - `0x001FFF44`
  - `0x001FFF48`
  - `0x001FFF4C`
  - `0x001FFF50`
- dump du dernier writer vu sur le slot

### Contrôle hôte

UE5 :
- `bEnableStackWatch`
- `StackWatchPhys`
- `StackWatchSize`
- `bStackWatchTargetValueEnabled`
- `StackWatchTargetValue`

---

## Partie 9 — Mission Ghidra : clarification du crash stack (2026-04-14)

### Correction de diagnostic importante

Le `STACKWATCH` a bien confirmé que le mot lu comme `ra` à la fin de `FUN_8014e034`
vaut parfois `0x00000001`.

Mais l'interprétation précédente :

- "le writer `pc=0x80137290` est la vraie écriture métier fautive"

était trop rapide.

Après relecture Ghidra / désassemblage, les PCs vus dans `STACKWATCH` sont en réalité
principalement des **prologues de fonctions** :

- `0x801360D4` dans `FUN_801360c8`
- `0x80135E7C` dans `FUN_80135e70`
- `0x801359C8` dans `FUN_80135970`
- `0x80137290` dans `FUN_80137274`

Exemples :

```asm
801360c8: addiu sp,sp,-0x28
801360d4: sw    s1,0x14(sp)

80135e70: addiu sp,sp,-0x28
80135e7c: sw    s1,0x14(sp)

80135970: addiu sp,sp,-0x40
801359c8: sw    v1,0x2c(sp)

80137274: addiu sp,sp,-0x40
80137290: sw    s3,0x2c(sp)
```

Donc :

- le `1` vu à `0x80137290` n'est **pas** forcément le `*puVar3 = 1` du code métier
- c'est très probablement un **save de registre** sur la pile
- et le fait que ce save tombe sur `0x001FFF4C` signifie que ce mot est réutilisé comme
  slot de pile par plusieurs helpers du pipeline de rendu

### Ce que disent maintenant clairement les fonctions Ghidra

`FUN_8012673c` est la boucle runtime principale du dino. Pour chaque entrée de
`DAT_80163b30`, elle appelle :

1. `FUN_801360c8(...)`
2. `FUN_80135970(...)`
3. `FUN_80135e70(...)`
4. `FUN_80135940(...)`
5. `FUN_80137274(...)`

`FUN_801360c8` et `FUN_80135e70` :

- parcourent une chaîne de nœuds via le champ `+0x48`
- empilent les nœuds dans `DAT_80165fa0`
- recomposent les matrices monde / lumière
- écrivent les matrices dérivées en `+0x24..+0x40`
- puis marquent chaque nœud par :

```c
puVar3 = (undefined4 *)*piVar7;
*puVar3 = 1;
```

Ce `*puVar3 = 1` existe bien, mais ce n'est **pas** ce que montre directement le
`STACKWATCH` sur `0x80137290`.

`FUN_80137274` est un dispatcher de rendu PsyQ :

- il lit le descripteur objet préparé plus haut
- décode les codes GPU (`0x20`, `0x24`, `0x34`, etc.)
- appelle `GsPrst*` / helpers de prims pour fabriquer le rendu final

### Nouvelle lecture du crash

Le crash stack n'est plus à lire comme :

- "un pointeur de dirty flag écrit par erreur sur le slot de `ra`"

mais plutôt comme :

- "le mot `0x001FFF4C` est réutilisé comme slot de pile par plusieurs helpers"
- "à un moment, la pile n'est plus cohérente avec la frame qui croit encore posséder ce mot"

Autrement dit, le front prioritaire devient :

1. **comprendre pourquoi `FUN_8014e034` relit un mot de pile devenu stale**
2. déterminer s'il s'agit :
   - d'un `sp` incorrect / désaligné
   - d'une réentrance inattendue dans le chemin GPU wait/timeout
   - d'un retour via une profondeur de pile différente de celle de l'entrée

### Point clé sur `FUN_8014e034`

`FUN_8014e034` n'est pas une fonction gameplay quelconque :

- c'est la routine d'attente / timeout GPU
- elle est appelée depuis `FUN_8014de14`
- elle surveille la queue GPU / DMA / GPUSTAT

Donc si `ra` devient faux ici, le soupçon le plus fort n'est plus le GTE ou le clip :

- c'est un problème de **discipline de pile autour du chemin GPU wait**
- probablement pendant une phase de rendu déjà avancée

### Direction de reprise recommandée

La prochaine passe de debug doit viser :

- le **propriétaire logique** du mot `0x001FFF4C` au moment exact du crash
- pas seulement le dernier `pc` qui a écrit dessus

Concrètement :

1. instrumenter / tracer les variations de `sp` autour du callpath GPU wait
2. corréler `FUN_8014de14 -> FUN_8014e034` avec la boucle de rendu `FUN_8012673c`
3. vérifier si le chemin GPU wait peut être réentrant ou revenir avec une profondeur
   de pile incohérente

CLI :
- `--stack-watch=ADDR[:SIZE]`
- `--stack-watch-target=VALUE`
- `--no-stack-watch`

Par défaut, le mode debug courant reste :
- `STACKWATCH` actif
- cible = `0x00000001`

### But de la prochaine session

Trouver le **writer exact** qui écrase le slot sauvegardé de `ra`, puis le corréler
dans Ghidra pour trancher :

- store CPU normal mais mauvais pointeur
- overflow de buffer / copie
- write DMA inattendu

---

## Partie 4 — Règle critique UE5 runtime : snapshot de frame + pointeurs stales

### Nouveau diagnostic important

En relisant le composant UE5 2D avec l'hypothèse "le bug peut être côté composant", deux
risques structurels ont été identifiés.

Ils ne prouvent pas à eux seuls la cause du bug TREX, mais ils expliquent très bien :

- le côté **"aléatoire"**
- les **polys qui partent en pointe**
- les comportements différents entre **CLI** et **UE5**
- et le fait qu'un souci puisse réapparaître plus tard dans **GPU3D**

### Règle 1 — Ne jamais continuer avec un `Gpu*` déclaré stale

Dans `PSX2DRenderComponent`, le code détecte explicitement qu'un `Gpu_` n'a plus le
`magic` valide, log l'erreur, puis **continue quand même** à lire dedans.

Ça, c'est un anti-pattern de debug :

- soit le pointeur est valide
- soit il ne l'est plus et on doit **stopper net**

Continuer "pour voir" avec un `Gpu*` stale peut produire exactement :

- données vertex incohérentes
- triangles absurdes
- corruption visuelle "aléatoire"
- crashs ou quasi-crashs après Live Coding / réinit / rebinding raté

### Règle 2 — Le composant doit consommer un snapshot cohérent, jamais un mix live+copie

Le composant 2D fait correctement :

- `copy_ready_draw_list(DrawListCopy)`

Mais ensuite une partie de son calcul relit de l'état live GPU (`display_config()`)
au lieu de rester strictement sur le snapshot copié.

Donc il peut reconstruire un mesh avec :

- des vertices issus d'une frame N
- et une largeur/hauteur/display state issus d'une frame N+1

Même si ce n'est pas la cause unique du bug TREX, c'est une **violation de contrat**.

### Conséquence architecturale

Cette règle doit être considérée comme valable pour **tous** les composants runtime UE :

- `PSX2DRenderComponent`
- `PSX3DRenderComponent`
- tout futur composant de debug caméra / surface / viewer

Autrement dit :

1. **si un pointeur core/runtime est stale → on stoppe, on ne consomme pas**
2. **si on copie un snapshot de frame → tout le calcul de rendu doit dériver de CE snapshot**
3. **ne jamais mélanger "ready draw list copié" et "GPU live state" dans le même rebuild**

### Pourquoi c'est important pour GPU3D

Même si le bug observé aujourd'hui est côté `GPU2D`, on risque sinon de reproduire la même
famille de bugs côté `GPU3D` :

- pose caméra issue d'un frame copié
- mais résolution / display / scale / options relues en live
- ou pire : consommation d'un pointeur shadow GPU devenu stale après rebinding

Donc ce point n'est pas un détail local au 2D :

**c'est une règle de design runtime pour tout le bridge UE5.**

### Décision de debug

Pour les prochaines sessions :

- considérer le mode "continue malgré stale pointer" comme **interdit**
- considérer toute lecture live de `display_config`, `draw_env`, frame counters, etc.
  comme suspecte dès qu'un snapshot a déjà été copié
- appliquer la même discipline au chemin 3D avant de débugger plus loin les polys proches
- nouveau scaffold : `GteModern`
- CLI : `--gte-backend=faithful|modern`
- UE5 : propriété `GteBackendMode`

Validation CLI faite :
- `TREX.EXE` démarre bien avec `--gte-backend=modern`
- les `printf` remontent toujours
- le backend choisi est bien loggué côté CPU

Important :
- dans **cette première passe**, `GteModern` est encore un **scaffold fidèle**
- il ne fait **pas encore** les calculs float / non saturés
- le refactor sert à rendre l'expérimentation suivante simple et sûre

### Prochaine étape immédiate

Implémenter la vraie divergence de `GteModern` :

- conserver les commandes / registres compatibles
- calculer les projections internes en float/double
- stocker une représentation projetée moderne non saturée
- comparer TREX :
  - backend fidèle
  - backend moderne float

### Note sur les FLAG bits — ✅ CORRIGÉ (2026-04-14)

L'audit avait trouvé que 5 FLAG bits étaient aux mauvaises positions dans `gte.h:57-61`.

**Confirmation définitive via DuckStation** : le fichier source de référence
`duckstation/src/core/gte_types.h` (union `FLAGS`, lignes 36-41) utilise `BitField<u32, ...>`
et donne les positions exactes du hardware PS1 :

| Flag | DuckStation (`gte_types.h`) | Notre ancien code | Corrigé |
|---|---|---|---|
| `mac0_overflow` (pos) | **bit 16** | bit 13 ❌ | bit 16 ✅ |
| `mac0_underflow` (neg) | **bit 15** | bit 12 ❌ | bit 15 ✅ |
| `sx2_saturated` | **bit 14** | bit 16 ❌ | bit 14 ✅ |
| `sy2_saturated` | **bit 13** | bit 15 ❌ | bit 13 ✅ |
| `ir0_saturated` | **bit 12** | bit 14 ❌ | bit 12 ✅ |

Le masque `FLAG_ERROR_BITS = 0x7F87E000` (bits 30-23 + 18-13) était déjà correct — il couvre
l'ensemble de la plage, donc le calcul du bit 31 (error) n'était pas affecté.

**Impact** : pas la cause du bug TREX (FLAG = 0 pendant tout le run), mais corrige un vrai
écart hardware pour les jeux qui testent des bits FLAG individuels après RTPS (ex: test
`gte_stFLAG() < 0` dans les libs PsyQ — le bit 31 était OK, mais un jeu qui teste
`FLAG & (1<<14)` pour SX2 saturation aurait lu IR0 saturation à la place).

**Fichiers modifiés** :
- `src/gte/gte.h` — FLAG_SX2_SAT, FLAG_SY2_SAT, FLAG_IR0_SAT, FLAG_MAC0_OFLOW_POS/NEG
- `src/gte/gte_3d.h` — même correction (mirror du primary GTE)

---

## Fichiers modifiés dans cette session

### Changements permanents
- `src/gpu/gpu.h` — DrawVertex contract documented, clip_to_draw_area default OFF
- `src/gpu/gpu.cpp` — push_triangle: span check removed, offset never applied, policy comment
- `src/gpu/gte_correlation.h` — cleanup (removed geom_offset accessors)
- `src/gte/gte.h` — `force_geom_offset_zero_` flag + setter
- `src/gte/gte.cpp` — rtps_internal: OFX/OFY forced to 0 when flag set
- `src/gte/gte_3d.h` — same flag on shadow GTE
- `src/gte/gte_3d.cpp` — rtps_internal mirror + push_sxy saturation aligned with primary
- `src/emu/psx3d_profile_store.h` — Quirks substruct
- `src/emu/psx3d_profile_store.cpp` — QUIRK parse/serialize
- `src/emu/core.h` — psx_params_, psx3d_profile_data_, load_psx3d_profile_for_exe()
- `src/emu/core.cpp` — quirk propagation, psx_params injection, .EXE extension strip
- `src/r3000/cpu.h` — IFETCH ADEL repeat detector
- `src/r3000/cpu.cpp` — ADEL halt + CTC2 handler (cleanup)
- `cli/main.cpp` — --force-gte-geom-offset-zero, --psxparam, profile load for --load
- `integrations/ue5/.../PSXEmulatorComponent.h` — PsxParam, bForceGteGeomOffsetZero, Psx3dProfilePath doc
- `integrations/ue5/.../PSXEmulatorComponent.cpp` — PsxParam parsing, quirk propagation, Saved/PSXProfiles default
- `integrations/ue5/.../PSX2DRenderComponent.h` — bCenterDisplay removed
- `integrations/ue5/.../PSX2DRenderComponent.cpp` — pivot always centered, policy comment
- `integrations/ue5/.../PSXSurfaceComponent.cpp` — plane centered on both axes
- `profiles/psx3d/TREX.psx3dprof` — TREX profile with quirk

### Instrumentation temporaire (à nettoyer)
- `src/gte/gte.cpp` — GTE v3 tag, execute() call counting, AVSZ3 pre/post logging, DIV_OFLOW counter, NCLIP cull detection

---

## Commandes de test

### CLI
```bash
./lib/Release/r3000_emu.exe \
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" \
  --load="E:/Projects/PSX/roms/TREX.EXE" \
  --psxparam=1,30 \
  --timeout-ms=35000
```

### UE5
- `bDevKitMode = true`
- `ExePath = E:\Projects\PSX\roms\TREX.EXE`
- `PsxParam = 1,60`
- `bForceGteGeomOffsetZero = false` (auto via profile)
- Profile auto-loaded depuis `Saved/PSXProfiles/TREX.psx3dprof`

---

## Outil de debug crash CPU (2026-04-13)

Pour les crashes aléatoires type :

- `IFETCH fault kind=1 vaddr=0x00000001`
- `IFETCH ADEL repeated ... — halting CPU`

le core expose maintenant un **buffer circulaire optionnel d'états CPU complets**.

### Ce qui est capturé

Sur chaque instruction fetchée, si l'option est activée :

- `PC`
- `instr`
- `HI/LO`
- `COP0 Status/Cause/EPC/BadVAddr`
- **les 32 registres GPR**

### Déclenchement du dump

Le dump est automatiquement émis quand :

- un premier `ADEL/ADES` arrive
- un `IFETCH ADEL` se répète jusqu'au seuil fatal

### Activation

#### UE5
- propriété composant : `CpuCrashTraceSteps`
- `0` = désactivé
- valeur conseillée : `256` à `512`

#### CLI
- option : `--cpu-crash-trace=N`
- exemple :

```bash
./lib/Release/r3000_emu.exe \
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" \
  --load="E:/Projects/PSX/roms/TREX.EXE" \
  --psxparam=1,30 \
  --cpu-crash-trace=512 \
  --timeout-ms=35000
```

### But

Comparer un run "bon" et un run "mauvais" juste avant le crash pour voir :

- si le `PC` dérive avant le fault
- si un registre pointeur/retour (`ra`, `sp`, `a*`, `s*`, `t*`) diverge
- si le crash vient d'une vraie non-déterminisme runtime ou d'une corruption progressive

---

## Parité CLI ↔ UE et preuve DMA2 LL (2026-04-13)

### Parité de boot validée

- Le CLI dispose maintenant d'un vrai mode `--devkit` aligné sur le boot UE devkit :
  - BIOS mappé via `set_bios_copy(...)`
  - injection `PsxParam`
  - `fast_boot_from_exe(...)`
  - `text_hle`
- Conséquence :
  - le CLI affiche bien `Dino: Mode: interactive(0), timeout -1.`
  - et rejoint le même type de rendu GPU que sous UE

### Conséquence debug

- Le bug polygons / clipping n'est plus attribuable uniquement au composant UE.
- Le CLI reproduit désormais :
  - les primitives GPU extrêmes
  - les `RECT_16x16` avec coordonnées absurdes
  - les quads/triangles semi-transparents monstrueux
  - `DMA2 LL ... SAFETY`

### Instrumentation DMA2 LL

- Le walker DMA2 linked-list a maintenant une détection de cycle.
- Exemple capturé sur TREX :

```text
DMA2 LL cycle: start=0x16362C repeat=0x00000 last_hdr=0x00000000 next=0x00000 words=0 nodes=331 total_words=2106
DMA2 LL tail[0]: node=0x163408 hdr=0x00163404 next=0x163404 words=0
DMA2 LL tail[1]: node=0x163404 hdr=0x8018DF90 next=0x18DF90 words=128
DMA2 LL tail[2]: node=0x18DF90 hdr=0x09172DD0 next=0x172DD0 words=9
DMA2 LL tail[3]: node=0x172DD0 hdr=0x0902B748 next=0x2B748 words=9
DMA2 LL tail[4]: node=0x2B748 hdr=0xFEADFD0D next=0xDFD0D words=254
DMA2 LL tail[5]: node=0xDFD0D hdr=0x26FEF200 next=0x1EF200 words=38
DMA2 LL tail[6]: node=0x1EF200 hdr=0x00000000 next=0x00000 words=0
```

### Interprétation actuelle

- La linked-list ne fait pas juste "beaucoup de nœuds".
- Elle finit par suivre de faux headers dans de la donnée corrompue / non-header.
- Puis elle arrive sur une zone RAM nulle (`0x1EF200 -> 0x00000000`).
- Puis elle reboucle sur `0x000000`.

### Conclusion de debug actuelle

- Le couple :
  - coordonnées proches de `±1024`
  - triangles/quads monstrueux
  - `DMA2 LL ... SAFETY`
- est cohérent avec une linked-list GPU qui sort de sa chaîne valide.

La prochaine question à trancher n'est plus `UE ou core ?`, mais :

1. qui corrompt le pointeur `next`
2. ou quel packet est relu comme un faux node header
3. et si le problème vient de la construction guest de la liste ou de notre lecture DMA2

### Analyse Ghidra + dump mémoire des tail nodes (2026-04-13 soir)

- `0x80141D04` tombe dans une routine PsyQ standard décompilée comme `GsPrstTG3LFG(...)`.
- Cette routine :
  - fait le `RTPT/NCLIP/AVSZ3`
  - écrit le header packet à `a3` avec `0x09xxxxxx`
  - puis chaîne l'entrée OT via `sw v1, 0(t6)` à `0x80141D04`
- Donc :
  - `0x80141D04` n'est pas une corruption exotique
  - c'est le chemin standard Sony qui insère un packet dans l'ordering table

Conséquence importante :

- le `next` d'un header packet type `0x0902AB40` vient du contenu déjà présent dans l'OT au moment de l'insertion
- si ce `next` est mauvais, il faut regarder :
  - soit l'OT bucket déjà corrompu
  - soit un packet plus loin qui contient un header devenu invalide

#### Cas capturé en CLI avec dump mémoire des tail nodes

Exemple :

```text
DMA2 LL tail[2]: node=0x15E790 hdr=0x80173578
DMA2 LL mem[2]: node=0x15E790 w0=0x80173578 w1=0x0015E790 w2=0x0015E794 w3=0x0015E798
DMA2 LL tail[3]: node=0x173578 hdr=0x0902AB40
DMA2 LL mem[3]: node=0x173578 w0=0x0902AB40 ...
DMA2 LL tail[4]: node=0x2AB40 hdr=0x014F038B
DMA2 LL mem[4]: node=0x2AB40 w0=0x014F038B ...
DMA2 LL tail[5]: node=0xF038B hdr=0x11010111
```

Lecture actuelle :

- `0x15E790` ressemble à une entrée OT normale :
  - le premier mot pointe vers un packet `0x173578`
  - les mots suivants ressemblent encore à la chaîne OTC (`0x15E790`, `0x15E794`, ...)
- `0x173578` ressemble à un packet header standard PsyQ (`0x09xxxxxx`)
- le **premier saut vraiment douteux** est ensuite :
  - `next=0x02AB40`
  - puis encore pire `next=0x0F038B` (non aligné, clairement suspect)

Conclusion intermédiaire :

- la corruption visible ne semble pas naître au moment où `0x80141D04` insère dans l'OT
- elle apparaît plus loin dans la chaîne, quand un packet/header relu ensuite contient un `next` incohérent
- l'axe de debug le plus prometteur devient :
  - tracer le **writer PC** du mot header des nodes suspects (`0x173578`, `0x02AB40`, `0x0F038B`)
  - et vérifier si ces adresses appartiennent bien à la zone packet attendue pour le frame courant

#### Nouveau point acquis : premier faux header écrit par une routine CPU de copie

Sur un run CLI plus récent, on a capturé :

```text
DMA2 LL tail[0]: node=0x173820 hdr=0x0902AEF0
DMA2 LL mem[0]: node=0x173820 writer_pc=0x80141CF0 ...
DMA2 LL tail[1]: node=0x2AEF0 hdr=0x012200E1
DMA2 LL mem[1]: node=0x2AEF0 writer_pc=0x80126240 ...
```

Interprétation :

- `0x173820` est encore un packet PsyQ plausible, écrit par la routine OT/GTE standard autour de `0x80141CF0`.
- Le **premier header douteux** `0x012200E1` à `0x02AEF0` a en revanche été écrit par `writer_pc=0x80126240`.

Analyse Ghidra :

- `0x80126240` n'est pas un code OT/GPU dédié.
- Il est au milieu de `FUN_801261e0`, une routine de **copie générique par blocs de 8 octets** (`lwl/lwr/swl/swr`).
- `FUN_801261e0` est appelée depuis `FUN_80125e14`, qui orchestre ensuite `FUN_80126318(...)`.

Conclusion de travail :

- la chaîne DMA2 semble traverser une zone RAM qui a été remplie par une **copie CPU générique**, pas uniquement par les constructeurs de packets GPU attendus
- cela renforce fortement l'hypothèse :
  - soit d'un **chevauchement mémoire** entre OT/packets et un autre buffer
  - soit d'une **interprétation d'une zone copiée comme header DMA** alors qu'elle n'est pas censée être suivie comme node

### Instrumentation GTE ciblée (2026-04-13 nuit)

Pour distinguer proprement :

- `polygone extrême déjà né dans le GTE`
- vs `packet / DMA / GP0 devenu mauvais plus tard`

on a ajouté dans `src/gte/gte.cpp` un log diagnostic :

- tags : `GTE EXTREME_RTPT` / `GTE EXTREME_RTPS`
- condition :
  - au moins une coordonnée projetée `SXY` atteint environ `|900|` ou plus
  - et soit le bit signe final de `FLAG` reste à `0`
  - soit les bits de saturation `SXY` sont présents

Le log sort :

- `FLAG`
- `fatal` (bit signe de `FLAG`)
- `sxy_sat`
- `SXY0/1/2`
- `SZ1/2/3`
- `OFX/OFY`
- `H`

Objectif :

- vérifier si TREX produit déjà des vertices projetés extrêmes **avant** la construction du packet GPU
- et voir si ces cas passent quand même le filtre PsyQ basé sur `gte_stFLAG() < 0`

Cette instrumentation ne modifie pas le comportement du GTE.

### Nouveau dump automatique DMA GPU (2026-04-13 nuit)

On a ajouté un dump lourd mais ciblé pour arrêter de perdre les runs rares :

- tag log : `GPUDMA`
- fichier UE5 dédié : `gpu_dma.log`
- déclenchement automatique si :
  - `DMA2 LL` part en `SAFETY`
  - cycle détecté dans la linked-list
  - primitive GPU avec coordonnées extrêmes vue pendant un DMA2 LL
  - faute CPU grave (`ADEL/ADES/RI/...`) côté game code

Le dump contient :

- résumé de la linked-list
- chaque node DMA :
  - `addr`
  - `hdr`
  - `next`
  - `words`
  - `writer_pc`
- décodage séquentiel des mots GP0 :
  - début de commande (`CMD`)
  - type (`POLYGON`, `LINE`, `RECT`, etc.)
  - nombre de paramètres attendu
  - paramètres bruts (`PARAM`)
  - décodage XY heuristique avec marquage `EXTREME`

But :

- corréler directement "poly qui pète" ↔ contenu exact du packet DMA GPU
- et avoir aussi un dump exploitable quand le CPU crashe avant qu'on voie le frame complet

### Vérification DuckStation sur `PushSXY` / `STSXY3` (2026-04-14)

Point validé en recoupant :

- Ghidra (`GsPrstTG3LFG`) :
  - `RTPT`
  - `NCLIP`
  - `AVSZ3`
  - puis `stsxy3_gt3(a3)`
- DuckStation source (`src/core/gte.cpp`) :
  - `RTPS(...)`
  - `CheckMACOverflow<0>(Sx/Sy)`
  - `PushSXY(s32(Sx >> 16), s32(Sy >> 16))`

Conclusion importante :

- les coordonnées stockées dans les packets GT3 viennent bien **directement** du FIFO `SXY0/1/2` du GTE
- si un packet GPU contient des `Y ~= -1008/-1021`, la source peut déjà être :
  - la projection GTE elle-même
  - ou le code jeu/lib qui relit/repacke ces `SXY`
- ce n'est pas un artefact du composant UE

Écart matériel corrigé chez nous :

- notre `push_sxy()` écrivait :
  - `SXY0 <- SXY1`
  - `SXY1 <- SXY2`
  - `SXY2 <- val`
  - **et aussi `SXYP <- val`**
- DuckStation ne fait **pas** cette dernière écriture dans `PushSXY()`
- matériellement :
  - `SXYP`/reg15 est un **registre d'écriture** qui pousse la FIFO
  - et un **miroir de lecture** de `SXY2`
  - pas un quatrième registre interne indépendant à maintenir en parallèle

Correctif appliqué :

- suppression de l'écriture parasite de `D_SXYP` dans :
  - `src/gte/gte.cpp`
  - `src/gte/gte_3d.cpp`

Impact attendu :

- supprimer un état GTE impossible / non matériel
- éviter qu'un code qui lit/écrit reg15 au mauvais moment voie un état incohérent chez nous
- ce n'est pas encore la preuve que c'était **la** cause TREX, mais c'est un vrai écart hardware générique éliminé

### Instrumentation `RTPT -> PushSXY` enrichie (2026-04-14)

Le log `GTE EXTREME_RTPT / EXTREME_RTPS` existait déjà, mais il ne disait pas
encore **avec quelles entrées exactes** on arrivait à des `SXY` extrêmes.

On a ajouté une trace interne au `Gte` fidèle dans `src/gte/gte.cpp` :

- stockage temporaire par sommet dans `rtps_internal()`
- dump uniquement quand un `RTPT/RTPS` déclenche déjà `EXTREME_*`

Pour chaque sommet (`EXTREME_RTPT_V0/V1/V2`), on sort maintenant :

- vertex d'entrée `V=(x,y,z)`
- MAC bruts de transformation `(mac1_raw, mac2_raw, mac3_raw)`
- `IR1 / IR2 / IR3(z)`
- `SZ3`
- `H`
- quotient de perspective `q`
- accumulateurs `Sx/Sy` avant shift final
- `SXY` avant clamp `[-1024..1023]`
- `flag_before_push`
- bit `last`

Objectif :

- trancher si les coordonnées absurdes vues dans `gpu_dma.log` naissent déjà
  dans la projection GTE fidèle
- ou si elles se déforment seulement plus tard dans `stsxy3_gt3` / repack packet

Cette instrumentation est purement diagnostic :

- elle ne modifie pas le comportement
- elle est ciblée aux cas déjà extrêmes

### Corrélation `GPUDMA EXTREME` ↔ snapshot GTE (2026-04-14)

Pour arrêter de comparer des fragments à la main, on a enrichi la capture
des triangles extrêmes côté GPU :

- `gpu::Gpu::DmaExtremePrimitiveInfo` garde maintenant, quand disponible :
  - les XY finaux du triangle GPU
  - la corrélation GTE associée
  - `source_pc`
  - les 3 vertices 3D
  - `SZ`
  - la transform active
  - le flag `swap` si la corrélation passe par la permutation quad

Au moment d'un `GPUDMA EXTREME`, `bus.cpp` loggue maintenant aussi :

- `EXTREME_CORR ...`
- ou `EXTREME_CORR unavailable ...`

Donc on distingue enfin deux classes de cas :

- packet GPU extrême **avec** corrélation GTE disponible
- packet GPU extrême **sans** corrélation GTE exploitable

### Script d'analyse rapide (2026-04-14)

Nouveau script :

- `scripts/analyze_trex_extremes.py`

But :

- relire `system.log` (`EXTREME_RTPT / EXTREME_RTPT_V*`)
- relire `gpu_dma.log` (`EXTREME primitive / EXTREME_CORR`)
- rapprocher les events et sortir le meilleur match trouvé

Usage typique :

```bash
python scripts/analyze_trex_extremes.py
```

Ce n'est pas encore un debugger graphique DMA, mais c'est un premier comparateur
offline qui permet de voir rapidement :

- si un triangle extrême GPU a une corrélation GTE
- si ses `SXY` ressemblent à un `EXTREME_RTPT`
- ou si on est sur un cas "packet extrême sans match GTE", donc suspect ailleurs

---

## Partie 5 — FLAG bits corrigés (2026-04-14 nuit)

### Découverte

Vérification croisée directe contre `duckstation/src/core/gte_types.h` (union `FLAGS`).
DuckStation utilise des `BitField<u32, bool, bit, 1>` typés, ce qui donne les positions
**exactes et vérifiables** du hardware PS1 :

```cpp
// DuckStation gte_types.h — FLAGS union (lignes 36-41) :
BitField<u32, bool, 16, 1> mac0_overflow;   // bit 16
BitField<u32, bool, 15, 1> mac0_underflow;  // bit 15
BitField<u32, bool, 14, 1> sx2_saturated;   // bit 14
BitField<u32, bool, 13, 1> sy2_saturated;   // bit 13
BitField<u32, bool, 12, 1> ir0_saturated;   // bit 12
```

### Fix appliqué

```diff
 // src/gte/gte.h et src/gte/gte_3d.h :
-FLAG_SX2_SAT         = 1u << 16   →   FLAG_MAC0_OFLOW_POS = 1u << 16
-FLAG_SY2_SAT         = 1u << 15   →   FLAG_MAC0_OFLOW_NEG = 1u << 15
-FLAG_IR0_SAT         = 1u << 14   →   FLAG_SX2_SAT        = 1u << 14
-FLAG_MAC0_OFLOW_POS  = 1u << 13   →   FLAG_SY2_SAT        = 1u << 13
-FLAG_MAC0_OFLOW_NEG  = 1u << 12   →   FLAG_IR0_SAT        = 1u << 12
```

### Vérification

- Build Release : ✅ `r3000_emu.exe` compilé sans erreurs
- `FLAG_ERROR_BITS = 0x7F87E000` inchangé (couvre bits 30-23 + 18-13, identique DuckStation)
- Le calcul du bit 31 (error = OR de tous les error bits) reste correct
- Pas d'impact sur TREX (FLAG = 0 tout le run) mais correctif générique important

### Conséquence fonctionnelle

Avant le fix, un jeu PsyQ faisant :
```c
gte_ldv0(&v0);  // RTPS
gte_rtps();
if (gte_stFLAG() & (1 << 14))  // test SX2 saturation
    skip_polygon();
```
...aurait testé IR0 saturation au lieu de SX2 saturation chez nous.

De même, `check_mac_overflow(0, Sx)` dans `rtps_internal` mettait bit 13 au lieu de bit 16,
ce qui signifie que le flag MAC0 overflow était écrit dans le slot réservé à SY2 saturation.


---

## Partie 6 — Analyse des Corrélations GTE/DMA (2026-04-14)

### Résultats du script analyze_trex_extremes.py sur les logs PSXVR :

En exécutant le script sur system.log et gpu_dma.log d'une capture récente sous UE5, nous avons trouvé 5 primitives DMA extrêmes :

- DMA[1] / DMA[2] tri=[(0, 0), (1023, 0), (0, 511)] -> corr: unavailable (no GTE match)
- DMA[3] tri=[(12, -1016), (28, -1016), (12, -1000)] -> corr: unavailable (no GTE match)
- DMA[4] tri=[(-992, 20), (-976, 20), (-992, 36)] -> corr: unavailable (no GTE match)

- DMA[5] tri=[(-18, 33), (935, -491), (-12, 34)] -> distance=0 (PERFECT MATCH)
  [GTE] EXTREME_RTPT_V1 #3 in=(-31144,-32655,-6266) mac=(127628112...) sxy_preclamp=(935,-491)

### Conclusion :

1. La majorité du garbage visuel NE VIENT PAS du GTE ! Les primitives géantes arrivent directement dans le GPU en provenance de la file DMA (par exemple à l'adresse en RAM 0x159048 ou 0x16362C). Cela confirme à 100% que la linked-list DMA2 traverse de la mémoire corrompue.
2. Un vrai glitch GTE se cache dans le lot : La primitive DMA[5] trouve son origine dans l'envoi d'un sommet monstrueux (Z=-6266) par l'engine MIPS (défaut possible de near-clipping).

Prochaine étape de debug : Utiliser le writer PC tracker pour surveiller l'écriture aux adresses corrompues 0x159048 et 0x16362C.

---

## Partie 7 — Découverte du Crash CPU (Stack Corruption) (2026-04-14)

### Le faux-positif de la corruption mémoire
Nous avons réalisé que les primitives `(0, 0)(1023, 0)(0, 511)` observées dans la DMA ne sont **PAS** une altération aléatoire de RAM ou de liste DMA. Ce sont des opérations 2D parfaitement valides du moteur PS1 (VRAM Clear) de 1024x512. L'intégration de UE5 les perçoit à tort comme de la géométrie 3D, mais la mémoire de la Linked List n'est pas "corrompue" pour le rendu. 

### Le vrai crash : Débordement sur le Stack (0x1FFF50)
L'analyse de la toute fin du fichier `system.log` révèle la raison exacte des blocages (freezes/crashes) de l'émulateur. 
Le log affiche une boucle `ifetch_adel_repeat` (Fetch Exception) lors de l'exécution de `jr ra` :
```
[CPU] CrashTrace dump: reason=ifetch_adel_repeat entries=512
[CPU] CrashTrace[510] pc=0x8014E1C4 instr=0x03E00008 (jr ra) epc=0x8014E1BC
[CPU] CrashTrace[510] sp=0x801FFF50 fp=0x8015B600 ra=0x00000001
```

La mémoire physique au sommet du Stack (pointé par `$sp = 0x1FFF50`) s'est fait écraser par des données corrompues valant `1`. Lors de l'épilogue d'une fonction, le CPU recharge son `$ra`, obtient la valeur absurde `1`, et la console crash instantanément en tentant de sauter à l'adresse 1.

### Next Steps validés pour la prochaine session : Le Piège à l'Offset
Puisque le registre `$ra` est rechargé depuis un offset du stack pointé par `$sp`, un simple watchpoint aveugle sur l'adresse de base du pointeur ne suffit pas.

La désassemblage Ghidra du vrai épilogue de `FUN_8014e034` montre :

```asm
8014e034: addiu sp,sp,-0x20
8014e03c: sw    ra,0x1c(sp)
...
8014e1b8: lw    ra,0x1c(sp)
8014e1bc: lw    s0,0x18(sp)
8014e1c0: addiu sp,sp,0x20
8014e1c4: jr    ra
```

Donc le slot de retour correct est bien **`0x1c(sp)`**, pas `0x18(sp)`.

L'objectif immédiat pour la reprise de l'investigation est :
1. **Ouvrir Ghidra** ou le dump de l'EXE.
2. Regarder l'instruction juste avant le crash : l'épilogue de la fonction finissant en `0x8014E1BC`.
3. Relever l'instruction exacte de récupération du registre de retour (`lw ra, 0x1c(sp)`).
4. Attention : sur le `CrashTrace`, le `sp` vu au `jr ra` est **déjà restauré** (`addiu sp,sp,0x20` a déjà tourné).  
   Donc si le trace affiche `sp=0x801FFF50` au `jr ra`, le `sp` utilisé par `lw ra,0x1c(sp)` juste avant était `0x801FFF30`.
5. Calculer l'adresse du slot corrompu exact : `0x801FFF30 + 0x1c = 0x801FFF4C`, soit **physique `0x001FFF4C`**.
6. Placer un watchpoint strict dans le bus mémoire sur `0x001FFF4C` (éventuellement une petite plage autour si besoin), idéalement en filtrant la valeur `0x00000001`.
Cela nous livrera instantanément le Writer PC exact du code ou du DMA fautif qui écrase la pile CPU.

---

## Partie 9 — Analyse code-only du front CPU↔GPU wait (2026-04-14)

### Changement de diagnostic

Après lecture croisée du core émulateur et du code TREX sous Ghidra, la piste la
plus forte n'est plus un simple "write mémoire aléatoire", mais un possible
écart de **modèle de synchronisation CPU↔GPU** chez nous.

Important :

- cette partie est une **analyse de code**, pas encore une preuve runtime finale
- aucune modification comportementale ne doit être faite sur cette base sans en
  discuter
- l'objectif est de figer le diagnostic courant avant toute nouvelle passe

### Ce que TREX attend réellement

La routine `FUN_8014de14` est maintenant clairement identifiée comme un chemin
de **wait GPU / wait draw**.

Décompilation Ghidra résumée :

- si la file logicielle GPU n'est pas vide, elle la draine
- elle attend ensuite que `DMA_GPU_CHCR.bit24 == 0`
- elle attend ensuite que `GPUSTAT bit 26 == 1`
- en cas d'attente trop longue, elle appelle `FUN_8014e034`

La routine `FUN_8014e034` est bien un helper de timeout GPU :

- elle appelle `FUN_8014f48c(-1)`
- elle loggue `GPU timeout:que=%d,stat=%08x,chcr=%08x,madr=%08x`
- elle reset la file GPU / le canal DMA GPU / certains registres GPU
- son épilogue est bien celui qui finit par faire `jr ra`

Autrement dit :

- le crash stack continue d'apparaître **dans un vrai chemin wait/timeout GPU**
- ce n'est pas un endroit arbitraire du programme

### Ce que fait notre core aujourd'hui

Lecture de `src/r3000/bus.cpp` :

- DMA2 GPU est déclenché directement dans `Bus::write_u32(...)` quand on écrit
  `CHCR`
- pour un transfert CPU→GPU :
  - on lit les mots depuis la RAM
  - on appelle immédiatement `gpu_->mmio_write32(kGpuBase, w)` pour chaque mot
  - puis on appelle immédiatement `dma_finish(2)`

Point clé :

- `dma_finish(2)` efface aussitôt `CHCR.bit24`
- donc chez nous, **DMA2 n'est plus busy dès que la copie CPU→GPU est terminée**

Lecture de `src/gpu/gpu.cpp` :

- `GPUSTAT` bits `26/28/25` sont reconstruits dynamiquement
- `ready_cmd = (gp0_state_ == idle)`
- `ready_dma = ready_cmd`

Donc chez nous :

- `GPUSTAT bit26/28/25` reflètent surtout "le parseur GP0 est idle"
- pas nécessairement "le GPU PS1 a réellement fini son travail logique"

### Asymétrie importante de notre modèle

Sous UE5, le threading externe porte surtout sur :

- VBlank
- HBlank / Timer 1
- draw list swap via `tick_vblank_swap_only()`

Mais **DMA2 GPU lui-même reste synchrone côté CPU**.

Donc notre modèle actuel ressemble à ceci :

- raster / VBlank : asynchrones
- soumission DMA2 GPU : synchrone
- fin DMA2 : déclarée immédiatement après copie des mots GP0
- `GPUSTAT ready` : basé sur l'idle du parseur GP0

Cette asymétrie est maintenant un suspect sérieux.

### Hypothèse forte actuelle

TREX peut très bien attendre une sémantique matérielle du style :

- "DMA GPU réellement terminé"
- puis "GPU réellement prêt / draw terminé"

alors que chez nous on fournit quelque chose de plus léger :

- "les mots ont été copiés vers le GPU"
- "le parseur GP0 est idle"

Si cette lecture est correcte, alors :

- le jeu peut croire que le GPU a fini trop tôt
- réutiliser trop tôt un buffer / OT / paquet
- et finir par tomber plus tard dans un wait timeout ou un crash de pile

### Ce que cette analyse n'affirme PAS encore

Cette partie ne prouve pas encore :

- que le bug vient uniquement de là
- que le thread model UE5 doit être refactoré
- que le DMA GPU doit forcément devenir un vrai thread séparé

Elle dit seulement :

- notre sémantique `DMA2 busy / GPUSTAT ready` est probablement trop simple
- et c'est actuellement la première explication générique crédible du chemin
  `wait draw` observé dans TREX

### Direction recommandée pour la suite

Avant toute modification comportementale :

1. instrumenter proprement le protocole `wait draw`
2. comparer :
   - `DMA2.CHCR.bit24`
   - `GPUSTAT bit26`
   - état de file GPU
   - `gp0_state_`
3. vérifier si TREX sort du wait alors que notre core n'a fourni qu'un
   "pseudo-ready" plus faible que le hardware

Règle de prudence :

- **ne pas modifier le système de thread ni la sémantique DMA/GPU sur la seule
  base de cette lecture**
- confirmer d'abord par instrumentation ciblée

---

## Partie 10 — Expérimentation "DMA2 busy + GPU ready différé" (2026-04-14)

### But

Tester l'hypothèse suivante sans introduire un nouveau thread DMA :

- `DMA2.CHCR.bit24` ne doit pas tomber immédiatement après la simple copie CPU→GPU
- `GPUSTAT ready` ne doit pas dépendre uniquement de `gp0_state_ == idle`

### Implémentation expérimentale

Le core a reçu un modèle minimal et déterministe :

- `src/r3000/bus.cpp`
  - DMA2 ne fait plus `dma_finish(2)` immédiatement
  - on calcule un délai minimal basé sur le nombre de mots soumis
  - `dma_finish(2)` n'arrive qu'après décrément de ce délai dans `tick()/tick_peripherals()`
- `src/gpu/gpu.cpp`
  - `Gpu::notify_dma_submit(words, linked_list)`
  - `Gpu::tick_timing(cycles)`
  - `GPUSTAT bit26/28/25` sont maintenant retardés par `dma_busy_cycles_`

Philosophie :

- pas de thread DMA supplémentaire
- pas de refactor massif
- juste un état "encore busy" suffisamment crédible pour tester les chemins
  `wait draw`

### Résultat du test CLI immédiat

Le build CLI passe bien, mais le run TREX CLI continue de retomber dans le vieux
chemin :

- `ISO panic`
- `PANIC after ReadTOC fail`
- boucle BIOS avec `I_STAT=0x0001 I_MASK=0x0000`

Donc ce test CLI ne tranche pas encore l'effet de cette expérimentation sur le
front de crash observé sous UE5.

### Conclusion provisoire

Cette modification :

- reste cohérente avec l'hypothèse matérielle actuelle
- est volontairement limitée
- **n'est pas encore validée** comme correction du bug TREX

Le prochain verdict utile devra venir d'un run UE5 reproduisant le vrai chemin
de rendu/crach TREX.

---

## Partie 11 — Analyse Ghidra de la chaîne objet -> packets -> dispatcher (2026-04-14)

### Ce qu'on peut maintenant affirmer

Le groupe de fonctions vu par `STACKWATCH` :

- `FUN_801360c8`
- `FUN_80135970`
- `FUN_80135e70`
- `FUN_80135940`
- `FUN_80137274`

ne ressemble pas à une écriture sauvage isolée. Il ressemble à une chaîne
normale de préparation de matrices puis de dispatch de packets GPU pour chaque
objet du dino.

### Boucle principale dans `FUN_8012673c`

Pour chaque entrée de `DAT_80163b30`, TREX fait :

1. `FUN_801360c8(entry[1], scratch_matrix)`
2. `FUN_80135970(scratch_matrix)`
3. `FUN_80135e70(entry[1], scratch_matrix)`
4. `FUN_80135940(scratch_matrix)`
5. `FUN_80137274(entry, ordering_table, 2, 0x1f800000)`

Donc :

- `entry + 0x4` = pointeur objet/nœud
- `entry + 0x0` = mot de contrôle/flags pour le dispatcher
- `entry + 0x8` = description source des packets
- `entry + 0xC` = tête de la linked-list de packets préparée juste avant

Taille d'une entrée :

- `0x14` octets (`5` mots)

### Initialisation de `DAT_80163b30`

`FUN_80127a50` construit cette table en deux passes :

1. `FUN_80136998(&DAT_8001800c, entry, index)`
   - écrit `entry[2] = base + index * 0x1c`
   - met `entry[0] = 0`
2. `entry[1] = &DAT_8015b5e0`
   puis `FUN_801369b4(entry, packet_pool_ptr)`
   - écrit `entry[3] = head_of_packet_list`
   - retourne le nouveau pointeur de fin dans le pool

Autrement dit :

- `FUN_80136998` prépare la description source
- `FUN_801369b4` fabrique la liste de packets GPU chaînée
- `FUN_80137274` ne crée pas les packets, il les consomme/dispatche

### Nature de `FUN_80137274`

`FUN_80137274` est un dispatcher de primitives GPU :

- lit `param_1[2]` pour récupérer la description source
- lit `param_1[3]` comme tête de linked-list
- boucle sur `puVar5 = (uint *)(*puVar5 & 0xffffff)`
- dispatch sur les opcodes GPU :
  - `0x20/0x21`
  - `0x24/0x25`
  - `0x28/0x29`
  - `0x2c/0x2d`
  - `0x30/0x31`
  - `0x34/0x35`
  - `0x38/0x39`
  - `0x3c/0x3d`

et appelle des helpers du style :

- `GsPrstNF3`
- `GsPrstNG3`
- `GsPrstTNF3`
- `GsPrstTNG3`
- `FUN_80138aac`
- `FUN_80138d9c`

Conclusion importante :

- `FUN_80137274` ressemble à un consommateur normal de packets
- le hit `STACKWATCH` à `0x80137290` est dans son **prologue**
  (`sw s3,0x2c(sp)`)
- donc il ne faut plus interpréter ce PC comme "la" ligne métier qui corrompt
  tout à elle seule

### Nature de `FUN_801360c8` et `FUN_80135e70`

Ces deux fonctions :

- remontent une hiérarchie de nœuds via `node[0x12]`
- reconstruisent une matrice monde/transformation dans un scratch local
- repropagent les matrices calculées dans les nœuds (`+0x24..+0x40`)
- marquent les nœuds visités avec `*node = 1`

Le motif est quasiment identique dans les deux fonctions.

Donc :

- l'écriture de `1` vue dans les sessions précédentes existe bien dans ce
  pipeline
- mais `STACKWATCH` à `0x001FFF4C` attrape souvent des **saves de pile** sur le
  même mot, pas uniquement ce dirty flag métier

### Ce que cette lecture change

On a maintenant une lecture plus propre :

- le bug ne ressemble pas à "un seul store absurde vers le slot de `ra`"
- il ressemble davantage à un problème de contrat plus large dans la chaîne :
  - structure objet/nœud
  - linked-list de packets
  - ou réutilisation/réentrance de contexte au moment du rendu

### Prochaine cible recommandée

La prochaine analyse Ghidra utile n'est plus `FUN_80137274` seule, mais :

1. les structures pointées par `entry[1]`, `entry[2]`, `entry[3]`
2. le layout réel du bloc source à partir de `DAT_8001800c`
3. la façon dont `FUN_801369b4` repacke ces packets avant dispatch

Autrement dit :

- `FUN_80137274` paraît surtout être la fin normale du pipeline
- la vraie dérive est probablement avant, dans la préparation de la structure
  ou dans la liste de packets qu'il consomme

---

## Partie 12 — Piste forte sur les copies packées `LWL/LWR/SWL/SWR` (2026-04-14)

### Fait nouveau important

Les routines de copie/relocalisation :

- `FUN_801260a0`
- `FUN_801261e0`

ne sont pas des copies "C" banales. Le désassemblage montre qu'elles utilisent
explicitement les instructions MIPS non alignées :

- `LWL`
- `LWR`
- `SWL`
- `SWR`

Exemple dans `FUN_801261e0` :

- `0x8012622c : lwl v0, 0x3(a2)`
- `0x80126230 : lwr v0, 0x0(a2)`
- `0x8012623c : swl v0, 0x3(a3)`
- `0x80126240 : swr v0, 0x0(a3)`

### Pourquoi c'est très intéressant

On avait déjà vu `0x80126240` apparaître comme `writer_pc` dans les sessions
précédentes. Cette adresse n'est pas un store métier "bizarre" :

- c'est le `SWR` d'une routine de bulk-copy/relocalisation

Donc certains writers qui semblaient suspects dans les logs sont en réalité :

- des écritures normales de copie packée
- très sensibles à la fidélité de l'émulation des opcodes non alignés

### Nouvelle hypothèse générique crédible

Si notre implémentation de :

- `LWL`
- `LWR`
- `SWL`
- `SWR`

est légèrement fausse, alors on peut corrompre :

- les structures relogées depuis `DAT_80018000`
- les packets GPU préparés plus tard
- puis obtenir plus loin :
  - des coordonnées extrêmes
  - des drawlists qui explosent
  - et éventuellement le crash CPU final

Cette hypothèse est intéressante parce qu'elle est :

- **générique**
- **compatible avec un bug qui marche "presque" tout le temps**
- et beaucoup plus "émulateur" que "jeu cassé"

### Ce qu'on sait déjà

Le pipeline de données passe bien par ces routines :

1. `FUN_80125d6c` copie le bloc source vers `DAT_801527a0`
2. `FUN_80125d9c` appelle :
   - `FUN_801260a0(..., 0x80060000)`
   - `FUN_80125f60(..., DAT_801527b0)`
3. `FUN_80127a50` construit `DAT_80163b30`
4. `FUN_801369b4` fabrique les packet lists
5. `FUN_80137274` les dispatche

Donc une erreur sur les copies packées peut polluer tout le pipeline très tôt.

### Point de prudence

Cette partie **ne prouve pas encore** que nos opcodes `LWL/LWR/SWL/SWR` sont
faux.

Elle prouve seulement que :

- TREX dépend fortement de ces opcodes dans les copies critiques
- et que plusieurs `writer_pc` observés précédemment correspondent
  précisément à ces instructions

### Suite recommandée

Avant de retoucher quoi que ce soit :

1. relire soigneusement l'implémentation émulateur de :
   - `LWL`
   - `LWR`
   - `SWL`
   - `SWR`
2. comparer les formules et la sémantique de merge/load-delay avec une
   référence fiable
3. seulement ensuite décider si cette piste mérite une instrumentation ou un fix

### Observation critique sur notre pipeline `pending_load_`

Dans notre core actuel :

- l'instruction courante produit `next_pending_load`
- en fin de step, on fait :
  - `commit_pending_load()` sur le load **précédent**
  - puis `pending_load_ = next_pending_load`

Donc pour la séquence réelle vue dans TREX :

1. `LWL v0, 3(a2)`
2. `LWR v0, 0(a2)`
3. `SWL v0, 3(a3)`
4. `SWR v0, 0(a3)`

on obtient actuellement :

- `LWR` fusionne correctement avec le `pending_load_` de `LWL`
- **mais**
- au début de `SWL`, le résultat final de `LWR` n'est pas encore visible dans
  `gpr_[v0]`
- `SWL` lit donc encore la valeur partielle déjà commitée de `LWL`

Autrement dit :

- nos formules `LWL/LWR/SWL/SWR` peuvent être justes
- mais notre **timing de visibilité** du load delay peut tout de même casser les
  copies packées

Cette observation est très forte, parce qu'elle colle exactement au pattern
machine utilisé par TREX dans `FUN_801260a0` et `FUN_801261e0`.

### Implication

Si cette lecture est correcte, alors :

- les structures relogées peuvent être corrompues très tôt
- sans qu'on voie immédiatement un crash
- puis la corruption remonte plus tard dans :
  - les packets GPU
  - les coordonnées extrêmes
  - et possiblement le crash CPU final

### Priorité de validation

C'est maintenant l'une des premières hypothèses à vérifier avant :

- tout refactor de thread DMA
- tout changement de modèle GPU wait
- toute théorie spécifique au jeu

### Vérification DuckStation

La comparaison avec DuckStation sur `src/core/cpu_core.cpp` montre que :

- `LWL/LWR` lisent bien la valeur delayed en attente pour `rt`
- puis programment à leur tour un nouveau delayed load
- `SWL/SWR`, eux, lisent simplement `ReadReg(rt)`
- et `UpdateLoadDelay()` se fait après l'exécution de l'instruction courante

Donc, sur la séquence :

1. `LWL`
2. `LWR`
3. `SWL`
4. `SWR`

DuckStation **ne** rend pas visible le résultat delayed final de `LWR` à
`SWL` immédiatement.

Conclusion :

- la sous-hypothèse "notre `pending_load_` est trop en retard d'un cycle par
  rapport à DuckStation" **n'est pas confirmée**
- il ne faut pas patcher ce point à l'aveugle

La piste `LWL/LWR/SWL/SWR` reste intéressante côté format de données et copies
packées, mais **pas encore** comme divergence claire vis-à-vis de DuckStation.

---

## Partie 13 — Carte plus claire du bloc source et des records de `0x1c` (2026-04-14)

### Ce qu'on sait maintenant sur le pipeline de données

Le pipeline TREX n'est pas "objet -> GTE -> GPU" en direct. Il passe par un
bloc source relogé et une table d'entrées intermédiaires :

1. `DAT_80018000` : bloc source initial
2. `FUN_80125d6c` / `FUN_80125e94` : copie vers `DAT_801527a0`
3. `FUN_80125d9c` :
   - `FUN_801260a0(..., 0x80060000)` : copie packée/relogée
   - `FUN_80125f60(..., DAT_801527b0)` : table auxiliaire pour `gteMIMefunc`
4. `FUN_80127a50` :
   - `FUN_80135d14(&DAT_80018004)` : fixup des offsets internes
   - `FUN_80136998` : prépare `entry[2]`
   - `FUN_801369b4` : construit `entry[3]`
5. `FUN_8012673c` consomme `DAT_80163b30`
6. `FUN_80137274` dispatche les packets GPU

### Format du bloc à records de `0x1c`

`FUN_80135d14` montre un format très propre :

- mot `+0x0` du bloc : flags (`bit0 = déjà fixupé`)
- mot `+0x4` : nombre d'entrées
- mot `+0x8` : début du tableau d'entrées

Puis pour chaque entrée de `0x1c` octets, `FUN_80135d14` fait :

- fixup `entry + 0x00`
- fixup `entry + 0x08`
- fixup `entry + 0x10`

en ajoutant la base du tableau (`a0` après `addiu a0,a0,0x4`).

Donc chaque record contient **au moins trois offsets relatifs** convertis en
pointeurs absolus.

### Champ critique déjà identifié

`FUN_801369b4` prend `entry[2]` (source record) et lit immédiatement :

- `*(record + 0x10)` : pointeur de début de données/packets source
- `*(record + 0x14)` : nombre d'éléments

Donc :

- le fixup de `+0x10` est déjà dans le chemin critique de fabrication des
  packets
- si ce champ est faux, `FUN_801369b4` part directement sur une mauvaise base

### Ce que font les autres helpers autour

- `FUN_801360c8` / `FUN_80135e70`
  - remontent une hiérarchie de nœuds
  - propagent des matrices monde dans les champs `+0x24..+0x40`
  - marquent les nœuds visités avec `*node = 1`
- `FUN_80135970`
  - pousse la matrice de lumière
- `FUN_80135940`
  - fait `SetRotMatrix` / `SetTransMatrix`
- `FUN_8013638c`
  - prépare une matrice de caméra/repère global
  - peut utiliser `param_1[7]` comme pointeur vers un nœud auxiliaire

### Lecture actuelle

Le dispatcher final n'est probablement pas le bon suspect.

Le cœur du pipeline ressemble plutôt à :

- records source de `0x1c`
- trois offsets relatifs fixupés
- fabrication d'une linked-list de packets
- dispatch final

Donc la vraie dérive peut encore être dans :

1. le contenu source du record de `0x1c`
2. le fixup de ses offsets
3. la consommation/repack dans `FUN_801369b4`

### Conséquence pratique

La prochaine meilleure analyse n'est plus :

- "pourquoi `FUN_80137274` casse"

mais :

- "est-ce que le record `0x1c` pointé par `entry[2]` est encore cohérent juste
  avant `FUN_801369b4` ?"

---

## Partie 14 — Divergence `GPUSTAT` confirmée contre DuckStation (2026-04-14)

### Point confirmé

La comparaison avec DuckStation montre une divergence nette dans la construction
de `GPUSTAT` :

- DuckStation :
  - `bit25` = `dma_data_request`
  - `bit26` = `gpu_idle`
  - `bit27` = `ready_to_send_vram`
  - `bit28` = `ready_to_receive_dma`
- Chez nous, avant correction :
  - `bit26` et `bit28` étaient dérivés du même état simplifié
  - `bit25` dépendait aussi de cette même fusion

### Pourquoi c'est important

Le code Sony/TREX ne lit pas ces bits comme une seule notion de "ready" :

- `FUN_8014d998` attend `GPUSTAT & 0x10000000` (`bit28`)
- `FUN_8014de14` attend ensuite `GPUSTAT & 0x04000000` (`bit26`)

Donc le runtime distingue bien :

1. "le GPU peut accepter/exécuter de la queue"
2. "le GPU est réellement idle/terminé"

Fusionner ces deux états est un écart plausible pour le couloir :

- queue GPU
- wait draw
- timeout helper
- retour/IRQ
- crash CPU tardif

### Correction appliquée

Le core GPU a été refactoré pour séparer explicitement les états dynamiques :

- `gpu_idle`
- `ready_to_send_vram`
- `ready_to_receive_dma`
- `dma_request`

et `GPUSTAT` est maintenant reconstruit à partir de ces helpers, au lieu de
recycler un unique `ready_cmd`.

### Modèle choisi côté core

Sans FIFO matériel complet, le modèle reste simplifié mais respecte mieux la
sémantique hardware :

- `bit26 = gpu_idle`
  - vrai seulement si :
    - `gp0_state == idle`
    - pas de `dma_busy_cycles`
    - pas de `vram_to_cpu_active`
- `bit27 = ready_to_send_vram`
  - vrai seulement pendant un `GPUREAD`/`VRAM->CPU`
- `bit28 = ready_to_receive_dma`
  - vrai tant qu'on n'est pas dans la phase inverse `VRAM->CPU`
  - ne doit plus être fusionné avec `gpu_idle`
- `bit25 = dma_request`
  - dérivé de la direction DMA + des deux flags ci-dessus

### Lecture actuelle

Ce patch ne prouve pas encore qu'on a la cause racine du crash, mais il retire
une divergence claire avec DuckStation exactement dans le protocole
`queue/wait draw` que TREX utilise.

### Test réel post-patch

Un vrai run CLI TREX (`--load + --devkit + --psxparam`) a été refait après ce
patch.

Résultat :

- le boot reste bon
- le patch ne casse pas le démarrage
- mais le crash CPU / timeout GPU existe encore

Signal clé observé dans `logs/io.log` :

```text
GPU timeout:que=0,stat=d216208c,chcr=401,madr=8015f62c
```

Décodage utile de `GPUSTAT=0xD216208C` :

- `bit28 ready_to_receive_dma = 1`
- `bit25 dma_request = 1`
- `bit26 gpu_idle = 0`
- `dma_dir = 2`

Donc au moment du timeout :

- la queue logicielle GPU est vide (`que=0`)
- DMA2 n'est plus busy (`CHCR=0x401`)
- le GPU dit encore "je peux recevoir du DMA"
- mais il ne dit pas "je suis idle"

Conclusion mise à jour :

- la séparation `bit26/bit28` était nécessaire
- mais elle **ne suffit pas**
- la prochaine vraie cible est maintenant la **définition de `gpu_idle`**
- notre modèle de `bit26` reste probablement trop pauvre par rapport à
  DuckStation/hardware

---

## Commandes CLI utiles — TREX (2026-04-15)

### Commande de base correcte

Pour TREX, la commande CLI correcte est bien :

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --load="E:/Projects/PSX/roms/TREX.EXE" `
  --devkit `
  --psxparam=1,30 `
  --timeout-ms=15000
```

Points importants :

- `--load` seul n'est pas le bon chemin de parité pour TREX
- il faut `--devkit`
- `--psxparam=1,30` est le couple utile pour le dino
- `--devkit` ne doit pas forcer le HLE : le non-HLE reste la cible par défaut

### Correction CLI importante

Un bug de la CLI forçait auparavant `hle_vectors=1` quand `--devkit` était
actif :

```cpp
core_opt.hle_vectors = (devkit_mode ? 1 : 0) | (has_flag(argc, argv, "--hle") ? 1 : 0);
```

Cela polluait les runs TREX avec des boucles du type :

- `HLE UNHANDLED A(0x08)`
- `HLE UNHANDLED B(0x16)`

Le comportement correct est maintenant :

- `--devkit` = chemin de boot devkit/UE
- `--hle` = HLE explicite seulement
- sans `--hle`, un run `--devkit` reste non-HLE

### Correction core importante (2026-04-15 nuit)

Ce n'était pas encore suffisant :

- même après le fix CLI ci-dessus, `Core::fast_boot_from_exe()` re-forçait
  encore `cpu_->set_hle_vectors(1)`
- donc un run `--load + --devkit` pouvait rester contaminé par du HLE
  **après** l'init, même si la CLI avait bien demandé `hle_vectors=0`

Le fix correct a été fait dans le core :

- `fast_boot_from_exe()` respecte maintenant le mode choisi à l'init
- `fast_boot_from_cd()` aussi
- le core mémorise le couple :
  - `init_hle_vectors_`
  - `init_text_hle_`
- puis les réapplique au lieu de forcer HLE en douce

Conséquence pratique :

- les vieux logs `HLE UNHANDLED ...` / `HLE EXC VEC ...` sur les runs TREX
  devkit ne doivent plus être utilisés comme vérité si leur date est
  antérieure à ce correctif
- un vrai run CLI non-HLE TREX doit maintenant montrer :
  - `imask=0x0009`
  - plus de `HLE UNHANDLED`
  - plus de `HLE EXC VEC`

Résultat runtime observé juste après le fix :

- le vieux front `GPU timeout + HLE UNHANDLED B(0x16)` a disparu
- le run direct non-HLE reste vivant plus longtemps
- les `SR_TRACE` montrent un PC qui tourne en RAM jeu
  (`0x8000xxxx..0x80017xxx`) au lieu de retomber tout de suite dans le faux
  chemin HLE

Conclusion mise à jour :

- les anciens tests CLI devkit étaient encore pollués
- la comparaison UE5 ↔ CLI doit maintenant repartir uniquement de runs
  post-correctif core

### Nouvelle observation runtime critique (2026-04-15 nuit)

Une fois le faux HLE supprimé du chemin devkit, le comportement réel du run
`--load + --devkit` non-HLE change complètement :

- plus de :
  - `HLE UNHANDLED ...`
  - `HLE EXC VEC ...`
  - vieux front `GPU timeout` directement hérité du faux mode HLE
- à la place, on voit :
  - `Dev kit boot: ... hle=0 text_hle=0`
  - puis un saut BIOS transitoire vers `0x00000CA8`
  - puis une boucle basse en RAM/BIOS avec instructions nulles :
    - `PC=0x00001E00 .. 0x000020FC`
    - `INSTR=0x00000000`
    - `I_STAT=0x0000`
    - `I_MASK=0x0009`

Interprétation actuelle :

- le chemin `fast_boot_from_exe()` est historiquement un boot devkit **pensé avec
  HLE implicite**
- une fois ce HLE retiré correctement, on voit enfin le vrai problème :
  le contrat de boot devkit non-HLE n'est pas valide en l'état
- donc les anciens diagnostics "crash CPU wait GPU" observés sous `--devkit`
  étaient au moins en partie des artefacts de ce faux HLE résiduel

Conclusion de méthode :

- il ne faut plus utiliser les anciens runs CLI `--devkit` comme preuve
  du bug non-HLE tant qu'ils datent d'avant ce fix
- le prochain travail utile doit distinguer clairement :
  - **boot devkit hybride** (`--load + --devkit`)
  - **vrai boot non-HLE BIOS/hardware**

### Variante debug crash CPU

Pour la chasse au crash CPU / stack corruption :

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --load="E:/Projects/PSX/roms/TREX.EXE" `
  --devkit `
  --psxparam=1,30 `
  --cpu-crash-trace=512 `
  --timeout-ms=15000
```

### Variante trace wait GPU / IRQ

Pour arrêter dans le couloir kernel GPU et avoir une sortie plus lisible :

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --load="E:/Projects/PSX/roms/TREX.EXE" `
  --devkit `
  --psxparam=1,30 `
  --pretty `
  --reg-trace=0x8014E034:0x8014E084:0x00000001 `
  --stop-on-pc=0x8014E048 `
  --cpu-crash-trace=512 `
  --timeout-ms=10000
```

### Lancement PowerShell robuste

Quand PowerShell parse mal la ligne ou que la redirection est capricieuse, le
plus fiable est :

```powershell
$exe='E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe'
$args=@(
  '--bios=E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin',
  '--load=E:/Projects/PSX/roms/TREX.EXE',
  '--devkit',
  '--psxparam=1,30',
  '--cpu-crash-trace=512',
  '--timeout-ms=15000'
)
& $exe @args
```

### Rappel logs utiles

- CLI logs dossier :
  - `E:\Projects\github\Live\R3000-Emu\logs\`
- en pratique, les signaux les plus utiles pour TREX sont souvent dans :
  - `io.log`
  - `cdrom.log`

---

## Partie 14 — Split propre `--devkit` / `--devkit-hle` (2026-04-15)

### Objectif

Séparer enfin deux modes qui n'ont pas la même sémantique :

- `--devkit`
  - boot EXE direct **non-HLE**
  - BIOS réel toujours mappé
  - pas de bootstrap HLE forcé
- `--devkit-hle`
  - boot EXE direct **assisté HLE**
  - utile pour les cas où l'on veut explicitement un environnement SDK/kernel
    aidé

Important :

- `bootcd` / `--cd` ne doit pas être impacté
- les validations doivent se faire via les logs centralisés (`emu::logf`),
  pas via `stdout/stderr`

### Changements de code

Core :

- `src/emu/core.h`
- `src/emu/core.cpp`

Ajout d'un vrai mode de boot EXE :

- `Core::ExeBootMode::devkit`
- `Core::ExeBootMode::devkit_hle`

CLI :

- `cli/main.cpp`

Le CLI parse maintenant :

- `--devkit`
- `--devkit-hle`

Règles :

- si les deux sont passés, `--devkit-hle` gagne explicitement
- `--devkit-hle` force `hle_vectors=1`
- `--devkit` laisse le mode non-HLE intact

### Commandes de test validées

#### 1. Vrai devkit non-HLE

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --load="E:/Projects/PSX/roms/TREX.EXE" `
  --devkit `
  --psxparam=1,30 `
  --cpu-crash-trace=512 `
  --max-time=3 `
  --emu-log-level=trace
```

#### 2. Devkit assisté HLE

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --load="E:/Projects/PSX/roms/TREX.EXE" `
  --devkit-hle `
  --psxparam=1,30 `
  --cpu-crash-trace=512 `
  --max-time=3 `
  --emu-log-level=trace
```

#### 3. Contrôle `bootcd` inchangé

```powershell
E:\Projects\github\Live\R3000-Emu\lib\Debug\r3000_emu.exe `
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" `
  --cd="E:/Projects/PSX/roms/Ridge Racer (U).cue" `
  --max-time=2 `
  --emu-log-level=trace
```

### Résultats observés

#### `--devkit`

Run court valide, non-HLE réel :

- aucun `HLE UNHANDLED ...`
- aucun texte TREX visible sur ce run court
- `io.log` quasi vide à part :
  - `CDROM log start`
  - `GPU log start`

Interprétation :

- le mode n'utilise plus le faux chemin HLE
- le contrat de boot direct non-HLE reste à creuser, mais le split de mode est
  bien réel

#### `--devkit-hle`

Run court clairement assisté HLE :

- texte TREX visible immédiatement :
  - `Dino: Mode: attract(1), timeout 1500.`
  - `This is not pre-render...`
- appels HLE visibles dans `io.log` :
  - `HLE UNHANDLED B(0x15)`
  - `HLE OpenEvent ...`
  - `HLE UNHANDLED B(0x16)`

Interprétation :

- le mode HLE est bien séparé
- il reproduit le comportement assisté attendu

#### `bootcd`

Contrôle rapide OK :

- `io.log` montre bien :
  - `ISO9660: 'SYSTEM.CNF' -> ...`
  - `disc inserted ...`

Conclusion :

- le split `devkit` / `devkit-hle` est en place
- `bootcd` n'a pas été cassé par ce refactor
- la suite du debug TREX non-HLE doit maintenant repartir du vrai mode
  `--devkit`, sans confusion avec le chemin HLE assisté

