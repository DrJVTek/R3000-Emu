# Debug Soul Reaver (France) — LibCrypt Stall

## Mise à jour 2026-04-08 (soir) — ancien stall LibCrypt cassé

Validation runtime faite via le MCP stdio du CLI :
- breakpoint sur `0x80039240`
- lecture RAM de `cd2e0`, `cd2e4`, `cd5bc`, `ceec0`, `st`, `vi`
- comparaison avec l’ancien état bloqué

Résultat :
- au point `0x80039240`, on atteint maintenant :
  - `cd2e0 = 0`
  - `cd2e4 = 0`
  - `cd5bc = 258 (0x102)`
  - `st = 4`
  - `vi = 5`
- c’est précisément la divergence qu’on voulait corriger par rapport à l’ancien build où `cd2e4` restait bloqué à `0x800C12F8`
- c’est aussi aligné avec DuckStation, qui arrivait au même point avec `cd2e4 = 0`

Encore plus important :
- après quelques steps supplémentaires, `ceec0` passe bien à `1`
- `cd2e0/cd2e4` reviennent aux callbacks libcd normaux (`0x80036C84 / 0x80036DC0`)
- donc la chaîne LibCrypt n’est plus coincée sur le clear final de `cd2e4`

Conclusion mise à jour :
- le vieux stall LibCrypt historique est corrigé
- le correctif générique côté CD async/INT1 quand `want_data_ == 0` était bien le bon axe
- s’il reste encore un problème Soul Reaver après ça, ce n’est plus ce même blocage

### Progression confirmée après LibCrypt

Trace MCP supplémentaire après le breakpoint `0x80039240` :
- `vi` est écrit à `0xFFFFFFFF` pendant la sortie de la boucle (`pc` vu dans les logs runtime autour de `0x80039288`)
- puis `st` change bien de `4` vers `8`
- le watchpoint RAM sur `st` a capturé :
  - write `0x000D19B0 = 8`
  - `pc = 0x80039060`

Donc Soul Reaver ne reste plus coincé dans `state 4` après la fin du check LibCrypt.
Le flow reprend bien dans la machine d’états du jeu.

## Mise à jour 2026-04-08 (nuit) — progression confirmée jusqu'à `state 9`

Le point important de ce tour : il n'y a pas de nouveau stall immédiat juste après `state 8`.

Validation MCP runtime :
- breakpoint sur `0x80039240` pour accrocher la sortie du vieux stall LibCrypt
- watchpoint RAM sur `st` (`phys 0x000D19B0`)

Séquence observée :
- `state 4` avec `cd2e4 = 0`
- puis write `st = 8` à `pc = 0x80039060`
- puis write `st = 9` à `pc = 0x80039060`

État observé à l'entrée en `state 9` :
- `st = 9`
- `vi = 0xFFFFFFFF`
- `ceec0 = 1`
- `cd2e0 = 0x80036C84`
- `cd2e4 = 0x80036DC0`
- `cd5bc = 258`

Pendant cette phase :
- `BIGFILE.DAT` continue d'être lu
- `DMA3` continue de tourner
- les `LBA` continuent d'avancer

Conclusion :
- le jeu ne reste pas bloqué juste après LibCrypt
- il atteint bien `state 9`

### Interprétation de `state 9`

Décompilation Ghidra de `FUN_80038F34()` :

- `case 8` :
  - fait l'initialisation de transition
  - puis pose `DAT_800d19ac = 9`
- `case 9` :
  - appelle `FUN_80038e60(&DAT_800d173c, &DAT_800d19ac, uVar4)`

Décompilation de `FUN_80038e60()` :
- exécute un tick complet du jeu :
  - update input/render/runtime
  - incrémente `DAT_800d1738`
  - relance `FUN_8002f0a0(...)`

Donc les writes répétées de `st = 9` vues par le watchpoint ne sont pas une preuve de hang.
Le dispatcher principal réécrit simplement l'état courant à chaque itération.

### Note sur les logs `CD_DLVR FAIL`

Les warnings `CD_DLVR FAIL` vus pendant `state 9` ne signifient pas à eux seuls une panne de livraison.
Dans les traces capturées, ils correspondent surtout au cas :
- `due = 0`
- `flags = 1`
- `ready = 1`

Autrement dit :
- le secteur suivant n'est simplement pas encore arrivé à échéance
- pendant ce temps le jeu continue de consommer `BIGFILE.DAT`

