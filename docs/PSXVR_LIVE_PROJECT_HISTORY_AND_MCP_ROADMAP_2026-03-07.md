# PSXVR / PSX3D

## Historique du projet, systeme par tags, limites constatees, et nouvelle direction MCP

Date: 2026-03-07

---

## 1. But du projet

Le projet ne vise pas seulement a "emuler une PS1".

Le vrai objectif est plus ambitieux:

- emuler suffisamment fidelement une PS1 pour faire tourner de vrais jeux,
- reconstruire une representation 3D exploitable a partir du pipeline original PS1,
- afficher ensuite cette reconstruction dans Unreal Engine 5,
- puis, a terme, viser un runtime suffisamment performant pour de la VR standalone sur une machine de type `Quest 3`.

Autrement dit:

- emulation correcte,
- reconstruction 3D utile,
- integration UE5,
- et plus tard optimisation extreme.

Le projet est donc a la fois:

- un emu PS1,
- un outil de reverse engineering runtime,
- un pipeline de reconstruction 3D,
- et une future couche de compatibilite VR par jeu.

---

## 2. Pourquoi la PS1 est difficile pour la VR

La PS1 n'est pas un moteur 3D moderne.

Le jeu ne donne pas au GPU un "mesh monde" directement exploitable comme dans un moteur actuel.

Le pipeline reel ressemble plutot a:

- le jeu prepare ses donnees,
- le `GTE` transforme des vertices,
- le CPU reorganise / copie / subdivise / repackage ces donnees,
- le `DMA2` pousse une liste ordonnee vers le GPU,
- le GPU ne recoit au final que des primitives 2D/2.5D PS1.

Donc, si on veut retrouver une vraie structure 3D exploitable en UE5, il faut reconstituer la chaine causale:

- `GTE -> CPU -> RAM -> DMA2 -> GPU`

Le probleme est que:

- une partie de l'information 3D est detruite ou transformee,
- certains jeux font de la logique CPU supplementaire,
- certains polys deviennent des quads/triangles 2D "orphelins",
- certaines transformations ne sont plus triviales a relier a la source.

---

## 3. La premiere grande idee: la correlation par tags

La premiere architecture qui a vraiment debloque le projet a ete l'idee des `tags`.

### 3.1 Idee generale

Quand le `GTE` produit une face ou un vertex utile, on associe un identifiant logique a cette donnee.

Puis, quand cette donnee est stockee, recopier, ou transformee dans la chaine CPU/RAM/DMA/GPU, on essaye de faire survivre cette identite.

Au moment ou le GPU recoit une primitive, on cherche a savoir:

- cette primitive GPU vient-elle d'une face 3D passee par le GTE ?
- si oui, laquelle ?

### 3.2 Pourquoi c'etait puissant

Parce que cela permettait de sortir du mode "heuristique aveugle".

On ne cherchait plus seulement a deviner geometriquement qu'un triangle GPU "ressemble" a une face 3D.

On essayait de dire:

- "ce triangle GPU est probablement la projection de cette face GTE"

Cela a permis d'obtenir une premiere reconstruction 3D convaincante sur certains cas.

### 3.3 Le cas important: le drapeau

Au debut de cette procedure, on avait un cas frappant:

- le drapeau fonctionnait tres bien,
- quasiment `100%`,
- alors que le jeu complet ne suivait pas encore.

C'est un point important historiquement:

- cela prouve que l'idee du lien `GTE -> GPU` n'etait pas absurde,
- cela prouve aussi qu'on a casse des choses ensuite en complexifiant le systeme.

---

## 4. Le systeme "shadow GPU" et la reconstruction 3D

Pour exploiter ces tags, le projet a introduit une architecture parallele au GPU original.

### 4.1 Shadow GPU

On garde:

- le GPU emule "normal" pour le comportement PS1,
- et un `shadow GPU` / `Gpu3D` pour reconstruire de la geometrie UE5.

Ce `shadow GPU` recoit:

- les memes commandes GP0/GP1,
- mais avec des hints et des metadonnees supplementaires,
- afin de reconstruire des `DrawCmd3D`.

### 4.2 Differential tags

Le pipeline s'est appuye sur un encodage differentiel des tags:

- les hints sont transportes dans le flux,
- caches / resolus au moment du decode,
- puis utilises pour reconstruire une primitive 3D correlée.

Cela a donne un pipeline de type:

- `CPU/GTE side` -> genere hint / face index
- `DMA2 / GP0 side` -> decode du flux
- `Gpu3D` -> choisit un `face_idx`
- `UE5` -> reconstruit un mesh procedural a partir des vertices/normales/UV/couleurs reconstruits

### 4.3 Ce que ce pipeline a permis

Il a permis:

- de differencier primitives 3D et 2D,
- d'alimenter UE5 avec des triangles 3D reconstruits,
- de mesurer finement:
  - `3d`
  - `2d`
  - `tok_hint`
  - `tok_miss`
  - `miss_no_hint`
  - `miss_hint_stale`
  - `miss_decode_fail`

Ces compteurs ont ete essentiels pour comprendre ce qui marchait et ce qui ne marchait pas.

---

## 5. Ce qui a commence a casser

En pratique, a mesure que le systeme a grandi, on a vu apparaitre plusieurs problemes.

### 5.1 Les hints obsoletes

Un des problemes importants observes a ete:

- le hint existe,
- mais il est `stale`,
- donc il pointe vers une correlation qui n'est plus fiable dans le contexte courant.

Le symptome typique a ete:

- une scene partiellement reconstruite,
- puis une regression en beaucoup plus de `2d`,
- avec des compteurs du type `miss_hint_stale` qui explosent.

### 5.2 Les misses de decode

Autre famille de probleme:

- il y a bien tentative de hint,
- mais le decode ne parvient pas a reconstruire correctement la primitive.

Symptome:

- `miss_decode_fail`

### 5.3 Le fait que certains jeux sont plus compliques que d'autres

Ridge Racer a montre une verite fondamentale:

- certaines sequences du jeu suivent bien une logique "reconstructible" depuis le GTE,
- mais d'autres parties repackagent, subdivisent ou reordonnent davantage les donnees.

On ne peut donc pas se contenter d'une seule heuristique globale naive.

---

## 6. Pourquoi Ridge Racer est un tres bon cas de travail

Ridge Racer est devenu le cas d'etude central pour plusieurs raisons.

### 6.1 Il montre des succes et des limites

On y voit:

- du contenu 3D exploitable,
- des transitions menu/demo/gameplay,
- des structures repetitives,
- et des cas qui cassent les hypotheses simples.

### 6.2 Il est utile meme sans interaction

En CLI, il peut aller assez loin tout seul:

- logos,
- menu,
- mode demo

Cela permet:

- de faire de longues captures,
- de comparer les phases,
- de debugger sans dependre d'un workflow interactif immediat.

### 6.3 Il a expose une regression importante

Le projet a observe le fait suivant:

- une version simple du pipeline arrivait a faire marcher le drapeau,
- puis les evolutions suivantes ont degrade ce resultat,
- et certains correctifs ont meme rendu le drapeau "pire" visuellement.

Cela a donne un cas de regression ideal:

- un exemple tres visible,
- facile a discuter en live,
- et tres utile pour comprendre ce qu'on rate dans la reconstruction.

---

## 7. Le probleme central: il ne suffit pas d'etre "cible" pour etre "generique"

Le projet a mis en lumiere un vrai paradoxe:

- si on cible trop un jeu, on perd la genericite,
- si on reste trop generique, on ne reconstruit pas assez bien les cas reels.

La vraie question est donc:

- comment faire un systeme adaptable tout seul a plusieurs jeux,
- sans devoir recoder manuellement le pipeline a chaque fois,
- tout en acceptant qu'un profil par jeu sera probablement necessaire ?

La reponse actuelle du projet est:

- garder un socle runtime generique,
- mais accepter des profils par jeu,
- et deplacer l'analyse lourde hors ligne.

