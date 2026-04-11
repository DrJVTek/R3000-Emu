# PSXVR True 3D Patch System Strategy

Date: 2026-04-08

## But

Construire un systeme generique qui permette de faire tourner des jeux PS1 en:

- vraie 3D reconstructee,
- VR,
- avec un minimum de logique speciale codee "en dur par jeu",
- tout en gardant un chemin de rendu coherent entre CLI, UE5 et plus tard Quest.

Le point cle:

Le probleme n'est plus seulement "retrouver la geometrie GTE".
Le vrai probleme produit est maintenant la **composition complete de frame**:

- geometrie 3D reconstruite,
- primitives GPU 2D,
- affichage direct framebuffer / backbuffer,
- FMV / MDEC,
- effets de feedback / post-process old-school,
- ordre visuel entre tous ces elements,
- et adaptation VR.

Autrement dit:

Le systeme final doit savoir **de quoi une frame est composee** avant de savoir **comment la presenter en 3D/VR**.

---

## Ce qui existe deja

### Briques runtime deja presentes

- `UPSX3DRenderComponent`
  - reconstruction 3D GTE/GPU
  - fallback 2D partiel
  - modes de tracking `LegacyFollowOwner`, `WorldLocked`, `VRRecenterable`
- `UPSXVideoSurfaceComponent`
  - detection MDEC / affichage direct du display area
  - rendu sur plane
  - sphere mode annonce mais pas encore reellement exploite
- `UPSXImageSurfaceComponent`
  - detection d'images statiques ecrites directement dans la zone affichee
  - rendu sur plane
- `Psx3dModeManager`
  - distinction `game` / `analysis`
- `Psx3dProfileStore`
  - persistance des profils de reconstruction/analyse `psx3d`

### Limite structurelle actuelle

Aujourd'hui, ces briques existent surtout comme composants separes:

- 3D reconstruite
- video plane
- image plane

Mais il manque encore un **chef d'orchestre de composition** qui decide, frame par frame:

- quel pipeline est actif,
- quels layers doivent exister,
- qui est devant/derriere,
- ce qui doit etre remappe en surface 2D,
- et quels patches runtime sont applicables.

---

## Constat principal

Si on part sur "plein de booleens par jeu", on va perdre le projet.

Le bon modele n'est pas:

- une liste infinie de flags sans structure

Le bon modele est:

- un petit nombre de **categories de patches typés**
- appliquees par un **systeme de presentation/composition**
- configurees par **profils par jeu**
- avec, si besoin, des exceptions scene-specifiques.

En clair:

On veut un systeme de patches **declaratif**, pas un sac de hacks.

---

## Decision d'architecture proposee

### Un seul fichier profil, mais structure interne separee

La bonne simplification n'est probablement pas de separer les fichiers.
La bonne simplification est:

- **un seul fichier profil par jeu**
- avec des blocs internes bien distincts

Exemple logique:

- `analysis`
  - reconstruction 3D
  - hotspots
  - rules GTE/GPU
- `presentation`
  - composition de frame
  - surfaces 2D/video/image
  - ordre visuel
  - policies VR
- `scene_rules`
  - activations conditionnelles
  - patches limits a certains contextes du jeu

Pourquoi c'est mieux:

- un seul point d'entree par jeu
- plus simple a maintenir et versionner
- mais sans melanger conceptuellement analyse et presentation

Le principe a garder:

- fichier unique
- schema interne fortement structure

---

## Idee centrale: Frame Composition Graph

Au lieu de penser "renderer GPU3D + quelques flags", on doit penser:

Chaque frame produit un **graphe de presentation**.

Noeuds possibles:

- `WorldGeometry`
  - mesh 3D reconstruit
- `ScreenSurface`
  - plane ou demi-sphere qui affiche une image/video/framebuffer
- `Overlay2D`
  - sprites/quads/UI/polys 2D
- `FeedbackSurface`
  - reutilisation d'un rendu precedent
- `EffectLayer`
  - motion blur, image blend, special compositing

Ce graphe est derive de:

- la frame GPU,
- l'etat display,
- les ecritures VRAM,
- les metadonnees MDEC,
- le profil du jeu,
- et les heuristiques runtime.

Ce graphe ne doit pas etre purement statique.

Il doit pouvoir changer:

- selon la scene,
- selon le moment dans le jeu,
- selon le type d'affichage observe a cet instant.

Donc il faut assumer des **systemes de detection runtime** qui activent les bons modes.

---

## Activation conditionnelle par scene / contexte

Tu as raison: les patches ne seront pas globaux au jeu.

Le systeme doit pouvoir activer des regles:

- selon la scene,
- selon un ecran de boot,
- selon une FMV,
- selon une sequence de menu,
- selon une zone de gameplay,
- selon un effet visuel local.

### Sources de detection possibles

- `game state heuristics`
  - type de frame observee
  - presence de 3D / video / ecriture directe
- `display signature`
  - display area
  - cadence de changement
  - format 15-bit / 24-bit
- `GPU signature`
  - proportion de fullscreen quads
  - presence/absence de `origin_3d`
- `PC / callsite windows`
  - a utiliser seulement si necessaire
- `asset / stream signatures`
  - ex: sequence video connue

### Regle importante

On doit preferer:

- des detecteurs generiques bases sur le comportement

avant:

- des conditions trop specifiques a un PC ou a un nom de scene

Les signatures par PC restent utiles, mais comme dernier niveau de verrouillage,
pas comme base unique du systeme.

---

## Modes de composition a supporter

Au lieu d'avoir mille flags de haut niveau, on veut d'abord classifier la frame dans un des modes suivants:

### 1. `world_3d_only`

Cas:

- scene 3D classique
- HUD absent ou negligeable

Presentation:

- mesh 3D seulement

### 2. `world_3d_plus_overlay`

Cas:

- scene 3D + HUD / sprites / texte / quads
- logo PlayStation avec sprites par-dessus

Presentation:

- mesh 3D
- overlay 2D dans un layer dedie

### 3. `screen_surface_only`

Cas:

- FMV plein ecran
- image statique plein ecran
- ecran genere par ecriture directe framebuffer

Presentation:

- plane ou demi-sphere avec taille physique stable

### 4. `world_3d_plus_screen_surface`

Cas:

- video ou image dans le monde,
- ou rendu direct utilise comme couche de fond/avant-plan

Presentation:

- mesh 3D
- une surface ecran additionnelle

### 5. `feedback_effect`

Cas:

- Metal Gear Solid motion blur
- copies framebuffer / blend / reapplique en transparence

Presentation:

- surface de feedback ou history texture
- composee avec le reste