À ce stade, ces warnings ne doivent plus être lus comme la cause du vieux stall LibCrypt.

## Symptome
Le jeu joue PUBLOGO.STR et CRYLOGO.STR puis se bloque indéfiniment en `st=4`.
Musique SPU OK, GPU dessine, mais aucune transition de scène.

## Root Cause identifiée
Le jeu attend la completion du **LibCrypt check** dans une boucle tight :
```c
// Adresse: dans FUN_80038f34, case 4
do {
    DAT_800ceec0 = (DAT_800cd2e0 == 0 && DAT_800cd2e4 == 0);
} while (DAT_800ceec0 == 0);
FUN_80074530("GOT KEY");  // ← jamais atteint
```

`cd2e0` et `cd2e4` (phys 0x000CD2E0, 0x000CD2E4) sont des **pointeurs de callbacks async**.
Les callbacks sont appelés par le CD event handler (`FUN_800bfb34`) quand un CDROM INT1/INT3 arrive.

## Pourquoi ça bloque

## Mise à jour runtime (2026-04-08)

Les anciennes hypothèses "plus aucun IRQ CD après GetLocP" et "cd2e4 reste non-zero sans callback"
ne sont plus exactes sur le build courant.

### Ce qui est maintenant prouvé

- Les IRQ/événements CD continuent après l'entrée en phase LibCrypt :
  - `GetLocP` continue de retourner des positions qui avancent.
  - les logs `CD_IRQ set_irq(1)` continuent longtemps après la transition.
- Le test "skip du délai de changement de vitesse 2x→1x" **ne résout pas** le stall.
- Le callback final côté jeu **est bien exécuté** :
  - la fenêtre `0x800C1290..0x800C135C` tourne réellement,
  - `pc=0x800C12F8` est observé en runtime,
  - `cd2e0` y retombe bien à `0`.

### État stable observé pendant le stall

Autour de `vbl=3050..3400`, on voit :

```text
st=4, vi=5
cd2e0=0
cd2e4=0x800C12F8
ceec0=0
cd5bc=258
```

Le point important :
- `cd2e0` n'est plus le verrou,
- `cd2e4` reste chaîné vers le callback final,
- `ceec0` ("GOT KEY") ne passe jamais à `1`,
- donc la chaîne LibCrypt tourne mais **ne converge pas**.

### Nouvelle hypothèse principale

Le bug restant n'est probablement plus dans la livraison brute des secteurs.
Le problème est plus haut niveau :
- soit une valeur/événement attendu par la chaîne `0x800C12D4/0x800C12F8` ne change jamais,
- soit notre sémantique de réponse CD (`cd5bc`, status/résultat, ordre des callbacks)
  n'est pas assez fidèle pour que le callback final se termine.

## Lead fort COP0 debug (2026-04-08)

Nouveau signal très important :

- `FUN_80038F34()` appelle `FUN_800C121C()` juste avant `FUN_800C128C()` (ReadS LibCrypt).
- `FUN_800C121C()` programme plusieurs registres COP0 inhabituels juste avant le check.
- Dans notre CPU, les registres COP0 `3/5/9/11/...` sont encore traités comme de simples slots
  de tableau avec une nomenclature type R4k (`ENTRYLO1`, `PAGEMASK`, `COMPARE`) alors que
  sur PS1/R3000A cette zone correspond aux registres debug/breakpoint (`BPC/BDA/BDAM/BPCM/DCIC...`).
- Le code contient déjà un fix spécial Soul Reaver pour `reg 9 = BDAM`, preuve que cette zone
  est réellement utilisée par le jeu.
- En revanche, aucune vraie logique de breakpoint/watchpoint hardware COP0 n'est visible pour l'instant :
  seule l'exception `BREAK` d'instruction existe.

### Interprétation

Le vrai blocage Soul Reaver pourrait être un manque d'émulation des registres debug COP0,
pas un problème de lecture CD brute.

La chaîne callback LibCrypt continue bien à tourner, mais elle peut attendre un état/compteur
issu de ces registres debug qui n'arrive jamais chez nous.

## Résultat MCP décisif (2026-04-08, soir)

Le bridge MCP + step hooks a permis un test runtime propre sur la **boucle d'attente réelle** :

```c
// FUN_80038F34, state 4
do {
    DAT_800ceec0 = (DAT_800cd2e0 == 0 && DAT_800cd2e4 == 0);
} while (DAT_800ceec0 == 0);
```

Boucle identifiée en assembleur :