C'est la transition conceptuelle la plus importante du projet recent.

---

## 8. Pourquoi le runtime seul ne suffira probablement pas

Au debut, on pouvait esperer un systeme tres generique:

- on lance un jeu,
- le runtime observe tout,
- il apprend tout seul,
- il reconstruit tout seul.

En pratique, cette approche a de grosses limites:

- cout runtime eleve,
- heuristiques fragiles,
- trop de cas CPU/GTE/GPU differents selon les jeux,
- trop de risques de casser ce qui marchait deja.

Et surtout:

- le but final est la VR,
- donc le runtime devra etre leger.

Si on veut un jour tenir sur Quest 3, il faut reserver le runtime a ce qui est indispensable.

Donc la bonne direction devient:

- analyse lourde offline,
- exploitation legere runtime.

---

## 9. La nouvelle direction: analyse offline + MCP + Ghidra + LLM

La nouvelle direction du projet consiste a traiter la generation des profils comme un pipeline d'authoring.

### 9.1 Idee generale

Au lieu d'attendre que le runtime comprenne tout, on va construire une chaine d'outils qui permet de comprendre un jeu hors ligne.

Cette chaine combine:

- l'emulateur CLI,
- UE5 pour validation/acquisition interactive,
- Ghidra pour l'analyse statique,
- un LLM pour la synthese,
- et des serveurs MCP pour piloter tout cela proprement.

### 9.2 Le role du CLI

Le CLI devient le coeur de l'analyse lourde.

Pourquoi:

- il est scriptable,
- il peut tourner longtemps,
- il peut etre instrumente avec hooks/breakpoints,
- il peut produire des traces riches,
- il ne pollue pas le runtime final.

Le CLI est l'endroit ideal pour:

- trouver hotspots DMA2,
- suivre les chemins GTE,
- mesurer les misses,
- capturer les donnees necessaires pour construire un profil.

### 9.3 Le role de UE5

UE5 ne disparait pas.

Il reste utile pour:

- la validation visuelle,
- l'acces a des etats interactifs,
- le debug "en live",
- les tests de camera libre / VR,
- la presentation du projet.

Mais UE5 ne doit pas porter l'analyse la plus lourde.

UE5 devient surtout:

- un mode acquisition,
- un mode validation,
- un mode demonstration.

### 9.4 Le role de Ghidra

Ghidra permet de comprendre la structure statique du jeu:

- quelles fonctions construisent les paquets GPU,
- quelles fonctions chargent les registres GTE,
- ou se trouve la logique camera,
- ou se trouvent des structures de lumieres,
- quelles fonctions semblent faire de la subdivision CPU.

Le projet a explicitement retenu une contrainte utile:

- quand on trouve quelque chose dans Ghidra, il faut poser des noms/commentaires/labels,
- pour ne pas reperdre la connaissance plus tard.

Cela doit faire partie du pipeline, pas juste du debug ponctuel.

### 9.5 Le role du LLM

Le LLM n'est pas la pour "faire tourner le jeu".

Il est la pour:

- recoller les indices,
- synthétiser les traces runtime et l'analyse statique,
- proposer des hypotheses,
- generer ou enrichir les profils `.psx3dprof`,
- documenter les raisons des decisions.

Le LLM devient donc un assistant d'authoring offline, pas une dependance runtime.

### 9.6 Le role de MCP

L'idee MCP est de relier proprement les outils:

- `CLI MCP`
  - lancer l'emulateur,
  - poser des breakpoints/hooks,
  - lire RAM/registres/etat GPU,
  - exporter des traces,
  - generer un profil.

- `UE5 MCP`
  - conduire des scenes interactives,
  - recadrer/observer la scene,
  - valider visuellement une hypothese,
  - faire des captures plus "humaines".

- `Ghidra MCP`
  - lister des fonctions,
  - suivre des xrefs,
  - poser labels/commentaires,
  - aider a transformer une intuition runtime en connaissance structuree.

L'interet n'est pas juste "avoir des outils".

