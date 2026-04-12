# Debug session — GPU ↔ PSX2DRenderComponent double-buffer contract

**Date**: 2026-04-12
**Branch**: `codex/psx-naming-surface-refactor`
**Game**: TREX.EXE (SCEE Demo One — Europe)
**Symptom**: En UE5, la géométrie 2D saute visuellement entre deux bandes Y (haut/bas). Pas reproductible facilement en CLI (on n'atteint pas la phase de rendu TREX).

---

## Root cause suspectée — rupture de contrat `DrawCmd`

Le pipeline GPU → composant 2D a un **contrat flou** sur ce que contiennent `DrawCmd.v[].x/y` :

- **Coords brutes GP0** (telles que décodées du FIFO), ou
- **Coords raster finales** (après application de `draw_env.offset_x/y`) ?

Tant que ce contrat n'est pas explicite, chaque couche compense à sa façon et introduit un bug.

### Ce qu'on observe dans le code courant

#### `src/gpu/gpu.cpp`
- Commentaire ligne 421 : *"Vertices are stored with draw_offset subtracted (screen-relative), so both double-buffer halves overlap at the same coords"*
- Mais `log_draw_list_summary()` (l.57–99) calcule `raster = v.x + offset_x` et `raster = v.y + offset_y` — il **ajoute** l'offset, ce qui n'est cohérent que si `v.x/y` sont bruts.
- → Incohérence entre le commentaire (soustraction) et l'instrumentation (addition).
- Instrumentation provisoire ajoutée par Cortex : `DRAWLIST_SUMMARY` log `warn` qui dump raw_xy vs raster_xy, `raw_y(lo/hi)`, `raster_y(lo/hi)`, `ofs`, `disp`. Sortie attendue dans `gpu.log`.

#### `integrations/ue5/.../PSX2DRenderComponent.cpp` (l.312–319)
```cpp
const gpu::DisplayConfig& Disp = DrawList.display;
const float OriginX = bCenterDisplay
    ? (static_cast<float>(Disp.display_x) + 0.5f * static_cast<float>(Disp.width()))
    :                                        0.5f * static_cast<float>(Disp.width());
const float OriginY = bCenterDisplay
    ? (static_cast<float>(Disp.display_y) + 0.5f * static_cast<float>(Disp.height()))
    :                                        0.5f * static_cast<float>(Disp.height());
```

**Problème identifié par Cortex** :
`display_x/display_y` décrivent **où le buffer affiché se trouve en VRAM** — pas où les primitives vivent en espace écran logique.

Quand le jeu flippe ses deux buffers VRAM (y=0 ↔ y=240), `OriginY` saute aussi → la géométrie 2D saute haut/bas **même si les coords écran du jeu sont stables**. C'est exactement le symptôme observé.

Ligne 365–367 applique ensuite :
```cpp
const float dx = (V.x + 0.5f) - OriginX;
const float dy = (V.y + 0.5f) - OriginY;
Cur->Vertices.Add(FVector(Depth, dx*EffScale + DisplayOffset.X, -dy*EffScale + DisplayOffset.Y));
```

Donc si `V.x/y` sont déjà raster-finaux, soustraire `display_y` = double-compensation incorrecte.

---

## Stratégie discutée (question ouverte par l'utilisateur)

> *"En fait ça dépend si on veut que la classe GPU soit plus proche de la PS1 d'origine et si on veut que le composant lui émule ce qu'on a sur Android […] Il me semble que ça serait mieux que la classe GPU soit proche de ce qu'on fait dans la PS1 et que derrière on rattrape le coup sur le composant et pareil sur le composant 3D."*

### Option A — GPU fidèle à la PS1 (préférée par user)
- `src/gpu/gpu.cpp` stocke les vertices **en coords brutes GP0**, sans appliquer `draw_offset`.
- Porte aussi `draw_env` + `display` dans chaque `FrameDrawList` (déjà le cas).
- Les composants (`PSX2DRenderComponent`, 3D component) appliquent eux-mêmes la transformation coord PS1 → écran → monde UE.
- **Avantage** : le core reste un miroir exact du hardware, réutilisable pour Android / autres front-ends.
- **Inconvénient** : chaque front-end doit connaître les règles `offset_x/y` + `display_x/y` + recentrage.

### Option B — GPU donne du raster final
- `DrawCmd.v[]` contient déjà `x + offset_x`, `y + offset_y`.
- Les composants n'ont qu'à positionner la caméra / recentrer.
- **Avantage** : composants triviaux.
- **Inconvénient** : le core perd de l'information hardware, moins fidèle.

### Décision prise par l'utilisateur
**Option A — core fidèle PS1, composants responsables du mapping écran**.
Rattraper le coup dans `PSX2DRenderComponent` **et** dans le composant 3D symétriquement.

---

## Plan de correctif (à exécuter, pas encore fait)

1. **Clarifier le contrat dans `src/gpu/gpu.h`** — commenter explicitement sur `DrawCmd` / `DrawVertex` :
   > *"Coordonnées GP0 brutes, non transformées. Pour obtenir les coords raster finales : `v.x + draw_env.offset_x`. Pour projeter à l'écran : soustraire ensuite `display.display_x/y` selon le cas."*

2. **`src/gpu/gpu.cpp`** :
   - S'assurer que `DrawCmd.v[]` stocke bien les coords brutes (vérifier la phase de décodage GP0 — pas toucher si déjà le cas).
   - Garder `DRAWLIST_SUMMARY` actif tant que le bug n'est pas clos.
   - Supprimer le commentaire trompeur l.421 qui dit *"stored with draw_offset subtracted"*.

3. **`PSX2DRenderComponent.cpp` l.312–319** :
   - Retirer `Disp.display_x/y` du calcul d'origine.
   - Nouvelle origine = raster space :
     ```cpp
     // v.x/y sont en coords GP0 brutes — on applique d'abord draw_offset pour atterrir en raster,
     // puis on recentre autour du milieu du draw buffer.
     const auto& Env = DrawList.draw_env;
     const float OriginX = 0.5f * static_cast<float>(Disp.width());
     const float OriginY = 0.5f * static_cast<float>(Disp.height());
     // ...
     const float rx = (V.x + Env.offset_x) + 0.5f;
     const float ry = (V.y + Env.offset_y) + 0.5f;
     const float dx = rx - OriginX;
     const float dy = ry - OriginY;
     ```
   - `display_x/y` reste utilisé **uniquement** pour diagnostics / UI, plus pour positionnement géométrique.

4. **Composant 3D** — appliquer la même correction par symétrie (à vérifier dans `PSXVideoSurfaceComponent`/`PSXImageSurfaceComponent` si pertinent).

5. **Validation** :
   - CLI : lancer TREX avec `--3d-diag`, vérifier `DRAWLIST_SUMMARY` dans `logs/gpu.log` — confirmer que `raw_y` alterne mais que `raster_y` est stable (c'est le comportement attendu).
   - UE5 : rebuild Live Coding, tester visuellement, plus de saut haut/bas.

---

## Bugs secondaires signalés par Cortex (hors-scope de cette session)

- `PSXEmulatorComponent.cpp:562` — propagation de `EmuLogLevel` aux sinks fichiers : **correction déjà bonne**, ne touche pas au bug de rendu.
- Guest CPU fault `IFETCH fault vaddr=0x00000001` autour de `pc=0x8014E07C` dans `system.log` — **bug CPU séparé**, à traiter dans une autre session.
- TREX.EXE en `--load` seul part en panic BIOS/CD — normal, le demo-disc a besoin de son contexte disque.

---

## Fichiers pertinents

- `src/gpu/gpu.h` — définition `DrawCmd`, `DrawVertex`, `DisplayConfig`, `FrameDrawList`
- `src/gpu/gpu.cpp:57-99` — `log_draw_list_summary()` (instrumentation provisoire)
- `src/gpu/gpu.cpp:420-470` — VBlank swap + logging
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/PSX2DRenderComponent.cpp:312-380` — origin calc + vertex transform

## ROM et commande CLI de test

```
--bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin"
--cd="E:/Projects/PSX/roms/Demo One (Version 1) (Europe).cue"
--timeout-ms=30000
```

---

## Prochaine étape concrète

Attendre la validation utilisateur sur **l'Option A** (il a déjà penché pour cette solution), puis exécuter le plan ci-dessus dans l'ordre 1 → 5.

Le commentaire de user dans le dernier message :
> *"Il me semble que ça serait mieux que la classe GPU soit proche de ce qu'on fait dans la PS1 et que derrière on rattrape le coup sur le composant et pareil sur le composant 3D. Tu en penses ?"*

est une demande d'avis, pas encore un feu vert. **Répondre d'abord avec un avis technique court avant de toucher au code.**

---

## Update 2026-04-12 — User a validé Option A, fix appliqué + validé CLI

### Découvertes en lisant le code
- `gpu.cpp:255-264` (`push_triangle`) stocke en réalité **les coords GP0 brutes** dans `cmd.v[].x/y` (pas les `rx,ry` calculés juste au-dessus). Le contrat *réel* du core était déjà bon.
- Le commentaire `gpu.h:86` *"after draw offset"* était stale et trompeur — d'où la confusion.
- `PSX3DRenderComponent.cpp:291-292` utilise déjà `0.5f * RawW/H` sans `display_x/y` → **3D component déjà correct**, le bug était isolé au 2D.
- `PSXImageSurfaceComponent` et `PSXVideoSurfaceComponent` utilisent `display_x/y` légitimement comme coords VRAM de blit (ils lisent le scanout window) → **ne pas toucher**.

### Modifs appliquées
1. `src/gpu/gpu.h` : nouveau commentaire de contrat sur `DrawVertex` — coords brutes GP0 explicites + règles pour reconstruire raster/scanout côté front-end.
2. `src/gpu/gpu.cpp:253-258` : commentaire de stockage corrigé ("raw GP0 polygon coords").
3. `src/gpu/gpu.cpp:421` (VBlank swap) : commentaire corrigé ("no draw_offset applied" au lieu de "subtracted").
4. `integrations/.../PSX2DRenderComponent.cpp:312-323` : bloc origin réécrit. `OriginX/Y` = `width/2, height/2` si `bCenterDisplay`, sinon `0,0`. Plus aucune référence à `Disp.display_x/y`. Commentaire d'invariant ajouté.
5. `integrations/.../PSX2DRenderComponent.h:143` : tooltip de `bCenterDisplay` mise à jour pour expliquer pourquoi on ne mixe pas `display_x/y`.

### Validation empirique CLI (Demo One Europe, 20s, log → `workbench/demo1_drawlist.log`)

Le `DRAWLIST_SUMMARY` warn-log capture la transition BIOS → TREX en double-buffer. Pattern observé à partir de la frame 1935 :

```
f=1935 raw_xy=[0..320,0..256] raster_xy=[320..640,0..256]   ofs=(320,0)   disp=(320,256)+(320,256)
f=1936 raw_xy=[0..320,0..256] raster_xy=[320..640,256..512] ofs=(320,256) disp=(320,0)  +(320,256)
f=1937 raw_xy=[0..320,0..256] raster_xy=[320..640,0..256]   ofs=(320,0)   disp=(320,256)+(320,256)
f=1938 raw_xy=[0..320,0..256] raster_xy=[320..640,256..512] ofs=(320,256) disp=(320,0)  +(320,256)
```

**Analyse** :
- C'est le double-buffer canonique PS1 : VRAM(320,0) ↔ VRAM(320,256) qui flippent.
- `raw_xy` reste **rigoureusement stable** = `[0..320, 0..256]` → confirme empiriquement que le core stocke bien les coords GP0 logiques, indépendantes du buffer cible.
- `display_y` alterne 0 ↔ 256 chaque frame → c'est exactement la valeur qui faisait sauter `OriginY` de 128 ↔ 384 dans l'ancien code 2D component.

**Quantification du bug d'origine** :
- Avant le fix : `OriginY = display_y + 128` → frame impaire 128, frame paire 384 → saut de **256 pixels** entre les deux frames.
- Après le fix : `OriginY = 128` constant → géométrie stable.

L'amplitude du saut prédite (256 px = pleine hauteur 240p) colle au symptôme visuel "haut/bas" rapporté par l'utilisateur dans UE5.

### Reste à faire
- Tester en UE5 (Live Coding ou rebuild) — le binaire CLI ne peut pas valider visuellement, seulement la stabilité des données.
- Décider si on garde l'instrumentation `DRAWLIST_SUMMARY` warn-log en place ou si on la passe en `debug` une fois la régression confirmée fermée.