```text
80039240: lw v1, DAT_800cd2e0
8003924c: lw v0, DAT_800cd2e4
80039254: or v1, v1, v0
80039258: sltiu v1, v1, 1
8003925c: sw v1, DAT_800ceec0
80039260: beq v1, zero, 0x80039240
```

### Test 1

Hooks MCP sur la boucle :
- écrire `cd2e4 = 0` à `pc=0x80039240`
- écrire `ceec0 = 1` à `pc=0x80039258`

**Résultat** :
- sortie immédiate du stall,
- passage en `state=9`,
- `vidx = -1`,
- retour aux callbacks libcd normaux (`cd2e0=0x80036C84`, `cd2e4=0x80036DC0`).

### Test 2 (minimal)

Hook MCP **seul** :
- écrire `cd2e4 = 0` à `pc=0x80039240`

**Résultat** :
- le hook tire une fois (`hit_count=1`),
- le jeu passe quand même en `state=9`,
- donc **forcer `ceec0` n'est pas nécessaire**.

### Conclusion forte

Le verrou n'est plus "la boucle state 4 elle-même".
Le verrou est plus précisément :

- `DAT_800cd2e0` tombe bien à `0`,
- mais `DAT_800cd2e4` reste non-null trop longtemps,
- et c'est **le seul élément qui suffit à maintenir le stall**.

En pratique, le bug restant est donc très probablement :

1. soit le callback final LibCrypt ne clear jamais `cd2e4` chez nous,
2. soit un événement/résultat attendu par ce callback n'arrive jamais, donc il ne prend jamais son chemin de sortie.

Le test MCP montre que le reste de la transition vers le menu est sain dès que `cd2e4` redevient `0`.

## Comparaison DuckStation GDB (2026-04-08, nuit)

Connexion réussie au serveur GDB DuckStation (`127.0.0.1:2346`) via le workflow MCP/Python.

### Point de comparaison 1 : boucle principale `0x80039240`

Breakpoint placé sur `0x80039240` dans DuckStation.

État lu au hit :

```text
pc    = 0x80039240
st    = 4
vi    = 5
cd2e0 = 0
cd2e4 = 0
ceec0 = 0
cd5bc = 0x102
```

Conclusion très forte :
- au **même point** où notre émulateur boucle avec `cd2e4 = 0x800C12F8`,
- DuckStation arrive avec `cd2e4 = 0`.

Donc la divergence utile est maintenant prouvée directement :
le vrai bug est bien que **notre chaîne ne termine jamais le clear de `cd2e4`**.

### Point de comparaison 2 : callback `0x800C12F8`

Breakpoint placé sur `0x800C12F8` dans DuckStation.

État lu au hit :

```text
pc    = 0x800C12F8
st    = 4
vi    = 5
cd2e0 = 0
cd2e4 = 0x800C12F8
ceec0 = 0
cd5bc = 0x102
```

Donc DuckStation passe bien par le même callback intermédiaire que nous.

### Watchpoint sur `cd2e4`

En continuant depuis `0x800C12F8` avec watchpoint écriture sur `0x800CD2E4`,
DuckStation montre une vraie **chaîne de réarmement** :

```text
0x800C12D4
0x800C12F8
0x800C131C
... puis à terme la boucle 0x80039240 voit cd2e4 = 0
```

PCs observés aux écritures :
- `0x800C12C4`  -> `cd2e4 = 0x800C12D4`
- `0x800C13B8`  -> `cd2e4 = 0x800C12F8`
- `0x800C1424`  -> `cd2e4 = 0x800C131C`

Le watchpoint perturbe probablement la progression complète (on interrompt chaque écriture),
donc on ne voit pas le `0` final directement dans cette boucle de watch.
Mais le breakpoint sur `0x80039240` prouve que **sans perturbation**, DuckStation finit bien
par arriver avec `cd2e4 = 0`.

### Interprétation mise à jour

Notre émulateur n'est pas juste en retard d'un callback.
Le problème est plus précis :

- DuckStation fait vivre `cd2e4` dans une petite machine de callbacks (`12D4 -> 12F8 -> 131C -> ... -> 0`)
- chez nous, cette chaîne finit bloquée avec `cd2e4 = 0x800C12F8`
- donc il manque probablement soit :
  - un réveil/callback guest après le réarmement à `0x800C12F8`,
  - soit un passage correct de `IRQ CD -> I_STAT/event BIOS -> callback libcd/game`.

## Automate exact de la chaîne LibCrypt (désassemblage SLES_020.24)