### 6. `mixed_special`

Cas:

- jeux hybrides ou scenes atypiques
- plusieurs surfaces + 3D + overlay + effet

Presentation:

- entierement pilotee par le profil

---

## Signaux generiques a exploiter

Le systeme doit s'appuyer sur des signaux runtime generiques, pas sur des "if MetalGear".

### Signal A - densite 3D

- nombre de primitives `origin_3d`
- ratio `origin_3d / total`
- bbox 3D
- variation temporelle

### Signal B - activite display direct

- ecritures CPU->VRAM dans la zone affichee
- `LoadImage`/DMA vers backbuffer visible
- changements rapides de `display_x/display_y/width/height`

### Signal C - activite MDEC/video

- `has_mdec_display_content()`
- 15-bit/24-bit display mode
- cadence de renouvellement du display area

### Signal D - nature des primitives 2D

- quads fullscreen de clear
- sprites HUD
- polys texte
- proportion de primitives texturees/non texturees

### Signal E - feedback VRAM

- lecture/reutilisation d'une region VRAM qui correspond a un ancien buffer
- blends semi-transparents relies a cette region
- persistance frame-to-frame

### Signal F - scene/layout stability

- meme composition sur N frames
- changement brutal de mode lors d'une scene

---

## Strategies generiques de reconstruction 3D

Le projet ne doit pas supposer qu'il n'existe qu'une seule methode de reconstruction.

Il faut au contraire assumer plusieurs familles de methodes, avec:

- une priorite,
- un score de confiance,
- et la possibilite de mixer plusieurs indices.

### Strategie 1 - Correlation directe GTE -> GPU

C'est la methode actuelle et elle reste la plus forte quand elle marche.

Principe:

- capturer les sorties 2D du GTE,
- les comparer aux coordonnees 2D finalement envoyees au GPU,
- retrouver le lien entre:
  - points projetes,
  - primitive GPU,
  - et mesh 3D source.

Avantage:

- tres precise
- tres explicable
- correspond bien a la vraie causalite PS1

Limite:

- ne couvre pas tous les patterns
- peut casser quand le jeu remanie/interpole/recompose les primitives

### Strategie 2 - Reconstruction par familles de boucles de rendu

Celle-ci est probablement la suite logique la plus puissante maintenant qu'on a:

- MCP,
- Ghidra,
- traces runtime,
- profils.

Principe:

- analyser les boucles CPU qui construisent les paquets GPU,
- reconnaitre une famille de rendu,
- puis appliquer la recette de reconstruction adaptee.

Exemples de familles:

- `RTPT triangle direct`
- `RTPS accumulation`
- `quad split classique`
- `subdivision texturee`
- `sprite billboarding`
- `fullscreen feedback pass`
- `draw directe framebuffer`

Avantage:

- plus generique que le simple matching ponctuel
- permet de reconnaitre des patterns reutilises entre plusieurs jeux
- ouvre la voie a un vrai "catalogue de render loops PS1"

Limite:

- demande une bonne authoring pipeline
- necessite une taxonomie stable des familles de boucles

### Strategie 3 - Reconstruction par provenance memoire

Principe:

- suivre l'origine des donnees en RAM/registres
- comprendre quels mots deviennent des vertices GPU
- rattacher ces donnees a une origine GTE / objet / transform

Avantage:

- utile quand la relation n'est plus visible directement a l'etape GPU
- bonne base pour l'analyse offline

Limite:

- plus lourde
- demande des heuristiques propres et des outils d'observabilite

### Strategie 4 - Heuristique de regroupement topologique

Principe:

- meme sans correspondance parfaite GTE/GPU,
- reconnaitre des groupes de primitives comme appartenant a un meme mesh

Indices utiles:

- voisinage spatial ecran
- voisinage UV
- coherence OT
- cohorte temporelle dans la meme boucle CPU
- repetition frame-to-frame

Avantage:

- utile pour les cas "presque reconstructibles"

Limite:

- moins fiable
- doit etre marquee comme confidence plus faible

### Strategie 5 - Recipes speciales mais generiques

Certaines recettes peuvent etre "speciales" sans etre "hack de jeu".

Exemples:

- `quad paired-edge reconstruction`
- `subdivided textured quad reconstruction`
- `screen-aligned billboards`
- `feedback-derived layer`

Ce sont de bonnes candidates pour des modules standard du moteur,
pas pour des patches ad hoc par titre.

### Regle de priorite recommande

Quand plusieurs methodes sont disponibles:

1. correlation causale directe GTE/GPU
2. famille de boucle de rendu reconnue
3. provenance memoire / hints ADN
4. heuristique topologique
5. fallback 2D assume

Le moteur doit toujours pouvoir expliquer:

- quelle methode a gagne,
- et avec quel niveau de confiance.

---

## Classification des boucles de rendu

L'une des meilleures directions de recherche est de traiter les render loops
comme un probleme de reconnaissance de patterns.

### Idee centrale

Il n'y a pas dix mille facons pour un jeu PS1 de:

- transformer ses vertices,
- construire sa liste GPU,
- subdiviser ses quads,
- injecter du HUD,
- ou faire des passes speciales.

Donc le bon objectif long terme est:

- identifier des **types de boucles de rendu**
- puis rattacher chaque boucle observee a un type connu

### Pipeline d'authoring recommande

1. capturer une fenetre runtime representative
2. identifier:
   - PCs chauds
   - patterns DMA/OT
   - usage GTE
   - writes GPU
3. decompiler la boucle
4. classer la boucle dans une famille
5. generer une rule de profil
6. laisser le runtime reconnaitre cette famille plus tard

### Sources de classification

- empreinte des PCs chauds
- forme de la boucle
- sequence d'instructions GTE/GPU
- usage de DMA2
- type de primitives emises
- cadence et structure OT
- relation entre matrices GTE et vertices emis

### Exemple de sortie future

Le profil pourrait dire:

- `render_loop_type = rtps_mesh_builder_v2`
- `render_loop_type = fullscreen_fmv_presenter`
- `render_loop_type = sprite_overlay_pass`
- `render_loop_type = framebuffer_feedback_blend`

Puis le runtime saurait:

- comment reconstruire la geometrie,
- ou au minimum comment composer correctement la frame.

### Ce qu'il faut eviter

Il ne faut pas que cette classification devienne:

- une liste d'adresses brutes sans semantique

Il faut qu'elle reste:

- interpretable,
- factorisable,
- partageable entre jeux.

--- 

## Taxonomie de patches

La bonne granularite n'est pas "un flag par bug". On veut des categories.

### 1. Patches de composition

Exemples:

- `frame_mode = world_3d_plus_overlay`
- `prefer_screen_surface_when_no_origin3d = true`
- `allow_simultaneous_surface_and_geometry = true`

### 2. Patches de surface ecran

Exemples:

- `surface_shape = plane | hemisphere`
- `surface_anchor = camera_locked | world_locked | profile_defined`
- `surface_physical_width = ...`
- `surface_depth_policy = fixed | auto_from_scene`
- `display_area_source = visible_buffer | draw_buffer_a | draw_buffer_b | latched_write_region`
- `buffering_mode = mono | double | triple | auto`

### 3. Patches d'overlay 2D

Exemples:

- `overlay_mode = screen_space_emulated | world_billboard | depth_mapped`
- `overlay_sort = over_all | under_geometry | by_ot_bucket`
- `preserve_sprite_scale_across_resolution = true`

### 4. Patches d'effets speciaux

Exemples:

- `feedback_mode = none | copy_previous_surface | copy_previous_composed_frame`
- `feedback_blend = semi0..semi3 | custom`
- `clear_history_on_scene_enter = true`

### 5. Patches de geometrie / GTE

Exemples:

- `expand_clip_frustum_for_vr = true`
- `clip_expansion_scale = ...`
- `approx_world_from_frame_ref = true`
- `apply_gte_transform = true/false`

### 6. Patches memoire / machine

Exemples:

- `extended_ram_mode = off | devkit_like | custom`
- `timing_bias_profile = ...`

Important:

Ces patches systeme doivent rester **exceptionnels** et explicites.
Ils ne doivent pas etre la base du pipeline.

---

## Exemple de schema de profil unique

Le schema exact pourra changer, mais la forme cible devrait ressembler a ca:

```json
{
  "version": 1,
  "game_id": "SLUS-00594",
  "analysis": {
    "mode_rules": [],
    "camera_candidates": []
  },
  "presentation": {
    "default_composition": {
      "frame_mode": "world_3d_plus_overlay",
      "overlay_mode": "screen_space_emulated",
      "surface_shape": "plane",
      "preserve_display_physical_size": true
    }
  },
  "scene_rules": [
    {
      "name": "boot_logo",
      "match": {
        "display_width": 640,
        "display_height": 480,
        "has_3d": true,
        "fullscreen_2d_quads": true
      },
      "actions": {
        "overlay_sort": "over_all",
        "ignore_fullscreen_clear_quads": true
      }
    },
    {
      "name": "fmv_fullscreen",
      "match": {
        "mdec_active": true,
        "has_3d": false
      },
      "actions": {
        "frame_mode": "screen_surface_only",
        "surface_shape": "hemisphere"
      }
    }
  ],
  "effect_rules": [
    {
      "name": "feedback_motion_blur",
      "match": {
        "feedback_detected": true
      },
      "actions": {
        "feedback_mode": "copy_previous_composed_frame",
        "clear_history_on_scene_enter": true
      }
    }
  ],
  "vr_rules": {
    "tracking_mode": "vr_recenterable",
    "expand_clip_frustum_for_vr": false,
    "screen_surface_distance": 150.0
  }
}
```

Le point important:

On veut des **regles lisibles, diffables, debugables**.

---

## Cas concrets a traiter

### 1. Logo PlayStation

Probleme:

- 3D logo reconstruit
- texte/sprites par-dessus
- parfois plus de liste de polys exploitable "comme avant"

Besoin:

- mode `world_3d_plus_overlay`
- filtrage des fullscreen clears
- ordre stable des sprites par-dessus la 3D

### 2. FMV plein ecran

Probleme:

- affichage direct via MDEC / VRAM
- pas de liste de polys utile

Besoin:

- surface video dediee
- taille physique stable
- plane ou demi-sphere VR

### 3. Image statique ecrite directement en VRAM

Probleme:

- meme logique que FMV, mais sans flux video

Besoin:

- surface image dediee
- latch/hold jusqu'au changement de display

### 4. FMV + 3D ou display direct + 3D

Probleme:

- video / image et geometrie simultanees

Besoin:

- plusieurs layers simultanes
- ordre controle par profil

### 5. Motion blur / feedback (ex: Metal Gear Solid)

Probleme:

- l'effet original depend du double buffer VRAM
- si on ne rend plus via la VRAM finale, l'effet disparait
- si on garde l'ancien contenu sans controle, on pollue la scene

Besoin:

- systeme de `FeedbackSurface`
- source clairement definie
- policy de clear/reset

---

## Taille physique constante et normalisation resolution

Ce point est critique et doit etre une regle globale du systeme.

### Regle cible

La taille apparente d'un ecran, logo, HUD ou FMV ne doit pas dependre:

- de `320x240`
- de `640x480`
- ni d'un mode PS1 intermediaire

La resolution doit seulement changer:

- la densite de pixels
- pas la taille physique de presentation

### Consequence technique

Il faut separer:

- **resolution logique**
- **taille physique de presentation**

Donc:

- les surfaces `Image/Video/Display` doivent avoir une **taille physique stable**
- la resolution PS1 ne doit affecter que:
  - la precision du sampling,
  - les UV,
  - ou un crop/letterbox eventuel

En pratique:

- la geometrie de la surface reste stable
- les UV s'adaptent au contenu courant
- un cas particulier pourra exister pour du letterboxing / bandes noires reellement voulues
- mais ce cas doit etre explicite et non implicite

### Consequence directe pour la phase 1

Pour commencer simple:

- on fixe une taille physique de reference pour les surfaces 2D
- on ne laisse pas `320x240` vs `640x480` changer la taille du mesh
- on adapte seulement:
  - les UV
  - la zone de sampling
  - le format de decode

---

## Strategie VR

### 1. Geometrie 3D

Objectif:

- mesh monde libre
- tracking `WorldLocked` / `VRRecenterable`

### 2. Surfaces 2D / video / image

Deux familles:

- `camera-anchored surfaces`
  - pour ecrans/UI/cinematics
- `world-anchored surfaces`
  - pour certains effets diagetiques ou ecrans dans l'espace

Le profil doit pouvoir choisir.

Mais pour la phase initiale, avant la VR complete, il faut deja supporter
plusieurs **formes de surface**:

- `plane`
- `curved_plane`
- `hemisphere`
- plus tard eventuellement `custom_mesh`

Le plus propre est probablement:

- un mesh/shape defini par le profil
- avec un jeu restreint de parametres

plutot qu'un code special pour chaque composant.

### 3. Elargissement du clipping GTE

Bonne idee, mais a traiter comme un patch controle.

Regle recommandee:

- ne jamais changer ca globalement par defaut
- le mettre dans les `vr_rules`
- avec un mode `off` par defaut

Pourquoi:

- ca change la geometrie recuperee
- ca peut faire reapparaitre des polys volontairement clippes
- donc il faut que ce soit explicite, measurable et reversible

### 4. RAM supplementaire / mode devkit-like

Idem:

- a garder comme outil experimental
- pas comme comportement standard

Ca peut etre utile plus tard pour certains patches ou buffers additionnels,
mais ce n'est pas une base saine pour le pipeline principal.

---

## Futur 3D: mesh solide et qualite visuelle

Ce n'est pas la priorite immediate, mais il faut garder ces axes dans le cadre,
parce qu'ils seront critiques pour la vraie VR.

### 1. Sortir de la simple "liste de triangles colles"

Objectif:

- aller vers un vrai mesh exploitable visuellement

Problemes a traiter plus tard:

- adjacency
- continuites entre triangles
- reconstitution de quads/surfaces
- detection de coutures

### 2. Ameliorer les arrondis / silhouettes

Objectif:

- reduire l'aspect trop casse des meshes PS1 une fois promenes en VR

Pistes:

- regroupement de faces par surface
- lissage conditionnel
- reconstruction de quads avant triangulation finale
- eventuelle subdivision douce, mais seulement quand elle est sure

### 3. Normales et eclairage

Objectif:

- obtenir une presentation 3D plus solide sans trahir le look PS1

Pistes:

- normales derivees de la topologie recomposee
- normales plates / lissees selon profil
- eclairage stylise inspiré PS1 plutot que PBR brut

### 4. Solides et fermetures

Certains jeux pourront beneficier plus tard de:

- fermeture de volumes evidents
- double-face controle
- epaisseur artificielle pour certains elements

Mais cela doit rester:

- optionnel,
- profile-driven,
- et jamais applique globalement sans preuve.

### 5. Qualite VR

En VR, les defauts deviennent beaucoup plus visibles:

- intersections
- surfaces sans epaisseur
- clipping trop agressif
- sprites mal ancres
- incoherences d'echelle

Donc la qualite 3D future doit etre pensee comme:

- une couche d'amelioration progressive au-dessus de la reconstruction de base
- pas comme un pre-requis pour commencer

---

## Garde-fous de conception

### 1. Pas de hack ad hoc sans categorie

Si un patch n'entre dans aucune categorie propre:

- il ne doit pas etre ajoute tel quel

Il faut d'abord definir sa famille de comportement.

### 2. Pas de divergence CLI / UE5

Tout ce qui decide:

- la composition
- les activations de modes
- les patches

doit vivre dans le core autant que possible.

UE5 doit etre:

- un renderer/client,
- pas la seule source de verite.

### 3. Toute decision automatique doit etre explicable

Exemples de log:

- pourquoi on est passe en `screen_surface_only`
- pourquoi on a ignore un clear fullscreen
- pourquoi on a active un feedback layer
- quelle regle de profil a match

### 4. Les composants ne doivent pas se battre entre eux

Aujourd'hui on a:

- `Gpu3DComponent`
- `VideoComponent`
- `ImageComponent`

Le systeme final doit eviter:

- plusieurs composants visibles en meme temps "par accident"
- des toggles incoherents

Il faut un arbitre central de presentation.

---

## Roadmap recommande

### Phase 0 - Cadrage

- finaliser la taxonomie des patches
- valider le split `psx3dprof` / `psxvrprof`

### Phase 1 - Observabilite

- extraire des signaux runtime standardises:
  - `has_3d`
  - `has_video`
  - `has_display_write`
  - `fullscreen_clear`
  - `feedback_detected`
  - `display_signature`
- les logger proprement

### Phase 2 - Modele de composition

- ajouter un `FramePresentationMode`
- produire un `FrameCompositionDecision`
- centraliser l'arbitrage entre 3D / video / image / overlay

### Phase 3 - Profil unique et schema interne

- etendre le store de profil existant vers un schema unifie
- garder un format JSON versionne
- ajouter:
  - `analysis`
  - `presentation`
  - `scene_rules`
  - `effect_rules`

### Phase 4 - Unification des surfaces ecran

- unifier `VideoComponent` et `ImageComponent` conceptuellement en `DisplaySurface`
- garder plusieurs implementations si necessaire, mais une politique commune

### Phase 5 - Overlay 2D robuste

- systeme propre de tri:
  - over all
  - under geometry
  - OT-based
- gestion correcte des cas type logo PlayStation

### Phase 6 - Feedback / effets

- history buffer / feedback surface
- premier cas cible: Metal Gear Solid intro motion blur

### Phase 7 - VR tuning

- taille physique constante
- plane vs hemisphere
- regles de confort / distance / recenter
- clipping expansion optionnelle

### Phase 8 - Perf / Quest

- rendre le runtime le plus declaratif possible
- minimiser les copies
- garder l'analyse lourde hors ligne

---

## Phase 1 ciblee: finir correctement le 2D de presentation

Avant de replonger dans la 3D/VR complete, la priorite la plus saine est:

- finir la couche 2D de presentation

Portee exacte de cette phase:

### 1. Surfaces `Video` / `Image` / `Display`

Objectif:

- afficher tout contenu framebuffer/video/image a taille physique constante
- quel que soit le mode de resolution PS1

### 2. Formes de presentation

Objectif:

- supporter plusieurs formes de surface:
  - `plane`
  - `curved_plane`
  - `hemisphere`

Le choix doit etre pilotable par profil.

### 3. Sampling et UV

Objectif:

- la geometrie ne change pas de taille
- seuls les UV / zones de sampling s'adaptent
- eventuellement support explicite du letterbox/pillarbox

### 4. Arbitrage simple initial

Pour commencer:

- si video/image/direct-display actif
- on affiche une surface stable
- sans encore resoudre tous les cas hybrides complexes

### 5. Cas de validation

Ordre recommande:

1. FMV plein ecran simple
2. image statique ecrite directement dans le display
3. logo PlayStation
   - garder le fond/logo stable
   - permettre l'affichage du texte/sprite par-dessus

Le logo PlayStation est un excellent exercice parce qu'il force deja a penser:

- persistance d'une couche
- overlay sprite/polygon par-dessus
- coherence visuelle entre deux systemes d'affichage

---

## Strategie recommandee pour la detection `Video / Image / Display`

### Probleme a resoudre

Aujourd'hui:

- on a des briques separees pour `VideoComponent` et `ImageComponent`
- mais conceptuellement elles essaient deja de faire la meme chose:
  - ouvrir une surface
  - choisir une source texture
  - la maintenir
  - puis la fermer