L'interet est:

- un pipeline coherent,
- reproductible,
- qui produit de meilleurs profils,
- avec une part manuelle beaucoup mieux structuree.

---

## 10. Le profil par jeu: pourquoi c'est devenu central

Le projet a convergé vers l'idee qu'il faut un profil par jeu.

### 10.1 Pourquoi

Parce qu'un meme socle generique ne suffit pas a capturer:

- la logique exacte des packet builders,
- les racines camera,
- les cas de subdivision,
- les chemins GTE particuliers,
- et les conventions internes du jeu.

### 10.2 Convention retenue

Le projet a retenu une convention simple:

- le nom du fichier porte l'identite du jeu

Exemple:

- `SCUS-943.00.psx3dprof`

### 10.3 Pourquoi ce choix est bon

Parce qu'il permet:

- une bibliotheque de profils par jeu lisible,
- une identification humaine simple,
- une couche de validation par hash en plus si besoin,
- une exploitation runtime claire.

### 10.4 Chemin cible

Le chemin par defaut actuel du runtime/CLI est:

- `profiles/psx3d/<game_id>.psx3dprof`

C'est la bonne direction:

- un dossier dedie,
- des profils nommes par jeu,
- pas de gros fichier racine fourre-tout.

---

## 11. Ce que doit contenir un bon `.psx3dprof`

Un bon profil doit etre utile au runtime, mais aussi suffisamment riche pour etre regenere/ameliore.

Il doit donc rester structure.

### 11.1 Couche runtime fast path

Cette couche doit contenir:

- les PCs deja analyses,
- les hotspots valides,
- les regles de correlation stables,
- les racines camera valides,
- les meta-infos exploitables tres vite.

Le but:

- charger le profil,
- appliquer des regles deja connues,
- faire seulement des validations legeres.

### 11.2 Couche offline knowledge

Cette couche peut contenir:

- labels de fonctions,
- notes reverse,
- hypotheses,
- observations utiles a la regeneration du profil.

Cette couche sert au pipeline d'authoring, pas au fast path.

### 11.3 Ce que le profil ne doit pas devenir

Le profil ne doit pas devenir:

- un dump brut de traces,
- un fourre-tout de notes non structurees,
- une logique de patch VR globale,
- une dependance a Ghidra ou au LLM.

Le profil doit rester:

- lisible,
- versionnable,
- runtime-friendly.

---

## 12. Ce qu'on veut reconstruire de plus en plus proprement

Le projet ne veut pas seulement retrouver des triangles.

Il veut retrouver progressivement les vrais attributs utiles d'un mesh.

### 12.1 Positions

Retrouver les vertices 3D corrects est la base.

### 12.2 Normales

Le projet a explicité une regle importante:

- ne pas recalculer les normales si le jeu fournit deja une information exploitable,
- utiliser les normales source d'abord,
- recalcul geometrique seulement en fallback.

### 12.3 UV

Les UV doivent etre preservés autant que possible depuis la source PS1.

### 12.4 Couleurs

Les couleurs par vertex sont importantes:

- pour la fidelite visuelle,
- pour retrouver certaines formes d'eclairage PS1,
- pour l'aspect retro du rendu.

### 12.5 Etat materiau minimal

Il faut aussi conserver:

- texture page,
- CLUT,
- flags de blending,
- tout ce qui est necessaire pour reconstruire une approximation solide du materiau.

---

## 13. La prochaine grande etape technique: le cache de mesh en espace objet

Une intuition importante du projet est apparue ensuite:

- si on controle assez bien ce qui sort du GTE,
- on controle aussi les transforms,
- donc on peut peut-etre reconnaitre des objets 3D originaux,
- et les cacher une fois pour toutes.

### 13.1 Pourquoi c'est important

Aujourd'hui, sans cache objet, on risque de:

- reconstruire la geometrie encore et encore,
- repayer la logique a chaque frame,
- perdre des performances,
- et limiter l'avenir VR.

### 13.2 Bonne direction

Le bon cache n'est pas un cache "ecran".