Le bloc `0x800C128C..0x800C14DC` a maintenant été désassemblé proprement.

### Séquence

- `FUN_800C128C()`
  - `cd2e0 = 0`
  - `cd2e4 = 0x800C12D4`
  - envoie commande `0x1B`
- `0x800C12D4`
  - `cd2e0 = 0x800C1340`
  - envoie `GetLocP (0x11)`
- `0x800C1340`
  - clear `cd2e0`
  - compare le `GetLocP` retourné à la cible stockée via COP0
  - écrit `cd2e4 = 0x800C12B0` **ou** `cd2e4 = 0x800C12F8`
- `0x800C12F8`
  - `cd2e0 = 0x800C13C0`
  - envoie `GetLocP`
- `0x800C13C0`
  - clear `cd2e0`
  - si la condition est bonne, écrit `cd2e4 = 0x800C131C`
- `0x800C131C`
  - `cd2e0 = 0x800C142C`
  - envoie `GetLocP`
- `0x800C142C`
  - clear `cd2e0`
  - boucle ou prépare la fin
  - cas de sortie : `cd2e0 = 0x800C14DC`, `cd2e4 = 0`, puis commande `0x1A`
- `0x800C14DC`
  - callback final async

### Conséquence importante

`0x800C12F8` n'est pas "la fin".
C'est un **réarmement intermédiaire** qui doit ensuite lancer un nouveau `GetLocP`
via `cd2e0 = 0x800C13C0`.

Donc si on reste bloqués avec `cd2e4 = 0x800C12F8`, cela veut dire :
- la chaîne a bien atteint un point intermédiaire valide,
- mais la callback suivante n'est jamais exécutée.

## Nouvelle preuve runtime (2026-04-08, tard)

Le build courant montre maintenant ceci :

- à `pc=0x800C13B4`, le jeu écrit bien `cd2e4 = 0x800C12F8`
- juste après, le handler guest `FUN_800bfb34` tourne encore
- mais dans cette fenêtre, il voit :
  - `irqf = 0`
  - `pend = 1`
  - `data_ready = 1`
  - `resp = 0`
- puis côté contrôleur, les `INT1` continuent bien d'être délivrées :
  - `SR_CD DELIVER ... read_lba=13952`
  - ...
  - `SR_CD DELIVER ... read_lba=14063`

### Interprétation utile

Le bug restant n'est donc plus :
- "plus de secteurs",
- ni "plus de set_irq(1)",
- ni "le contrôleur CD s'est arrêté".

Le bug restant ressemble maintenant à :

- le contrôleur continue à livrer les `INT1`,
- mais après le réarmement `cd2e4 = 0x800C12F8`,
- cette livraison n'est plus transformée en exécution effective de la callback guest suivante.

La cible la plus crédible est donc le pont :

`CD INT1 -> I_STAT / event BIOS -> FUN_800bfb34 -> (*cd2e4)()`

pas la lecture brute du CD.
  - un événement CD / statut permettant la transition `0x800C12F8 -> suivant`,
  - soit une sémantique de callback/ack qui empêche le réarmement suivant
  - soit un prérequis COP0/debug qui conditionne cette sous-chaîne

### Test MCP côté R3000-Emu : `0x800C131C` / `0x800C1424`

Test runtime ajouté sur notre émulateur :
- attendre le stall (`st=4`, `vi=5`, `cd2e4=0x800C12F8`)
- poser deux step hooks non destructifs :
  - `pc=0x800C131C`
  - `pc=0x800C1424`
- laisser tourner plusieurs secondes supplémentaires

Résultat :
- `hit_count(0x800C131C) = 0`
- `hit_count(0x800C1424) = 0`
- `cd2e4` reste bloqué à `0x800C12F8`
- `pc` reste dans la boucle `0x80039240..0x80039258`

Conclusion :
- chez nous, la chaîne ne progresse **jamais** au-delà de `0x800C12F8`
- donc le bug n'est pas une mauvaise exécution de `0x800C131C`/`0x800C1424`
- il est **avant**, dans ce qui devrait déclencher la transition `0x800C12F8 -> 0x800C131C`