Ces heuristiques sont deja utiles, mais elles restent:

- locales,
- dupliquees,
- un peu fragiles,
- et sans vrai modele central de mode / policy.

Le bon modele n'est pas:

- "chaque composant se montre/cache tout seul"

Le bon modele est:

- un **systeme unique de surface d'affichage**
- qui observe les signaux runtime generiques
- choisit un **mode**
- applique une **policy**
- puis produit une decision de presentation.

Autrement dit:

- on ne veut plus plusieurs systemes qui arbitrent
- on veut un seul `DisplaySurfaceSystem`
- avec un presenter backend-specifique en aval

### Mode central propose

Ajouter un mode central dedie a la surface d'affichage:

- `none`
- `video_fmv`
- `static_image`
- `direct_display`
- `hold_last_surface`
- `hybrid_reserved`

Description:

- `none`
  - aucune surface ecran speciale
- `video_fmv`
  - flux MDEC / FMV / lecture continue d'une surface video
- `static_image`
  - image ecrite puis stable dans la zone affichee
- `direct_display`
  - contenu affiche via framebuffer visible / backbuffer sans signature MDEC claire
- `hold_last_surface`
  - maintenir temporairement la surface precedente pour laisser passer un overlay
  - cas typique: logo PlayStation
- `hybrid_reserved`
  - reserve pour les cas ou une scene impose plusieurs logiques en meme temps

### Etat runtime minimal recommande

Il faut un petit etat unique du type:

```cpp
enum class DisplaySurfaceMode
{
    none,
    video_fmv,
    static_image,
    direct_display,
    hold_last_surface,
    hybrid_reserved,
};

struct DisplaySurfaceState
{
    DisplaySurfaceMode mode{DisplaySurfaceMode::none};
    gpu::DisplayConfig display{};
    uint32 open_frame{0};
    uint32 last_refresh_frame{0};
    uint32 last_vram_seq{0};
    uint32 last_cpu_write_seq{0};
    uint32 video_confidence{0};
    uint32 image_confidence{0};
    uint32 direct_confidence{0};
    uint32 inactive_frames{0};
    bool hold_requested{false};
};
```

Le point important:

- on garde une **memoire courte**
- on ne decide pas tout sur une seule frame
- on ajoute une petite hysteresis pour eviter les bascules parasites

### Signaux d'entree a utiliser

On a deja quasiment tout ce qu'il faut.

#### Signal 1 - signature display

- `display_x`
- `display_y`
- `width`
- `height`
- `color_24bit`
- `display_enabled`

Cette signature doit devenir la base commune.

#### Signal 2 - evidence video

- `has_mdec_display_content()`
- activite MDEC recente
- cadence de refresh du display area
- presence de nouveaux pixels dans la zone affichee

#### Signal 3 - evidence image statique

- dernier `CpuVramWriteInfo`
- write couvrant exactement ou presque exactement la zone affichee
- contenu non vide / non noir
- stabilite sur plusieurs frames

#### Signal 4 - evidence direct display

- `vram_write_seq` qui avance alors que le display reste stable
- writes repetees dans la zone visible
- changements de buffer visible / display start
- absence de signature MDEC claire

#### Signal 5 - evidence overlay / reprise 3D

- primitives GPU presentes
- ratio primitives 2D / 3D
- changement de composition brusque

Ce signal ne doit pas toujours fermer la surface.

Il doit surtout dire:

- "la surface doit-elle rester seule"
- ou
- "la surface doit-elle etre maintenue pendant qu'un overlay passe devant"

### Regles d'ouverture recommandees

#### Ouvrir `video_fmv`

Ouvrir immediatement si:

- `has_mdec_display_content()` est vrai
- et la zone affichee contient un contenu reel

Ou ouvrir apres courte confirmation si:

- MDEC actif de facon recente
- ET VRAM/display se renouvellent sur `2-3` frames

Raison:

- le faux negatif FMV coute cher
- donc `video_fmv` doit avoir une ouverture plutot reactive

#### Ouvrir `static_image`

Ouvrir si:

- un `CpuVramWriteInfo` couvre la zone affichee ou la couvre "quasi totalement"
- le contenu extrait n'est pas vide
- il n'y a pas de signature `video_fmv`

Important:

- il ne faut pas exiger uniquement un match exact a vie
- il faut prevoir:
  - `exact_match`
  - `near_match`
  - `letterboxed_match`

Car certains ecrans utiles n'ecriront pas toujours un rectangle parfait identique au display.

#### Ouvrir `direct_display`

Ouvrir si:

- la zone affichee est visiblement alimentee
- mais sans signature MDEC
- et sans write unique stable type "image"

Cas cibles:

- ecran genere par rendu direct
- usage du backbuffer visible
- sequences transitoires ou de feedback

### Regles de maintien

Le maintien est plus important que l'ouverture.

Une fois un mode ouvert:

- on ne le ferme pas sur une seule frame contradictoire
- on demande quelques frames d'inactivite ou une evidence contraire forte

Exemples:

- `video_fmv`
  - reste ouvert tant que la video a ete rafraichie recemment
  - meme si une frame n'a pas de hit MDEC explicite
- `static_image`
  - reste ouvert tant que:
    - la signature display reste compatible
    - le contenu n'est pas remplace
    - aucune prise de controle explicite n'arrive
- `direct_display`
  - reste ouvert tant que le display continue a changer de facon coherente

### Regles de fermeture recommandees

#### Fermer `video_fmv`

Fermer si:

- pas de nouvelle evidence video depuis `N` frames
- ET une autre evidence forte prend le dessus

Ne pas fermer juste parce que:

- des primitives apparaissent

Car on veut supporter:

- `world_3d_plus_overlay`
- ou des overlays texte / sprites sur une video ou un fond maintenu

#### Fermer `static_image`

Fermer si:

- la signature display n'est plus compatible
- ou un vrai `video_fmv` prend la main
- ou un nouveau rendu direct ecrase explicitement la surface

Ne pas fermer trop vite juste parce que:

- des primitives GPU sont apparues

Sinon on cassera exactement les cas type:

- logo PlayStation
- fond conserve + texte / sprite par-dessus

#### Fermer `direct_display`

Fermer si:

- plus aucune activite pertinente dans la zone
- ou bascule claire vers `video_fmv`
- ou transition claire vers une autre logique de scene

### Mode special `hold_last_surface`

C'est probablement la cle pratique pour plusieurs cas de boot et de transitions.

Principe:

- si une surface valide vient d'exister
- et qu'un overlay/sprite/polygone apparait juste apres
- on peut demander a maintenir la surface precedente pendant quelques frames