Ce n'est pas:

- "je cache ce qui a ete dessine a l'ecran"

Le bon cache est un cache:

- d'espace objet,
- ou au moins d'espace stable avant camera finale.

### 13.3 Ce que le cache doit conserver

Le projet a retenu que le cache canonique doit pouvoir porter:

- positions,
- normales,
- UV,
- couleurs,
- topologie,
- etat materiau minimal.

Autrement dit:

- un vrai mesh canonique,
- pas seulement trois vertices.

### 13.4 Pourquoi c'est si prometteur

Si cela marche:

- on reconnait un objet ou un chunk stable,
- on le stocke une fois,
- ensuite on ne met a jour que son transform,
- et on s'approche d'une vraie scene 3D exploitable en UE5/VR.

### 13.5 Pourquoi c'est difficile

Il faut auto-detecter:

- dans quel espace sont les vertices,
- si l'objet est stable ou dynamique,
- si le CPU subdivise/deforme/copie encore la geometrie,
- quand invalider le cache.

Donc:

- tres prometteur,
- mais necessite une analyse plus fine.

---

## 14. Les lumieres

Le projet veut aussi, a terme, retrouver les lumieres.

### 14.1 Pourquoi

Parce qu'une reconstruction 3D sans semantique d'eclairage restera limitee.

Trouver:

- lumiere ambiante,
- vecteurs directionnels,
- couleurs de light,
- structures de lighting du jeu,

permettrait ensuite:

- de piloter UE5,
- ou au minimum d'alimenter des shaders/proxies plus intelligents.

### 14.2 Contrainte VR

Sur Quest 3, de vraies lumieres UE5 sont couteuses.

Donc la bonne strategie n'est probablement pas:

- "mettre des vraies lights partout"

Mais plutot:

- retrouver la semantique de lighting,
- puis choisir a runtime entre:
  - vraie lumiere UE,
  - approximation shader,
  - ou couche desactivee selon la plateforme.

---

## 15. Les patches VR par jeu

Il a aussi ete retenu qu'un profil 3D ne suffira pas a rendre un jeu confortable en VR.

Il faudra probablement une couche de patch VR par jeu.

### 15.1 Exemples de patchs

- recentrage camera,
- decallage du point de vue,
- HUD repositionne,
- menus remis a une profondeur lisible,
- corrections de comportement camera,
- ajustements de confort,
- eventuellement certains patches ROM.

### 15.2 Pourquoi il faut separer cette couche

Parce que:

- la reconstruction 3D et la compatibilite VR ne sont pas la meme chose,
- et melanger les deux rendrait le systeme vite illisible.

Le projet veut donc garder des couches distinctes:

- `psx3dprof`
- `lighting`
- `VR patch layer`

---

## 16. Le mode camera UE5

Une partie recente du travail a ete de preparer UE5 pour mieux explorer la scene.

### 16.1 Pourquoi

Pour du dev live, il faut pouvoir:

- se balader dans le monde reconstruit,
- observer les mesh sous tous les angles,
- presenter le projet de maniere impressionnante,
- et preparer plus tard un vrai mode VR.

### 16.2 Modes introduits

Le projet a prepare / reintroduit une logique de tracking camera avec:

- un mode legacy,
- un mode world locked / free roam,
- un mode VR recenterable.

L'idee n'est pas de finaliser la VR tout de suite, mais de rendre l'exploration plus utile des maintenant.

---

## 17. Ce qu'on a appris des regressions

Les regressions recentes ont appris plusieurs choses importantes.

### 17.1 Ce qui marchait peut etre casse facilement

Le cas du drapeau a montre que:

- un systeme simple peut parfois etre meilleur localement,
- la complexification sans garde-fou peut casser un cas deja bon.

### 17.2 Il faut des indicateurs clairs

Les compteurs comme:

- `tok_miss`
- `miss_no_hint`
- `miss_hint_stale`
- `miss_decode_fail`

sont devenus essentiels.

Ils permettent:

- de voir si le probleme est absence de hint,
- hint stale,
- echec de decode,
- ou autre pathologie.

### 17.3 Il faut documenter au fur et a mesure

Une grosse lecon du projet est qu'il faut documenter:

- les hypotheses,
- les profils,
- les chemins statiques dans Ghidra,
- les raisons des correctifs.

Sinon, la connaissance se perd entre les iterations.

---

## 18. Ce qu'on veut faire maintenant

Le projet entre dans une nouvelle phase.

### 18.1 Ce qu'on garde

On garde le socle existant:

- shadow GPU,
- correlation GTE/GPU,
- compteurs et logs,
- profil par jeu,
- integration UE5,
- analyse de camera,
- future direction mesh cache / lighting.

### 18.2 Ce qu'on change de philosophie

On ne considere plus que le runtime doit tout apprendre tout seul.

On accepte que:

- l'analyse lourde soit offline,
- le profil par jeu soit central,
- l'authoring passe par un pipeline d'outils,
- et que la compatibilite avance jeu par jeu.

### 18.3 La feuille de route immediate

Court terme:

- stabiliser le systeme de profils,
- utiliser `profiles/psx3d/<game>.psx3dprof`,
- consolider le pipeline `CLI + Ghidra + LLM`,
- remettre UE5 en mode validation propre,
- debugger les regressions majeures avec un cadre plus propre.

Moyen terme:

- construire le `CLI MCP`,
- brancher correctement `Ghidra MCP`,
- structurer un `UE5 MCP`,
- enrichir les profils par jeu,
- commencer a reconnaitre de vrais objets/meshes.

Long terme:

- cache mesh en espace objet,
- extraction des lumieres,
- patch VR par jeu,
- optimisation runtime lourde,
- JIT `x86` et `ARM`,
- cible VR standalone.

---

## 19. Position de conception retenue

La position actuelle du projet peut se resumer ainsi:

### Ce qu'on assume

- un systeme 100% generique et 100% automatique est probablement irrealisable ou trop couteux,
- il faut un socle runtime generique,
- mais aussi une couche de profils par jeu,
- et une couche d'authoring offline forte.

### Ce qu'on refuse

- faire du runtime une usine a gaz d'analyse permanente,
- melanger reconstruction 3D, lumieres, et patches VR dans un seul blob,
- perdre la connaissance entre deux sessions.

### Ce qu'on veut

- un pipeline propre,
- des profils de plus en plus riches,
- un runtime de plus en plus leger,
- et une progression solide vers de la vraie VR exploitable.

---

## 20. Conclusion

Le projet a deja franchi plusieurs etapes importantes:

- une emulation PS1 fonctionnelle,
- une premiere reconstruction 3D via correlation par tags,
- un shadow GPU,
- une integration UE5,
- des cas concrets reussis,
- des regressions instructives,
- et maintenant une vision plus claire de la suite.

La grande transition conceptuelle est la suivante:

- avant: reconstruire le plus possible directement au runtime
- maintenant: analyser hors ligne, comprendre le jeu, produire un profil par jeu, et reutiliser ce savoir au runtime

C'est probablement la seule voie serieuse si l'objectif final reste:

- de vrais jeux PS1,
- dans un monde 3D reconstruit,
- avec une ambition VR,
- et des contraintes de performance reelles.

---

## 21. Resume ultra-court pour slides

Si une version tres courte est necessaire pour une slide d'introduction:

- La PS1 ne fournit pas directement une scene 3D moderne.
- Le projet reconstruit cette scene a partir de la chaine `GTE -> CPU -> RAM -> DMA2 -> GPU`.
- Une premiere strategie par `tags` a permis des succes concrets, mais a aussi revele des limites et regressions.
- La nouvelle direction consiste a deplacer l'analyse lourde hors ligne.
- Le runtime devient profile-driven.
- Les outils cibles sont: `CLI`, `UE5`, `Ghidra`, `LLM`, relies par `MCP`.
- Objectif final: une reconstruction 3D exploitable et, a terme, une VR PS1 performante.