### Chain d'événements attendue :
1. ReadS async au LBA 13952 (03:08:02) — LibCrypt sector range
2. CDROM livre secteur → `set_irq(INT1)` → `irq_callback_` → `i_stat |= bit 2`
3. CPU prend exception → BIOS handler → DeliverEvent(CDROM class)
4. Game's CD callback → `FUN_800bfb34` lit CDROM_REG3, ACK IRQ
5. Callback dispatch : `if (event & 4) (*cd2e4)(status, result)` → LibCrypt check step
6. Callback met cd2e4 = next_step ou cd2e4 = 0 (done)
7. Retour exception → main loop voit cd2e4 = 0 → sort

### Ce qui ne marche pas :
Après l'entrée en `st=4 / vi=5`, la séquence LibCrypt n'aboutit jamais :
- `cd2e0` finit par tomber à `0`,
- mais `cd2e4` reste non-null,
- et `ceec0` ne passe jamais à `1`.

## Hypothèses restantes

### 1. Sémantique de réponse CD incomplète
Le jeu semble continuer à avancer dans ses callbacks, mais le callback final
`0x800C12D4/0x800C12F8` ne trouve jamais la condition de sortie.
Suspects :
- contenu/status remonté via `cd5bc`
- ordre exact des callbacks
- résultat de `GetLocP` / ReadS dans cette phase

### 2. Différence de contrat plus haut niveau que le timing brut
Le délai moteur 2x→1x a été testé en "skip" complet : **aucun effet sur le stall**.
Il peut rester un sujet de fidélité, mais ce n'est pas la cause principale.

### 3. Le thread CD documenté n'est pas encore réellement actif
Le doc architecture dit "CDROM sector thread implémenté", mais dans le code courant
`start_sector_thread()` n'a pas de callsite, donc le CD reste piloté par `tick()`.
Ce n'est pas forcément le bug, mais c'est important pour ne pas raisonner sur
une architecture qui n'est pas encore réellement branchée.

## Adresses clés (Soul Reaver France SLES-02024)

| Adresse | Variable | Description |
|---------|----------|-------------|
| 0x800D19AC | st | Game state (6=video, 4=loading, 8=init menu, 9=menu) |
| 0x800D19B4 | vi | Video index (0=PUBLOGO, 1=CRYLOGO, 5=game data, -1=done) |
| 0x800CB6E4 | cb6e4 | Cinemax pointer (0 = normal during loading) |
| 0x800CD2E0 | cd2e0 | Async CD callback 0 (0 = done) |
| 0x800CD2E4 | cd2e4 | Async CD callback 1 (0 = done) — **STUCK non-zero** |
| 0x800CD5BC | cd5bc | CD sync status (written by FUN_800bfb34) |
| 0x800CEEC0 | ceec0 | LibCrypt "GOT KEY" flag |
| 0x800CEEC4 | base | Video entry table (stride 0x38, 6 entries) |
| 0x800D1788 | | Controller button state |
| 0x80034520 | tick | Game tick counter (from Timer 2 / RCnt2) |

## Video table (0x800CEEC4, stride 0x38)

| Entry | File | Type | Next |
|-------|------|------|------|
| 0 | \PUBLOGO.STR;1 | 0 (stream) | 1 |
| 1 | \CRYLOGO.STR;1 | 0 (stream) | **5** |
| 2 | \KAININT.STR;1 | 0 (stream) | -1 |
| 3 | \VERSE.STR;1 | 0 (stream) | 4 |
| 4 | \CREDITS.STR;1 | 0 (stream) | -1 |
| 5 | \kain2\game\psx\main | **1** (data) | -1 |

**vi=5 est le comportement NORMAL** pour la version France.
Le flow: PUBLOGO(0) → CRYLOGO(1) → game data(5) → state 4 → LibCrypt → menu.

## State machine (FUN_80038f34)

```
state 6: Play video sequence (vi loop)
  → when type[vi] != 0: go to state 4
  → when vi < 0: go to state 8

state 4: Load game data + LibCrypt check
  → load file at entry[vi]
  → FUN_800c128c: send ReadS async, set cd2e4 callback
  → while(cd2e0 || cd2e4): WAIT FOR LIBCRYPT ← STALL HERE
  → "GOT KEY" → continue
  → go to state 6 or 8

state 8: Init menu
state 9: Menu loop
```

## LibCrypt CD event handler