Ce n'est pas un hack de plus.

C'est une primitive de composition legitime.

Cas cible:

- logo PlayStation noir + logo maintenus
- texte blanc / sprite par-dessus

Autrement dit:

- la surface n'est pas "encore active" parce que le flux la rafraichit
- elle est "encore presente" parce que la scene l'utilise comme fond

### Methode de decision recommandee

Le meilleur compromis n'est pas une IA lourde.

Le meilleur compromis est un petit moteur a score:

- `video_score`
- `image_score`
- `direct_score`
- `overlay_score`

Chaque frame:

- on ajoute des points si les signaux sont coherents
- on en retire si le signal disparait
- on prend une decision avec seuils + hysteresis

Exemple de logique:

- `video_score += 4` si `has_mdec_display_content()`
- `image_score += 3` si write display-compatible
- `direct_score += 2` si `vram_write_seq` progresse sans MDEC
- decay leger sur chaque frame
- fermeture seulement si le score courant tombe sous un seuil ET qu'aucun hold n'est actif

Avantages:

- robuste aux micro-variations
- explicable dans les logs
- facile a profiler et a patcher par jeu si besoin

### Ce qui doit etre flague dans le profil

Le profil ne doit pas dire directement:

- "si jeu == X alors force image"

Il doit plutot pouvoir dire:

- quelles strategies sont autorisees
- quels seuils sont ajustes
- si le `hold_last_surface` est permis
- quelle forme de surface utiliser

Exemples de champs utiles:

- `presentation.surface.mode_policy = auto | prefer_video | prefer_image | prefer_direct`
- `presentation.surface.allow_hold_last = true/false`
- `presentation.surface.open_thresholds`
- `presentation.surface.close_thresholds`
- `presentation.surface.match_policy = exact | near | letterbox_aware`
- `presentation.surface.shape = plane | curved_plane | hemisphere | custom_mesh`
- `presentation.surface.anchor = camera_locked | world_locked | profile_defined`
- `presentation.surface.physical_width`
- `presentation.surface.distance`

### Regle importante sur la taille physique

La taille physique de la surface ne doit pas dependre de la resolution PS1.

Donc:

- on garde une geometrie stable
- on adapte surtout:
  - UV
  - sampling
  - letterbox/pillarbox

La detection de mode doit se baser sur:

- la signature display
- pas sur la "taille monde" finale de la surface

### Architecture cible recommandee

Ordre recommande:

1. ajouter un **`DisplaySurfaceSystem`** commun
2. lui faire produire une `DisplaySurfaceDecision`
3. le faire consommer par un presenter backend-specifique
4. garder les anciennes briques UE5 uniquement comme marche d'escalier de migration

Exemple logique:

- le systeme choisit:
  - `mode`
  - `mode_policy`
  - `source_kind`
  - `source_rect`
  - `display_signature`
  - `shape`
  - `hold`
- puis le presenter applique:
  - la texture
  - la geometrie
  - la visibilite

Cela evite:

- plusieurs arbitres concurrents
- des heuristiques dupliquees
- des bugs de priorite difficiles a raisonner

### Recommandation claire sur le nombre de composants

Recommendation:

- **un seul systeme logique de surface**
- **un seul presenter logique de surface par backend**

Le backend peut etre:

- UE5 aujourd'hui
- un renderer Vulkan demain
- ou un backend debug plus simple

Le CLI n'a pas besoin de presenter de rendu, ce n'est pas un probleme:

- il peut quand meme utiliser la meme logique de mode / decision
- simplement sans presenter final

Pourquoi ce modele est meilleur:

- `video`, `image`, `direct_display` et `hold_last_surface` utilisent tous
  - la meme idee de surface
  - la meme logique de placement
  - la meme logique de taille physique
  - la meme politique de mesh
- seule la source de texture et la policy changent vraiment

Donc le bon decoupage est:

- `DisplaySurfaceSystem`
- `DisplaySurfacePresenter`

Et non:

- un composant pour video
- un autre pour image
- un autre pour direct display

### Recommandation claire sur le mesh de rendu

Oui, il faut absolument prevoir le type de mesh de rendu, y compris `custom_mesh`.

Le bon modele n'est pas:

- "la video s'affiche toujours sur un plane procedural"

Le bon modele est:

- la surface d'affichage choisit un **mesh policy**

Exemple:

- `plane`
- `curved_plane`
- `hemisphere`
- `custom_mesh`

Et il faut separer:

- la **decision de mode**
- la **source texture**
- la **geometrie de presentation**

Donc le presenter devrait idealement recevoir:

```cpp
enum class DisplaySurfaceShape
{
    plane,
    curved_plane,
    hemisphere,
    custom_mesh,
};
```

Avec une config associee du type:

- `shape`
- `physical_width`
- `physical_height_policy`
- `distance`
- `anchor`
- `curvature`
- `custom_mesh_asset`
- `custom_uv_policy`

### Consequence importante pour `custom_mesh`

`custom_mesh` ne doit pas etre un cas a part completement different.

Il doit etre:

- une option de presentation du meme systeme

Donc:

- meme texture
- meme decision de mode
- meme logique d'ouverture/fermeture
- seule la projection finale change

### Forme d'architecture recommandee

La forme la plus propre a terme est probablement:

- `DisplaySurfaceSystem`
  - agrege les signaux
  - maintient l'etat
  - choisit le mode
  - applique la policy de tweak
  - produit la decision finale
- `DisplaySurfacePresenter`
  - gere visibilite
  - pousse la texture
  - construit ou selectionne le mesh de rendu
- `DisplaySurfaceMeshProvider`
  - `plane`
  - `curved_plane`
  - `hemisphere`
  - `custom_mesh`

Comme ca:

- la logique de detection ne depend jamais du backend
- la policy de tweak est centralisee
- et le choix du mesh peut etre profile-driven
- y compris plus tard par scene

### Conclusion pratique pour cette phase

Pour la partie `video / image`, la meilleure methode n'est pas:

- plus d'heuristiques locales dans chaque composant

La meilleure methode est:

- un detecteur central
- un petit state machine
- des scores de confiance
- des regles d'ouverture / maintien / fermeture
- et un `hold_last_surface` explicite pour les cas de boot / overlay.

---

## Prochaines actions concretes recommandees

Avant de coder massivement, je recommande maintenant:

1. valider:
   - fichier profil unique
   - activation conditionnelle par scene/contexte
2. definir en code un enum central `FramePresentationMode`
3. definir une structure `FrameCompositionDecision`
4. definir un petit schema `presentation.surface`
   - `shape`
   - `physical_size`
   - `sampling_policy`
   - `anchor`
5. definir la forme precise du `DisplaySurfaceSystem`
6. commencer par la phase 1:
   - surfaces 2D/video/image a taille stable

---

## Mini design technique concret

Cette section sert de pont direct vers l'implementation.

L'idee est:

- rester compatible avec les briques actuelles
- ne pas ouvrir un second systeme de profil parallele
- et pouvoir avancer par etapes sans casser l'existant

### 1. Evolution du profil unique

Le plus propre est d'etendre le profil existant au lieu de creer un nouveau store.

Aujourd'hui, [`Psx3dProfileStore`](E:/Projects/github/Live/R3000-Emu/src/emu/psx3d_profile_store.h) contient surtout:

- `analysis`
- `hotspots`
- `camera_candidates`
- `mode_rules`

La suite logique est d'ajouter un bloc `presentation`.

Forme cible:

```cpp
struct PsxPresentationProfileData
{
    struct SurfaceConfig
    {
        std::string mode_policy{"auto"};
        std::string match_policy{"near"};
        std::string close_policy{"hysteresis"};
        std::string buffering_mode{"auto"};
        std::string shape{"plane"};
        std::string anchor{"camera_locked"};
        std::string sampling_policy{"preserve_aspect"};
        float physical_width{320.0f};
        float distance{150.0f};
        float curvature{0.0f};
        std::string custom_mesh_asset{};
        bool allow_hold_last{true};
        bool allow_overlay_over_surface{true};
        bool allow_surface_with_3d{false};
    };

    struct ThresholdConfig
    {
        uint32_t video_open{4};
        uint32_t image_open{4};
        uint32_t direct_open{4};
        uint32_t close_decay{1};
        uint32_t hold_frames{8};
    };

    SurfaceConfig surface{};
    ThresholdConfig thresholds{};
};
```

Puis:

```cpp
struct Psx3dProfileData
{
    ...
    PsxPresentationProfileData presentation{};
};
```

Important:

- on garde un seul fichier profil
- `analysis` et `presentation` vivent cote a cote
- les `scene_rules` futurs pourront override seulement une partie de `presentation`

### 2. Runtime enums recommandes

#### Presentation globale

```cpp
enum class FramePresentationMode : uint8_t
{
    world_3d_only = 0,
    world_3d_plus_overlay,
    screen_surface_only,
    world_3d_plus_screen_surface,
    feedback_effect,
    mixed_special,
};
```

#### Surface ecran

```cpp
enum class DisplaySurfaceMode : uint8_t
{
    none = 0,
    video_fmv,
    static_image,
    direct_display,
    hold_last_surface,
    hybrid_reserved,
};
```

#### Policy de surface

```cpp
enum class DisplaySurfaceModePolicy : uint8_t
{
    auto_detect = 0,
    prefer_video,
    prefer_image,
    prefer_direct,
    prefer_hold,
    force_surface_on,
    force_surface_off,
};
```

#### Forme de la surface

```cpp
enum class DisplaySurfaceShape : uint8_t
{
    plane = 0,
    curved_plane,
    hemisphere,
    custom_mesh,
};
```

#### Buffering de surface

```cpp
enum class DisplaySurfaceBufferingMode : uint8_t
{
    auto_detect = 0,
    mono,
    double_buffer,
    triple_buffer,
};
```

#### Source de texture

```cpp
enum class DisplaySurfaceSourceKind : uint8_t
{
    none = 0,
    display_area_copy,
    mdec_display,
    cpu_written_display,
    latched_previous_surface,
};
```

### 3. Structs runtime recommandes

#### Entree brute du detecteur

```cpp
struct DisplaySurfaceInputs
{
    gpu::DisplayConfig display{};
    gpu::CpuVramWriteInfo last_cpu_write{};
    gpu::FrameStats frame_stats{};
    bool has_mdec_display_content{false};
    uint32_t vram_write_seq{0};
    uint32_t frame_index{0};
};
```

#### Etat persistant du detecteur

```cpp
struct DisplaySurfaceState
{
    DisplaySurfaceMode mode{DisplaySurfaceMode::none};
    DisplaySurfaceModePolicy policy{DisplaySurfaceModePolicy::auto_detect};
    DisplaySurfaceBufferingMode buffering_mode{DisplaySurfaceBufferingMode::auto_detect};
    gpu::DisplayConfig latched_display{};
    uint32_t open_frame{0};
    uint32_t last_refresh_frame{0};
    uint32_t last_vram_seq{0};
    uint32_t last_cpu_write_seq{0};
    uint32_t video_score{0};
    uint32_t image_score{0};
    uint32_t direct_score{0};
    uint32_t hold_until_frame{0};
    uint32_t last_presented_buffer_id{0};
};
```

#### Decision finale

```cpp
struct DisplaySurfaceDecision
{
    bool visible{false};
    DisplaySurfaceMode mode{DisplaySurfaceMode::none};
    DisplaySurfaceModePolicy policy{DisplaySurfaceModePolicy::auto_detect};
    DisplaySurfaceBufferingMode buffering_mode{DisplaySurfaceBufferingMode::auto_detect};
    DisplaySurfaceSourceKind source_kind{DisplaySurfaceSourceKind::none};
    DisplaySurfaceShape shape{DisplaySurfaceShape::plane};
    gpu::DisplayConfig display{};
    uint16_t source_x{0};
    uint16_t source_y{0};
    uint16_t source_w{0};
    uint16_t source_h{0};
    bool use_letterbox_uv{false};
    bool hold_previous_surface{false};
    bool allow_overlay_over_surface{true};
    bool allow_surface_with_3d{false};
    float physical_width{320.0f};
    float distance{150.0f};
    float curvature{0.0f};
    std::string custom_mesh_asset{};
};
```

#### Etat de buffering observe

```cpp
struct DisplaySurfaceBufferState
{
    uint32_t visible_buffer_id{0};
    uint32_t producer_buffer_id{0};
    uint32_t history_buffer_count{0};
    bool display_reads_from_recent_write{false};
    bool page_flip_detected{false};
    bool multi_buffer_rotation_detected{false};
};
```

### 4. Classes recommandes

#### `DisplaySurfaceSystem`

Responsabilite:

- agreger les signaux GPU/core
- maintenir l'etat court terme
- appliquer les policies de tweak
- produire une `DisplaySurfaceDecision`

API cible:

```cpp
class DisplaySurfaceSystem
{
  public:
    void reset();
    void set_profile(const PsxPresentationProfileData* profile);
    DisplaySurfaceDecision evaluate(const DisplaySurfaceInputs& in);

  private:
    DisplaySurfaceState state_{};
    const PsxPresentationProfileData* profile_{nullptr};
};
```