`FUN_800bfb34` (game's CD interrupt processor):
1. Set CDROM index 1: `*REG0 = 1`
2. Read IRQ type: `*REG3 & 7`
3. Read response FIFO from REG1
4. ACK IRQ: `*REG3 = 7`
5. Dispatch:
   - INT1 (type=1) → returns 4 → calls `(*cd2e4)(status, result)`
   - INT2 (type=2) → returns 2 → calls `(*cd2e0)(status, result)`
   - INT3 (type=3) → returns 1 or 2
   - INT5 (type=5) → returns 6 (error)

## SBI / LibCrypt data

32 SubQ replacements loaded from `Legacy of Kain - Soul Reaver (France).sbi`
LBA range: 14429-16022 and 42430-44167

DuckStation LibCrypt sequence (from log):
- 16 sequential ReadS at LBAs 14102-16173
- ~10 sectors per read with GetLocP polling
- COP0 BPC/BDA used as check counter
- After 16 checks → GetID → passes → loads continue

## Fixes appliqués (2026-04-07/08)

1. **GetLocP/GetQ** : `head_lba_` au lieu de `read_lba_` (SBI lookup correct)
2. **GetLocP seek position** : retourne `read_lba_` pendant seek/pending
3. **Queued command dispatch** : retiré `pending_irq_type_ == 0` du check
4. **Timer 2 thread** : ne skip plus sysclk/8 (ch==2)
5. **Timer 2 reset_at_target** : reset count à 0 dans le thread quand count >= target

## Nouveau diagnostic (2026-04-08 soir)

La chaîne LibCrypt a maintenant une forme précise :

- `FUN_800c128c` met `cd2e4 = 0x800C12D4` puis lance `ReadS`
- `0x800C12D4` met `cd2e0 = 0x800C1340` puis lance `GetLocP`
- `0x800C1340` compare la réponse et réarme `cd2e4 = 0x800C12F8`
- **point de divergence** : chez nous, après ce réarmement, `0x800C12F8` n'est jamais rappelée
- DuckStation, lui, continue ensuite vers `0x800C13C0`, `0x800C131C`, `0x800C142C`, puis finit par remettre `cd2e4 = 0`

### Cause probable côté émulateur

Dans `src/cdrom/cdrom.cpp`, la livraison des secteurs `INT1` passe par le chemin async :

- `pending_irq_type_ == 0x01`
- `set_async_irq(INT1, status)`
- puis plus tard `deliver_async_irq() -> set_irq(1)`

Mais dans le bloc `pending_irq_type_ == 0x01`, on avait cette logique :

- si `try_fill_data_fifo()` n'écrit rien,
- alors on `return` immédiatement,
- on réarme juste le secteur suivant,
- et on **saute complètement** `set_async_irq(INT1)`.

Or juste après `0x800C13B4`, les traces montrent :

- `want_data_ = 0`
- `data_ready_pending_ = 1`
- `pending_irq_type_ = 1`
- les secteurs continuent d'avancer
- `FUN_800bfb34()` continue d'être appelée
- mais `CDROM_REG3 & 7 == 0`, donc aucun nouvel `INT1` n'est visible côté jeu

Comme `try_fill_data_fifo()` retourne immédiatement quand `want_data_ == 0`, on a probablement ce bug :

**on supprime le callback `INT1` juste parce que le jeu ne demande pas les données FIFO à cet instant, alors que le vrai hardware continue quand même à livrer l'événement secteur-ready.**

### Patch candidat posé dans le code

Dans `src/cdrom/cdrom.cpp`, le `return` anticipé a été restreint :

- avant : skip de l'`INT1` dès que `try_fill_data_fifo()` n'écrivait rien
- maintenant : ce skip ne se fait plus quand `want_data_ == 0`

Autrement dit :

- `want_data_ == 0` doit seulement empêcher le remplissage FIFO/DMA,
- **pas** supprimer l'`INT1` secteur-ready elle-même.

### État de validation

Le diagnostic statique est fort et colle exactement au symptôme.

Le rerun complet avec les bons arguments CLI a été pénible côté tooling PowerShell ce soir :

- `--bios=...` / `--cd=...` sont obligatoires
- certains lancements de test ont mal passé les arguments et n'ont pas atteint la phase LibCrypt

Donc :

- **le patch candidat est en place**
- **la validation runtime finale sur Soul Reaver reste à refaire proprement**
- mais c'est, de loin, la meilleure cause racine identifiée jusqu'ici

## Next steps

1. Isoler la chaîne runtime autour de `0x800C12D4/0x800C12F8`
2. Vérifier ce que cette chaîne attend exactement de `cd5bc` / des callbacks CD
3. Comparer cette phase avec DuckStation au même point, idéalement via MCP/GDB
4. Mettre à jour ce doc dès qu'un nouveau callback/pointeur de chaîne est identifié