#### `DisplaySurfacePresenter`

Responsabilite:

- recevoir une `DisplaySurfaceDecision`
- gerer texture, mesh, visibilite, UV
- ne jamais prendre lui-meme une decision de mode

API cible:

```cpp
class IDisplaySurfacePresenter
{
  public:
    virtual ~IDisplaySurfacePresenter() = default;
    virtual void ApplyDecision(const DisplaySurfaceDecision& decision) = 0;
};
```

Exemple UE5:

```cpp
class UPSXSurfaceComponent : public USceneComponent, public IDisplaySurfacePresenter
{
    ...
    void BindGpu(gpu::Gpu* gpu);
    void ApplyDecision(const DisplaySurfaceDecision& decision);
    void RefreshSurfaceTexture(const DisplaySurfaceDecision& decision);
    void RebuildSurfaceGeometryIfNeeded(const DisplaySurfaceDecision& decision);
};
```

#### `DisplaySurfaceMeshProvider`

Responsabilite:

- encapsuler la fabrication/selection de mesh
- separer proprement la geometrie de la logique de detection

API logique:

```cpp
struct DisplaySurfaceMeshLayout
{
    DisplaySurfaceShape shape{DisplaySurfaceShape::plane};
    float physical_width{320.0f};
    float physical_height{240.0f};
    float curvature{0.0f};
    float distance{150.0f};
    std::string custom_mesh_asset{};
};
```

### 5. Flux d'execution recommande

Ordre par frame:

1. lire les signaux du core
2. remplir `DisplaySurfaceInputs`
3. appeler `DisplaySurfaceSystem::evaluate()`
4. produire une `DisplaySurfaceDecision`
5. appliquer la decision au presenter
6. seulement si necessaire:
   - copier VRAM
   - mettre a jour texture
   - rebuilder la geometrie

Important:

- la decision doit arriver avant toute operation couteuse
- on ne copie pas la VRAM "au cas ou"

### 6. Politique de rebuild recommandee

#### Rebuild geometrie uniquement si

- `shape` change
- `physical_width` change
- `curvature` change
- `distance` impose une topologie differente
- `custom_mesh_asset` change

#### Refresh texture uniquement si

- la source est visible
- et que la source a vraiment change
  - `vram_write_seq`
  - refresh video
  - nouveau latch image

Donc:

- geometie rare
- texture frequente
- decision tous les ticks

### 7. Politique `custom_mesh`

Je recommande:

- `plane`, `curved_plane`, `hemisphere` = generation dynamique
- `custom_mesh` = asset fourni

Et surtout:

- les UV restent gouvernes par la meme logique
- le `custom_mesh` ne change pas les regles de detection
- il change seulement la projection finale

### 8. Politique de buffering

Il faut absolument prevoir:

- `mono`
- `double_buffer`
- `triple_buffer`
- `auto_detect`

Pourquoi c'est important:

- certains jeux affichent directement le buffer visible
- d'autres dessinent dans un backbuffer puis flip
- d'autres reutilisent plusieurs surfaces ou buffers d'historique
- et certains effets speciaux ressemblent a du triple buffering logique meme si la machine n'a pas un "triple buffer API" explicite

Le systeme de surface ne doit donc pas seulement choisir:

- "quelle texture afficher"

Il doit aussi savoir:

- de quel buffer elle vient
- si ce buffer est visible, en preparation, ou historique
- si on doit afficher le dernier visible, le prochain draw buffer, ou une surface latchee

La bonne approche:

- le `DisplaySurfaceSystem` calcule un `DisplaySurfaceBufferState`
- puis applique une `buffering_mode policy`

Comportement attendu:

- `mono`
  - un seul buffer pertinent
  - la surface lit toujours la meme source logique
- `double_buffer`
  - il faut distinguer:
    - buffer visible
    - buffer de draw
  - et savoir quand garder l'ancien pendant une transition
- `triple_buffer`
  - il faut aussi pouvoir conserver un historique plus ancien
  - utile pour certains effets de feedback / blend / motion blur
- `auto_detect`
  - mode par defaut
  - le systeme observe flips, rotations et relectures VRAM

Important:

- `buffering_mode` n'est pas un detail purement graphique
- c'est une partie de la detection de mode
- car un meme pattern de writes n'a pas la meme signification en mono ou en double buffer

### 9. Integration progressive recommandee

Pour eviter une migration trop violente:

#### Etape A

- ajouter `DisplaySurfaceSystem`
- sans encore supprimer les anciennes briques UE5 si elles aident la migration

#### Etape B

- ajouter un vrai presenter unique en UE5
- brancher la decision centrale dessus

#### Etape C

- retirer progressivement les anciennes heuristiques locales
- ne garder qu'un seul chemin de surface

#### Etape D

- brancher plus tard un presenter alternatif si besoin
- sans toucher a la logique centrale

### 10. Recommendation de placement de code

Je recommande:

- logique pure et profils:
  - `src/emu/`
- presentation UE5:
  - `integrations/ue5/...`

Concretement:

- `DisplaySurfaceSystem`
  - plutot dans `src/emu/`
- `DisplaySurfaceDecision`
  - partageable entre core et integration
- `UPSXSurfaceComponent`
  - dans l'integration UE5

Comme ca:

- le CLI pourra aussi reutiliser la meme logique de decision si utile
- UE5 fera le rendu aujourd'hui
- et un backend futur pourra reprendre exactement la meme decision

### 11. Prochaine implementation recommandee

Ordre de code le plus sain:

1. ajouter les enums/structs communs
2. ajouter `presentation` dans le profil unique
3. implementer `DisplaySurfaceSystem`
4. ajouter le calcul de `DisplaySurfaceBufferState`
5. brancher les signaux GPU existants
6. ajouter `UPSXSurfaceComponent`
7. brancher les policies de mode, de tweak et de buffering

---

## Conclusion

Le projet n'a plus besoin d'un "renderer 3D de plus".

Le projet a besoin d'un **systeme de composition PSX->VR**:

- profile-driven,
- observable,
- generique,
- explicable,
- avec des patches typés.

La bonne strategie n'est donc pas:

- "on ajoute plein de flags par jeu"

La bonne strategie est:

- "on definit une grammaire de patches, un moteur de composition, puis on profile les jeux dedans"

Si on fait ca proprement, on pourra absorber:

- les jeux 3D classiques,
- les overlays 2D,
- les FMV,
- les ecritures directes framebuffer,
- les effets de feedback,
- et les adaptations VR

dans un meme systeme coherent.
