# STR Video Streaming — État du Debug

> **⚠️ CLAUDE: LIRE CE FICHIER AU DÉBUT DE CHAQUE SESSION DE DEBUG STR !**
> **Mettre à jour ce fichier APRÈS chaque découverte ou changement.**
> **NE PAS ajouter de hooks debug sans d'abord relire ce doc.**

---

## État actuel (2026-04-02, session Claude Opus — analyse DMA IRQ coalescence)

### Baseline confirmée intacte (commit deca519)
- **Frame 1** : 200/200 MBs ✅
- **Frame 2** : 200/200 MBs ✅  
- **Frame 3** : 188/200 MBs (98%)
- **Frames 4+** : stall
- **IMPORTANT** : hello_strplay met ~163 secondes d'émulation pour démarrer ReadS.
  Il faut `--timeout-ms=180000` minimum, pas 30000 !

### Découverte session (2026-04-02, Claude Opus)

#### 1. DICR byte handlers implémentés et testés
- `read_u8` et `write_u8` pour DPCR (0x1F8010F0) et DICR (0x1F8010F4)
- **hello_strplay n'utilise PAS SB pour DICR** — que des écritures 32-bit
- Le handler est correct mais sans effet sur hello_strplay
- Peut servir pour Tekken ou d'autres jeux PsyQ qui utilisent SB

#### 2. DMA IRQ coalescence = cause racine confirmée par traces
Trace DMA3IRQ (hello_strplay ReadS, lba 109-112) :
```
lba=109 header(8w):  old_mf=0 → new_mf=1, irq_fired=1 ← SEUL IRQ qui fire
lba=109 payload(504): old_mf=1 → new_mf=1, irq_fired=0 ← coalesced (même secteur, attendu)
lba=110 header:       old_mf=1 → new_mf=1, irq_fired=0 ← coalesced (cross-sector!)
lba=110 payload:      old_mf=1 → new_mf=1, irq_fired=0
lba=111 header/payload: idem
lba=112: game ack DICR → mf=0, puis header fire à nouveau
```
**Résultat** : `data_ready_callback` fire 1 fois pour 4 secteurs au lieu de 1 fois/secteur.

#### 3. Cause de la coalescence cross-sector
- Notre DMA est **instantané** (0 cycles). Sur vrai PS1, DMA3 prend des centaines de cycles.
- Pendant StCdInterrupt (interrupts désactivés), TOUS les DMA3 d'un secteur completent.
- Le cdrom_->tick(112000) depuis DMA3 fait avancer le CD et queue le prochain INT1.
- CDROM INT1 → StCdInterrupt cascade : le prochain secteur se traite dans le MÊME handler.
- master_flag reste à 1 pendant toute la cascade → pas d'edge 0→1 → DMA IRQ perdu.

#### 4. DuckStation : architecture DMA/CDROM très différente
- DuckStation NE tick PAS le CD depuis DMA3
- `CheckForSectorBufferReadComplete()` appelé DANS `DMARead()` (avant CompleteTransfer)
- Missed INT1 redeliver avec 5000 cycles de délai
- `current_read_sector_buffer` SAUTE au write pointer quand INT1 est délivré
- DMA prend du temps réel (tick count retourné et ajouté au CPU)

#### 5. Tentatives et résultats
| Tentative | Résultat | Pourquoi |
|-----------|----------|----------|
| Retirer CD tick de DMA3/0/1 | 0 frames | CD starve — pas assez de cycles via Bus::tick() seul |
| Reorder check_sector_read_complete avant dma_finish | 200/200/188 | Pas d'effet |
| Auto-ack ch3 flag après dma_finish | 0 frames (quand appliqué à tous ch) | Casse GPU DMA2 |
| Auto-ack ch3 SEULEMENT | 0 frames (re-testé 180s) | Le handler a BESOIN du flag pour dispatcher data_ready_callback |
| want_data_=0 dans check_sector_read_complete | 0 frames | Casse BFRD (interdit par design) |
| try_redeliver_sector depuis check_complete | 200/200/188 | try_redeliver exige pending IRQ existant |

### Prochaine étape exacte

### Fix appliqué : DICR byte handlers (read_u8 + write_u8) ✅
- Résultat : **200/200/200** (vs 200/200/188 avant)
- PsyQ `StCdInterrupt` utilise `SB` à DICR+2 (0x1F8010F6) pour toggler ch3 DMA IRQ enable
- Non-last sector : SB 0x82 (ch1+master, ch3 OFF) → DMA3 IRQ supprimé
- Last sector : SB 0x8A (ch1+ch3+master, ch3 ON) → DMA3 IRQ fire → `data_ready_callback`
- Le handler est aussi ajouté pour read_u8 (LBU) — nécessaire pour le read-modify-write

### Stall frame 4 — analyse en cours
- `StCdInterrupt` boucle à pc=0x80016038/3C avec `dicr=0x00080000` (ch3 ON, master OFF)
- Le handler DMA GPU (pc=0x80068300) écrit hardcodé `0x04840000` → clobber ch3/ch1/master
- Le SB de StCdInterrupt écrit tout byte 2 → clobber les bits des autres handlers
- L'intercalage BIOS (CDROM bit 2 AVANT DMA bit 3) cause : SB met ch3 → SW clobber ch3
- Sur vrai PS1, le SW (GPU DMA handler) tourne AVANT le SB (StCdInterrupt) car dans des passes séparées
- Le `cdrom_->tick(112000)` coalesce les deux IRQ dans la même passe

### Prochaine étape frame 4
1. Comprendre exactement comment `dicr_` arrive à `0x00080000` (ch3 sans master)
2. Le defer INT1 du CD tick devrait séparer les passes — vérifier pourquoi ça ne suffit pas
3. Possibilité : le handler DMA GPU fire à chaque VBlank (pas seulement pendant STR) et clobber en continu

### NE PAS REFAIRE
- Changer les constantes de timing (112000, 20000/MB) — déjà optimisé
- Re-vérifier CPU shifts, zigzag, IDCT, scale table — tous confirmés corrects
- Modifier `sb_r_` directement — toutes les variantes testées ont échoué
- Retirer les ticks DMA3/DMA1 — sans eux, 0 frames
- Mettre want_data_=0 dans check_sector_read_complete — casse le boot
- Utiliser `--timeout-ms=30000` — **hello_strplay a besoin de 180000 minimum**

### NE PAS REFAIRE
- Changer les constantes de timing (112000, 20000/MB) — déjà optimisé
- Re-vérifier CPU shifts, zigzag, IDCT, scale table — tous confirmés corrects
- Modifier `sb_r_` directement — toutes les variantes testées ont échoué
- Retirer les ticks DMA3/DMA1 — sans eux, frame 2 tombe à 79/200

## État actuel baseline (commit 0f4723a)

### Ce qui marche ✅
- Boot BIOS complet (no HLE, no fast-boot)
- CD-ROM 8 sector buffers (comme DuckStation)
- XA audio filtering correct (après read_lba_++)
- STR streaming ReadS : tous les secteurs livrés correctement
- **Frame 1** : 200/200 MBs, fifo_out=38400 ✅
- **Frame 2** : 200/200 MBs, fifo_out=38400 ✅
- Jeu stable (pas de crash)

### Ce qui ne marche PAS encore ❌
- **Frame 3** : 188/200 MBs (98% — ~1-2 secteurs manquants)
- **Frames 4+** : le jeu bloque après frame 3 (plus de traitement CD)

### Nouveau résultat important (2026-04-02, outil `str_dump.py` et référence disque)
- Nouveau script utile :
  - `scripts/str_dump.py`
  - sait maintenant :
    - dumper un `.STR` brut
    - résoudre directement un fichier ISO9660 depuis une `CUE/BIN` via `--iso-file /COPY.STR`
    - comparer la référence disque aux traces runtime (`ISORAW`, `CDUSER`, `CDFIFO`, `DMA3DATA`)
    - auto-aligner la comparaison
- Point clé découvert :
  - le mauvais résultat précédent venait d'un **mauvais `lba_base` de comparaison**, pas d'une mauvaise lecture CD
  - avec `--auto-align --align-source VIDEOHDR`, l'outil retrouve :
    - `best lba base: 85`
  - ce `85` correspond bien au début réel de `/COPY.STR` sur le disque de `hello_strplay`
- Preuve runtime :
  - sur `logs/str_pipeline_baseline.timeline.txt`
  - avec référence directe `hello_strplay.cue + /COPY.STR`
  - la comparaison donne ensuite :
    - `compared entries: 53`
    - `mismatches: 0`
- Conséquence forte :
  - le pipeline de lecture disque/CD sur la fenêtre de test est **correct bit-for-bit**
  - les données suivantes matchent bien la source de vérité disque :
    - `ISORAW`
    - `CDUSER`
    - `CDFIFO`
    - `DMA3DATA` header
    - `DMA3DATA` payload
  - donc le bug restant n'est **pas** :
    - un mauvais LBA lu
    - un mauvais extrait secteur
    - un mauvais transfert `DMA3`
- Conclusion mise à jour :
  - la lecture CD + exposition FIFO + `DMA3` sont maintenant validées sur la fenêtre baseline
  - le verrou restant se resserre encore plus sur l'étage supérieur :
    - `StCdInterrupt`
    - `StGetNext`
    - `StFreeRing`
    - `data_ready_callback`
    - et/ou la consommation `DMA0 -> MDEC -> DMA1`

### Nouveau résultat important (2026-04-02, recouvrement `strNextVlc()/StFreeRing()` vs frame courante)
- Relecture directe du source guest :
  - [`hello_strplay/strplay.c`](E:/Projects/PSX/nolibgs_hello_worlds/hello_strplay/strplay.c)
- Ordre réel dans `strDoPlayback()` :
  - `DecDCTin(...)`
  - `DecDCTout(...)`
  - `strNextVlc(...)`
  - `strSync(...)`
- Donc le player :
  - lance la decode de la frame courante
  - prépare déjà la frame suivante via `strNextVlc()`
  - **avant** d'attendre la fin complète de la frame courante
- Dans `strNextVlc()` :
  - `next = strNext(...)`
  - `DecDCTvlc(next, ...)`
  - `StFreeRing(next)`
- Corrélation runtime :
  - à `lba=117`, `StCdInterrupt` est encore bloqué avec :
    - `cur_frame=3`
    - `exp_sec=7`
    - `slot_ptr=0x801F0020` puis `0x801F0040`
  - mais le `StFreeRing(next)` déclenché depuis `strDoPlayback` part déjà avec :
    - `a0=0x801F5A00`
    - `ra=0x800105E8`
  - et plus tard un autre reset massif repart avec :
    - `a0=0x801F0360`
    - `ra=0x800105E8`
- Point important :
  - l'argument `next` passé à `StFreeRing()` n'est **pas** le slot courant visible du ring (`0x801F0020` / `0x801F0040`)
  - il pointe nettement plus loin dans le buffer logique
  - donc `StFreeRing(next)` ne libère pas juste "le secteur courant", il agit sur le bloc/frame déjà sélectionné par `strNext()`
- Interprétation mise à jour :
  - le verrou restant ressemble de plus en plus à un conflit de recouvrement entre :
    - `strNextVlc()/StFreeRing(next)` qui avancent/libèrent la frame suivante
    - et `strCallback()/StCdInterrupt()` qui sont encore en train de finir la frame courante
  - autrement dit :
    - le pipeline guest semble plus optimiste que ce que notre ordonnancement callbacks/IRQ tolère encore
    - on libère logiquement "trop tôt" du point de vue de `StCdInterrupt`, même si le code est correct sur hardware

### Nouveau résultat important (2026-04-02, fenêtre `lba=117..130`)
- Trace directe de la table `libetc/intr_dma` :
  - slot `3` reçoit `0x80019218` (`data_ready_callback`)
  - donc le callback libcd fautif est bien câblé sur **DMA3**, pas sur `DMA1`
- Conséquence :
  - la piste "faux ready déclenché par un callback DMA1" est désormais éliminée
  - le problème reste bien du côté `DMA3 header/payload` et de l’IRQ DMA3 qui finit par réveiller `data_ready_callback()` au mauvais moment
- Relecture directe du binaire `hello_strplay.bin` :
  - `frame 3` sur disque = `lba 109..120`
  - `frame 4` sur disque = `lba 121..131`
  - donc à `lba=130`, le secteur réel sur disque est `frame=4 sec=8`
- Disassembly PSYQ + traces runtime :
  - la branche `StCdInterrupt` `0x8001646C -> 0x800164A8/AC` n'est pas un clear arbitraire
  - elle reset l'état quand le header reçu n'appartient déjà plus à `Stframe_no/Stsector_offset` attendus
  - dans notre run cassé :
    - `exp_sec=7`
    - `cur_frame=3`
    - mais le secteur de reprise appartient déjà à `frame 4`
- Nouvelle instrumentation utile :
  - pendant les nombreuses entrées `strCallback()` à `lba=120/121`, on lit systématiquement :
    - `StCdIntrFlag=0`
    - `StFinalSector=0`
    - `Stframe_no=3`
    - `Stsector_offset=7`
  - donc le callback MDEC tourne bien, mais **libcd n'a pas encore armé de reprise `StCdInterrupt()`**
- Le ring CD interne confirme le vrai moment de perte :
  - overwrite commence à `lba=126`
  - puis continue à `127`, `128`, `129`, `130`
  - donc `frame3 sec7..10` sont déjà perdus **avant** le réarmement `BFRD=1` à `lba=130`
- Correctif structurel appliqué :
  - la redelivery/résume du ring choisit maintenant le **plus ancien secteur encore bufferisé** (FIFO par `lba`) au lieu du "prochain slot physique" après wrap
  - preuve :
    - avant, la reprise à `lba=130` relisait un secteur tardif (`frame4 sec7`)
    - maintenant, elle reprend sur le plus ancien survivant (`frame4 sec1`)
- Conclusion :
  - ce correctif FIFO élimine un vrai bug de reprise tardive
  - **mais il ne suffit pas** à sauver `frame 3`, car les secteurs manquants étaient déjà écrasés avant le réarmement
  - le verrou principal restant est donc en amont :
    - cadence relative entre la progression CD brute et le moment où le player/libcd redevient capable de consommer avant le wrap 8 secteurs

### Nouveau résultat important (2026-04-01, point de méthode rapide)
- Plusieurs essais "propres" ont été refaits pour éviter le brute force sur `hello_strplay`:
  1. **Retirer toute avance du lecteur CD depuis DMA1/DMA3**
     - idée: laisser le CD avancer uniquement via `Bus::tick()` / `Cdrom::tick()`
     - résultat runtime:
       - `frame 1 = 200/200`
       - `frame 2 = 81/200`
       - `frame 3 = 78/200`
     - conclusion:
       - le principe architectural est correct
       - mais notre horloge globale actuelle ne facture pas encore assez le temps réellement consommé par le pipeline `CPU/DMA/MDEC`
       - retirer brutalement toute compensation temporelle casse plus tôt
  2. **Remplacer cette avance DMA par une "dette de cycles" globale**
     - idée: ne plus appeler `cdrom_->tick()` depuis DMA, mais ajouter un coût au temps global du bus
     - résultat runtime:
       - reste autour de `200/79/78`
     - conclusion:
       - la simple dette ajoutée localement n'aligne pas encore la cadence réelle observée
  3. **Empêcher l'écrasement des vieux secteurs quand la queue 8 secteurs est pleine**
     - idée: préserver l'ordre FIFO en "drop newest" plutôt qu'en overwrite
     - résultat runtime:
       - pas d'amélioration du flux vidéo
       - le player revient toujours trop tard et repart sur un header tardif
     - conclusion:
       - ce n'est pas le levier principal à lui seul
- **Baseline restaurée après essais**:
  - `frame 1 = 200/200`
  - `frame 2 = 200/200`
  - `frame 3 = 188/200`
- Conclusion de méthode:
  - le moyen rapide **n'est pas** de retoucher encore des constantes de timing CD une par une
  - le bon axe est de traiter le problème comme un **bug de budget temporel système / cadence de callbacks**, visible par le fait que:
    - sans compensation, la queue CD déborde beaucoup trop tôt
    - avec la baseline actuelle, on revient à `188/200`, donc plus près du vrai comportement
  - prochaine voie non brute-force:
    - comparer la cadence exacte `INT1 -> DMA3(8) -> DMA3(504) -> callback slice -> réarmement BFRD`
      avec DuckStation / matériel attendu
    - ou modéliser correctement le coût global du pipeline `DMA/MDEC/callback` sans faire avancer le CD "au hasard"

### Conclusion statique importante (2026-03-30, revue DuckStation + core)
- **Le problème restant est structurel dans notre modèle de timing DMA/CD, pas dans les données STR.**
- Relecture ciblée :
  - `src/r3000/bus.cpp`
  - `src/cdrom/cdrom.cpp`
  - `duckstation/src/core/dma.cpp`
  - `duckstation/src/core/bus.h`
  - `duckstation/src/core/cdrom.cpp`
- Écart clé #1 :
  - DuckStation ne fait **pas** avancer le lecteur CD depuis `DMA3`.
  - `DMA::TransferDeviceToMemory<Channel::CDROM>()` appelle `CDROM::DMARead()`, puis ne facture que le coût RAM/DMA :
    - `Bus::GetDMARAMTickCount(word_count)`
    - soit un coût très faible et proportionnel aux mots (`word_count + ((word_count + 15) / 16)`)
  - chez nous, `Bus::exec_dma3_transfer()` :
    - vide le FIFO CD
    - lève `dma_finish(3)`
    - puis fait un **gros** `cdrom_->tick(112000)` par DMA3 streaming
  - donc `DMA3` joue à la fois le rôle de transfert mémoire **et** d'horloge du lecteur, ce que DuckStation ne fait pas
- Écart clé #2 :
  - DuckStation traite la fin de lecture buffer **dans `CDROM::DMARead()`** :
    - copie des bytes
    - `CheckForSectorBufferReadComplete()`
    - puis seulement ensuite `CompleteTransfer()`
  - chez nous, `Bus::exec_dma3_transfer()` appelle `cdrom_->check_sector_read_complete()` **après** :
    - `dma_finish(3)`
    - et après le `cdrom_->tick(112000)`
  - conséquence probable :
    - la redelivery INT1 / promotion suivante est programmée trop tard
    - donc `data_ready_callback` arrive quand `e08` a déjà avancé
    - ce qui colle avec `logs/str_ready_focus_120.txt` (`lba=112`, `e08=3`, `e10=0`) et le saut de `slot1/slot2`
- Écart clé #3 :
  - DuckStation expose `DRQSTS` via `request_register.BFRD`
  - chez nous, `Cdrom::status_reg()` bit6 vaut `!is_fifo_empty()`
  - donc le helper PsyQ qui poll `CDROM_REG0 & 0x40` ne voit pas la même condition que sur le vrai hardware / DuckStation
- Conclusion de design :
  - la solution n'est pas de retoucher encore une constante locale `112000`
  - la solution est de :
    1. donner au CDROM une vraie API type `DMARead(words, word_count)` comme DuckStation
    2. faire `check_sector_read_complete()` **à l'intérieur** de cette lecture DMA, avant `dma_finish(3)`
    3. retirer le `cdrom_->tick(112000)` post-`DMA3`
    4. ne plus faire avancer le lecteur CD depuis `DMA1 MDEC_OUT`
    5. corriger `DRQSTS` pour refléter `BFRD`, pas l'occupation brute du buffer
- En bref :
  - notre core fait aujourd'hui avancer le **producteur** (lecteur CD) depuis les **consommateurs** (`DMA3`, `DMA1`)
  - DuckStation garde l'arrivée des secteurs pilotée par les événements CDROM, et ne fait payer à DMA que son coût mémoire
  - c'est très probablement la vraie raison du `188/200 MBs`

### Nouveau résultat important (2026-03-30, IRQ INT1 vs BFRD)
- **Bug confirmé dans le core CDROM** :
  - le chemin `tick()` supprimait complètement l'`INT1` suivante si `want_data_==0`
  - en pratique, si le jeu clear `BFRD` pendant qu'une frame est en cours de decode, le lecteur continuait d'avancer mais sans nouvelle notification secteur
- Correctif appliqué dans `src/cdrom/cdrom.cpp`
  - `want_data_` ne gate plus la livraison de l'`INT1`
  - `BFRD` continue seulement de gate le remplissage du FIFO CPU
- Preuve avant correctif (`logs/str_cd_ring_focus_120.stderr.txt`) :
  - après `Frame decode END: 200 MBs` (frame 2), on voyait :
    - `Request reg write=0x00`
    - puis `advance #33 pre lba=117 ... want=0`
    - puis une longue suite de `advance` sans nouveau `Request=0x80`
  - le lecteur avançait "en silence"
- Preuve après correctif (`logs/str_cd_ring_fix1_120.stderr.txt`) :
  - après la frame 2, on voit maintenant :
    - `advance #33 pre lba=117 ... want=0`
    - **mais aussi** des `CDROM IRQ push` continus même quand `want_data_=0`
    - puis plus tard un nouveau `Request reg write=0x80` à la ligne `15817`
    - et de nouveaux `push` secteur (`lba=128`, `lba=129`)
- Conclusion :
  - le bug "plus d'IRQ CD quand `BFRD` vaut 0" était **réel**
  - il est maintenant corrigé
  - **mais il n'était pas l'unique cause** du blocage STR

### Effet du correctif
- Le symptôme a changé :
  - avant : le flux CD avançait sans plus aucune notification secteur visible
  - maintenant : les `INT1` continuent bien de tomber, même avec `BFRD=0`
- Cependant :
  - `Frame 3` reste à **188/200 MBs**
  - après cette frame, on observe surtout une **tempête de `Request reg write=0x00`**
  - avec très peu de retours à `Request reg write=0x80`
- Interprétation :
  - on a bien supprimé une perte d'IRQ côté core
  - le blocage restant est plus haut niveau, probablement dans la façon dont le code PsyQ/player réagit à la cadence des `INT1` quand il est encore occupé à finir la frame précédente

### Nouveau résultat important (2026-03-30, callsites `Request=0x00/0x80`)
- Hook temporaire sur les écritures `1F801803.Index0` avec capture du `PC`
  - artifact : `logs/str_req_pc_120.stderr.txt`
- Callsites identifiés :
  - `0x800161B8` = `StCdInterrupt()` écrit `Request=0x00`
  - `0x800161D8` = `StCdInterrupt()` réarme avec `Request=0x80`
  - `0x80018064` = handler BIOS/PsyQ CD générique (`BIOS_1_OBJ_64`) écrit seulement `Request=0x00`
- Corrélation avec le source `hello_strplay/strplay.c` :
  - `StCdInterrupt()` n'est appelé que depuis `strCallback()`
  - et `strCallback()` est la callback `DecDCToutCallback`, donc **une callback de fin de slice MDEC**
  - en mode 24-bit :
    - `if (StCdIntrFlag) { StCdInterrupt(); StCdIntrFlag = 0; }`
- Ce qu'on observe en runtime :
  - avant la fin de frame 2 :
    - séquence normale par secteur :
      - `0x80018064 -> 0x00`
      - `0x800161B8 -> 0x00`
      - `0x800161D8 -> 0x80`
  - juste après :
    - pour `read_lba=118..127`, on voit **presque uniquement `0x80018064 -> 0x00`**
    - `StCdInterrupt()` ne revient qu'après `Frame decode END: 188 MBs`
    - puis un nouveau `0x800161D8 -> 0x80` apparaît enfin à `read_lba=128`
- Interprétation forte :
  - les **IRQ secteur brutes** continuent bien d'arriver
  - mais leur **consommation PsyQ** via `StCdIntrFlag -> strCallback -> StCdInterrupt()` n'arrive plus à suivre pendant la frame 3
  - plusieurs `INT1` brutes sont donc vraisemblablement **coalescées** en une seule notification utile côté player
- Conclusion :
  - le bug restant n'est plus "le CDROM ne livre pas"
  - le bug restant n'est plus "l'INT1 n'est pas générée"
- le bug est maintenant localisé sur la **cadence relative** entre :
  - arrivée des `INT1` secteur
  - callbacks slice (`DecDCToutCallback`)
  - et réarmement `BFRD` via `StCdInterrupt()`

### Nouveau résultat important (2026-03-30, fausse piste `sb_r_` / buffer visible)
- Revue ciblée DuckStation + essais de correction sur `cdrom.cpp`
  - objectif : rapprocher notre modèle du couple :
    - `current_write_sector_buffer`
    - `current_read_sector_buffer`
  - idée testée :
    - faire suivre `sb_r_` au secteur livré par l'`INT1`
    - et/ou rapprocher `DRQSTS/BFRD` du modèle DuckStation
- Résultats obtenus :
  1. **Variante agressive (suivi direct du secteur livré)**
     - artifact : `logs/str_release_visible_fix.log`
     - résultat :
       - frame 1 = `200/200`
       - puis le flux part en vrille très tôt
       - overwrite massif du ring CD
     - conclusion : **régression**, fix rejeté
  2. **Variante plus proche DuckStation**
     - artifact : `logs/str_release_duckish_fix.log`
     - résultat :
       - frame 1 = `200/200`
       - frame 2 = `200/200`
       - frame 3 = **`78/200 MBs`**
     - trace utile :
       - à `lba=123`, le header `DMA3 8 words` repart bien
       - mais ensuite le callback slice revient trop tard
       - `read_lba/data_lba` sautent déjà à `125`
       - et le `DMA3 504 words` de la frame suivante n'est plus programmé au bon moment
     - conclusion : ce n'est **pas** le bon fix non plus
- Décision :
  - retour à la baseline saine précédente (celle qui reste à `188/200` sur frame 3)
  - ne **pas** conserver le suivi direct de `sb_r_` sur chaque `INT1`
- Conclusion forte :
  - le problème restant n'est pas "on pointe le mauvais buffer visible" **à lui seul**
  - le vrai verrou est plus haut :
    - `StCdInterrupt()` / `strCallback` reviennent **trop tard** par rapport au flux secteur
    - quand ils reviennent, ils ne programment déjà plus le bon payload complet (`504 words`)
  - autrement dit :
    - toucher seulement `sb_r_` déplace le bug
    - mais ne corrige pas la cadence logicielle qui provoque le trou

### Tentatives invalidées (2026-03-30, revue structurelle CDROM)
- Deux corrections "DuckStation-like" plus agressives ont été essayées puis **rejetées** :
  1. **Auto-clear `BFRD` / `request_ &= ~0x80` dans `check_sector_read_complete()`**
     - effet observé :
       - tempête immédiate de `Request reg write=0x00`
       - `CD_RING overwrite` massif
       - le player ne retombe plus sur une cadence exploitable
     - conclusion :
       - dans notre modèle actuel, transplanter directement l'auto-clear de DuckStation casse le flot
       - la divergence restante n'est donc pas corrigeable par ce seul changement local
  2. **Repointage de `sb_r_` vers le secteur livré au moment exact de l'INT1**
     - artifact : `logs/str_release_sbdeliver.log`
     - effet observé :
       - reprise très instable
       - `Request=0x00` en boucle
       - plus de progression STR exploitable
     - conclusion :
       - faire suivre `sb_r_` "brutalement" au secteur livré ne suffit pas
       - il faut corriger la politique complète de visibilité du buffer, pas juste le pointeur
- Décision :
  - retour à la **baseline saine** :
    - `frame 1 = 200/200`
    - `frame 2 = 200/200`
    - `frame 3 = 188/200`
  - les deux essais ci-dessus ne doivent **pas** être repris tels quels

### Tentative invalidée (2026-03-30 nuit, sélection explicite du slot livré sur `INT1`)
- Essai :
  - après `try_fill_data_fifo()`, rechercher le slot dont `slot.lba == data_lba_`
  - puis forcer `sb_r_` sur ce slot au moment de `DataReady`
- Motivation :
  - rapprocher notre modèle de `current_read_sector_buffer = current_write_sector_buffer` chez DuckStation
- Résultat runtime :
  - la fenêtre utile `lba=109..125` reste **inchangée**
  - même wrap `slot 21 -> slot 0`
  - même `StFreeRing(slot 11)` à `lba=112`
  - même `StGetNext(slot 1)` à `lba=116`
  - puis la run finit en longue boucle tardive d'`INT1` avec `want=0`
  - artifacts :
    - `logs/str_timeline.txt`
    - `logs/str_release_current.stderr.txt`
- Conclusion :
  - le problème n'est pas juste "quel slot `sb_r_` pointe" au moment de `DataReady`
  - forcer le slot livré n'empêche pas la purge du span `11..20`
  - patch **retiré**

### Nouveau résultat important (2026-03-30, secteurs "sautés" confirmés)
- Le pattern de LBAs non livrés côté vidéo est **régulier et correct** :
  - `92, 100, 108, 116, 124, ...`
- Lecture directe du BIN `hello_strplay.bin` :
  - secteurs vidéo utiles :
    - `mode=0x02`
    - `file=0x00`
    - `channel=0x01`
    - `submode=0x48` (`data + realtime`)
  - secteurs sautés :
    - `mode=0x02`
    - `file=0x01`
    - `channel=0x01`
    - `submode=0x64` (`audio + form2 + realtime`)
- Conclusion :
  - les secteurs sautés par le core sont bien de **vrais secteurs XA audio realtime**
  - la piste "on saute par erreur des secteurs vidéo utiles" est **invalidée**

### Nouveau résultat important (2026-03-30, queue CD 8 secteurs)
- En relisant le chemin `ReadN advance`, on a trouvé un bug réel :
  - `clear_data()` était encore appelé à chaque nouveau secteur
  - si `BFRD=0`, cela effaçait le buffer READ courant avant que PsyQ ne revienne le consommer
  - conséquence : la queue ne pouvait jamais monter au-delà de `qdepth=1`
- Correctif appliqué :
  - **ne plus vider le READ buffer** dans `ReadN advance`
- Effet observé dans `logs/cdrom.log` :
  - avant :
    - `qdepth` restait à `1`
    - aucune croissance réelle de la queue
  - après :
    - `qdepth` monte bien `1 -> 8`
    - puis on observe des `CD_RING overwrite ... qdepth=8 qmask=0xFF`
- Conclusion :
  - le modèle "queue qui restait à 1" était bien **buggé**
  - la queue CD 8 secteurs fonctionne maintenant réellement
  - **mais la frame 3 reste à 188/200 MBs**, donc ce bug n'était pas l'unique cause

### Nouveau résultat important (2026-03-30, point précis où ça casse)
- Avec la queue 8 secteurs corrigée, le moment du décrochage est plus clair :
  - jusqu'à `lba=115`, le player fait ses paires DMA3 normales pour les secteurs vidéo :
    - `8 words`
    - puis `504 words`
  - à `lba=117`, les `INT1` continuent
  - à partir de `lba=118`, `BFRD` retombe à `0`, la queue commence à se remplir
  - à `lba=127`, on revoit seulement **un DMA3 de 8 words**
  - mais le **DMA3 de 504 words ne revient pas**
  - ensuite la queue monte à `8` puis overwrite
- Interprétation forte :
  - le problème restant n'est plus la lecture disque ni le filtrage XA
  - le problème est maintenant le **chemin logiciel qui ne relance plus le transfert complet du secteur**
  - autrement dit : on ne perd plus les secteurs à l'entrée, on perd le **drainage utile côté PsyQ/player**

### Tentative invalidée (2026-03-30, budget timing DMA1/MDEC)
- Test de réduction du budget artificiel `DMA1 MDEC_OUT`
  - tentative "DuckStation-like" : `2688 ticks / MB`
    - trop bas dans notre modèle actuel
    - en 135s de run, on n'atteint même plus la zone vidéo utile (`lba < 85`)
  - tentative intermédiaire : `8000 ticks / MB`
    - la run atteint bien la zone STR
    - mais `frame 3` reste `188/200 MBs`
    - et la queue finit quand même en overwrite
- Décision :
  - retour au budget précédent `20000 cycles/MB`
  - **ne pas repartir sur un "fix timing DMA1" à l'aveugle**

### Nouveau résultat important (2026-03-30, helper DMA3 PsyQ)
- Hook temporaire sur la programmation DMA3 (`CHCR`) :
  - artifact : `logs/str_dma3_reg_trace.txt`
- Tous les démarrages DMA3 vidéo observés passent par le même callsite :
  - `pc=0x80016B28`
  - Ghidra : helper PsyQ `C_011_OBJ_A20()` appelé depuis `C_011_OBJ_9C4()`
  - ce helper écrit `DPCR/MADR/BCR`, attend `CDROM_REG0 & 0x40`, puis arme `CHCR`
- Séquence observée :
  - `lba=109..115` : paires normales `8 words` puis `504 words`
  - `lba=116..127` : **plus aucun nouveau DMA3 n'est programmé**
  - `lba=128` : retour partiel avec **seulement** le DMA3 `8 words`
- Conclusion :
  - le bug restant n'est pas "le `504 words` disparaît tout seul après un `8 words`"
  - le bug est plus haut niveau :
    - pendant une longue fenêtre, le guest **n'appelle même plus** le helper PsyQ qui programme DMA3
    - puis il revient tardivement avec seulement la lecture du header

### Nouveau résultat important (2026-03-30, vérité disque sur `lba=117..129`)
- Lecture directe du BIN `hello_strplay.bin` sur les headers STR :
  - `lba=117..120` = `frame 3`, secteurs `7..10 / 11`
  - `lba=121..123` = `frame 4`, secteurs `0..2 / 10`
  - `lba=124` = secteur XA audio realtime
  - `lba=125..129` = `frame 4`, secteurs `3..7 / 10`
- Conséquence importante :
  - le trou de programmation DMA3 ne couvre pas seulement la fin de `frame 3`
  - il couvre aussi le début de `frame 4`
  - et le retour tardif à `lba=128` arrive déjà sur `frame 4 sec=6`

### Nouveau résultat important (2026-03-30, codes d'état `StCdInterrupt`)
- Hook temporaire sur `DAT_8002d748` :
  - artifact : `logs/str_stcd_state_trace.txt`
- Mapping obtenu en recroisant avec Ghidra (`StCdInterrupt`) :
  - `state=10` = chemin normal avant lancement du payload DMA3 (`0x1f8 words`)
  - `state=4` = early return sur `if (*DAT_80033dc8 != 0)` : slot courant du ring déjà occupé
  - `state=6` = mismatch de continuité / frame (`sec/frame` inattendus après lecture du header)
- Trace observée :
  - `lba=109..115` : `state=10`
  - `lba=117..127` : **`state=4` répété**
  - `lba=128` : **`state=6`**
  - `lba=129..130` : retour à `state=4`
- Interprétation forte :
  - entre `lba=117` et `lba=127`, `StCdInterrupt()` ne lance plus aucun DMA3 car il considère que le slot d'écriture courant est encore occupé
  - à `lba=128`, il relit enfin un header (`8 words`), mais ce header arrive trop tard dans la continuité du flux (`frame 4 sec=6`) et déclenche le reset de continuité (`state=6`) avant le payload
- Conclusion :
  - le bug restant n'est plus centré sur le lecteur CD lui-même
  - le bug restant est maintenant localisé sur la **gestion de l'occupation/libération des slots du ring PsyQ**
  - piste suivante la plus probable :
    - pourquoi le slot pointé par `DAT_80033e08` reste non nul trop longtemps
    - donc côté `StFreeRing()` / transition des statuts de slot, pas côté lecture disque brute

### Nouveau résultat important (2026-03-30, contrat `StGetNext` / `StFreeRing`)
- Ghidra :
  - `StGetNext()` :
    - si le slot courant vaut `2`, il le passe immédiatement à **`4`**
    - puis retourne :
      - `header = slot`
      - `addr = base payload` (`ring_base + ring_size*0x20 + slot*0x7e0`)
  - `StFreeRing(base)` :
    - recalcule l'index de slot à partir de `base`
    - **ne libère que si le slot de base vaut `4`**
    - puis remet à `0` tous les slots du span (`psVar5[3]`)
- Conséquence importante :
  - le chemin normal PsyQ est bien :
    - `2 -> 4` dans `StGetNext`
    - puis `4 -> 0` dans `StFreeRing`
- Donc le problème restant est encore plus précis :
  - soit `StFreeRing()` n'est pas appelé au bon moment
  - soit il reçoit un `base` qui ne retombe pas sur le bon slot
  - soit le slot bloquant vu par `StCdInterrupt(state=4)` n'est pas celui que le player vient de libérer

### Nouveau résultat important (2026-03-29 nuit)
- **Adresse correcte** de `data_ready_callback` : `0x80019218`
  - Pas `0x80019290`
  - Preuve : `hello_strplay.map` + Ghidra
- **`data_ready_callback` n'est PAS un callback "fin DMA3 de frame" uniquement**
  - `CdRead2()` l'installe via `CdDataCallback(data_ready_callback)` (`CDREAD2_OBJ_4C`)
  - `C_011_OBJ_8B0` peut aussi l'appeler quand `DAT_80033e38 != 0`
- **Run BIOS instrumenté** (`logs/str_data_ready_bios_240.stderr.txt`) :
  - le callback est bien appelé
  - et il est appelé **plusieurs fois pour un même `frameCount`**
  - ex : `frame_hdr=1` sur `#2..#12`, `frame_hdr=2` sur `#13..#21`
- Conclusion :
  - l'hypothèse "DMA3 completion IRQ manquante" est **invalidée**
  - le vrai problème est plus haut niveau : **quand** le ring buffer est marqué prêt vs **quand** `StGetNext`/`DecDCTvlc` consomment la frame

### Nouveau résultat important (2026-03-29 fin de nuit)
- **Le mélange apparent des blocs dans les dumps PPM était un faux positif**
  - cause : `dump_frame_ppm()` reconstruisait l'image en ordre raster-major
  - or `hello_strplay` utilise `DecDCTout()` par slices verticales de 16 pixels, donc le flux MDEC est **slice-major**
- Correctif appliqué dans `src/mdec/mdec.cpp`
  - reconstruction PPM en ordre `slice-major`
  - index macrobloc : `(x / 16) * mb_rows + (y / 16)`
- Preuve runtime (`--mdec-dump=3`) :
  - `logs/mdec_frame_0001.ppm` montre désormais une image cohérente, visuellement conforme aux premières frames après le fade
  - `logs/mdec_frame_0000.ppm` = noir (normal avant affichage utile)
  - `logs/mdec_frame_0002.ppm` reste tronquée car frame 3 n'a que **188 MBs**
- Conclusion :
  - **le MDEC ne semble pas sortir les macroblocs dans le désordre**
  - le problème restant est bien la **frame incomplète / rendue trop tôt**, pas un mauvais ordre spatial global

### Nouveau résultat important (2026-03-30 tôt)
- **`StGetNext` voit une frame "prête" alors que tous ses slots ne sont pas encore prêts**
- Hook temporaire sur `StGetNext` (`0x80015F88`) :
  - log artifact : `logs/str_stgetnext_span_180.stderr.txt`
- Résultats clés :
  - Frame 1 :
    - `idx=0 status=2 sec=0/11 frame=1`
  - Frame 2 :
    - `idx=11 status=2 sec=0/10 frame=2`
    - span observé au moment du `status==2` :
      - `11:2/0,12:2/1,13:2/2,14:0/0,...`
  - Frame 3 :
    - `idx=21 status=1 frame=3 -> next_idx=0 next_status=2`
    - donc `StGetNext` passe bien par le **wrap sentinel** puis considère `slot 0` prêt pour frame 3
    - span observé au même moment :
      - `0:2/0,1:3/1,2:3/2,3:2/3,4:2/4,5:2/5,...`
- Interprétation :
  - `status==2` devient visible **avant** que tout le span des `nSectors` soit stabilisé en état final
  - on observe des slots encore en `3` (et même `0` sur frame 2) dans le span au moment où `StGetNext` expose déjà la frame
  - cela colle exactement avec le symptôme :
    - `DecDCTvlc` démarre sur une frame encore incomplète
    - `frame 3` finit à `188/200 MBs`
- Conclusion de debug :
  - le problème est maintenant localisé sur la **transition des slots vers l'état visible** (`2`) / le moment où le ring est exposé à `StGetNext`
  - ce n'est **pas** un problème de géométrie MDEC ni simplement un problème de wrap non géré par `StGetNext`

### Nouveau résultat important (2026-03-30, callback DMA)
- Hook temporaire sur `data_ready_callback` (`0x80019218`)
  - log artifacts :
    - `logs/str_dataready_180.stderr.txt`
    - `logs/str_dataready_df8_180.stderr.txt`
- Faits observés :
  - la callback tourne bien **par secteur / slot**, pas seulement en fin de frame
  - exemples :
    - frame 1 : `idx=0..10`
    - frame 2 : `idx=11..20`
    - frame 3 : wrap sur `idx=0`, puis `idx=3..6` visibles dans le log
  - au moment où elle tourne, le slot courant est typiquement déjà en `pre_status=3`, puis la callback le rend visible (`2`)
  - lecture directe des globals au moment du hook :
    - `DAT_80033df8 = 0`
    - `DAT_80033e38 = 0` la plupart du temps, `1` seulement sur le dernier secteur observé d'une frame
  - donc la callback observée ne peut pas être expliquée seulement par le chemin `if (DAT_80033df8 && DAT_80033e38)` de `C_011_OBJ_8B0`
  - le `ra=0x8001107C` pointe vers le dispatcher DMA (`INTR_DMA_OBJ_4C`) : on passe donc bien par le **chemin IRQ DMA**
- Interprétation :
  - la visibilité `status=2` est bien alimentée par le **dispatcher DMA par secteur**
  - le problème restant n'est pas un scramble MDEC, mais l'exposition trop tôt d'une frame alors que son span n'est pas encore entièrement remplacé/stabilisé dans le ring
- Tentative invalidée :
  - ajout de timing artificiel sur `DMA2` GPU block-mode (`LoadImage`/uploads VRAM)
  - **aucun effet** : frame 3 reste `188/200 MBs`
  - patch retiré

### Nouveau résultat important (2026-03-30, trace directe des slots du ring)
- Hooks temporaires sur :
  - écritures des statuts/globals du ring dans `bus.cpp`
  - entrée de `StFreeRing()` dans `cpu.cpp`
- Artifacts :
  - `logs/str_ring_state_trace.txt`
  - `logs/str_stfreering_trace.txt`
- Résultats clés :
- `StFreeRing()` est bien appelé :
  - slot `0` à `read_lba=92`
  - slot `11` à `read_lba=117`

### Nouveau résultat important (2026-03-30, coalescence prouvée des IRQ DMA3 utiles)
- Instrumentation ciblée dans `src/r3000/bus.cpp` autour de :
  - `dma_finish(3)`
  - write `DICR`
  - write `I_STAT`
- Artifact principal :
  - `logs/str_dma3irq_window.stderr.txt`
- Preuve directe observée sur `lba=109..112` :
  - `lba=109`
    - premier DMA3 : `old_mf=0 -> new_mf=1`, `irq_fired=1`
    - second DMA3 : `old_mf=1 -> new_mf=1`, `irq_fired=0`
  - `lba=110`
    - les deux DMA3 finissent avec `old_mf=1 -> new_mf=1`, `irq_fired=0`
  - `lba=111`
    - même pattern : aucun nouvel edge IRQ DMA utile
  - `lba=112`
    - le guest finit enfin par ack le DMA via :
      - `DICR write pc=0x80011060 v=0x088A0000 ack=0x08000000`
      - `old=0x888A0000 -> new=0x008A0000`
      - `old_mf=1 -> new_mf=0`
    - le DMA3 suivant peut alors refaire :
      - `old_mf=0 -> new_mf=1`, `irq_fired=1`
- Conclusion forte :
  - la formule DICR elle-même n'a rien d'anormal (DuckStation ne lève aussi l'IRQ DMA que sur la transition `master_flag 0 -> 1`)
  - **mais dans notre run, plusieurs complétions DMA3 utiles tombent pendant que `master_flag` est déjà à 1**
  - donc côté guest/PsyQ, on ne voit effectivement qu'une IRQ DMA utile environ tous les 3 secteurs dans cette fenêtre
  - cela colle avec les slots qui passent `3 -> 2` trop peu souvent et avec `frame 3 = 188/200 MBs`

### Tentative invalidée (2026-03-30, complétion DMA3 différée)
- Idée testée :
  - retirer le `cdrom_->tick(112000)` direct dans `exec_dma3_transfer()`
  - et transformer le payload DMA3 streamé en complétion différée via `Bus::tick()`
- Résultat :
  - le pipeline devient trop lent
  - on n'obtient plus qu'une seule `Frame decode END: 200 MBs`
  - puis la queue CD part en overwrite
- Décision :
  - patch retiré
  - **ne pas repartir sur un "DMA3 delayed finish" global en l'état**

### Tentative invalidée (2026-03-30, replay artificiel des payload IRQ DMA3)
- Idée testée :
  - rejouer une IRQ DMA3 "payload" après ACK quand une complétion était tombée alors que `master_flag` était déjà haut
- Résultat :
  - trop invasif / dévie trop tôt
  - le run court ne retombe même plus proprement sur la phase STR exploitable
- Décision :
  - patch retiré
  - **ne pas empiler d'IRQ DMA3 synthétiques sans modèle temporel plus solide**

### Nouveau résultat important (2026-03-30, la redelivery `0xFE` n'est pas active dans la fenêtre critique)
- Vérification croisée entre :
  - `logs/str_cd_ring_fix1_120.stderr.txt`
  - `logs/str_dma3irq_window.stderr.txt`
- Dans la fenêtre `lba=109..112`, on observe :
  - `push/consume/advance` normaux
  - `pending_reason=0xFF`
  - **aucune** trace de redelivery `0xFE`
  - et `qdepth` reste à `1`
- Conséquence importante :
  - la coalescence IRQ DMA3 observée sur `109..112` ne vient **pas** d'une "buffered redelivery" trop agressive
  - elle arrive même dans un chemin de cadence secteur **normal** (`0xFF`, un seul secteur visible à la fois)
- Conclusion :
  - la piste "missed INT1 redelivery à 5000 cycles" est **invalidée** pour le bug `188/200`
  - le noeud restant se resserre encore sur la **cadence DMA3 utile / ACK guest**, pas sur la logique de redelivery CD
    - slot `0` à `read_lba=127`
  - pendant la frame problématique, on voit :
    - `slot=0 old=1 new=3 read_lba=109`
    - `slot=1 old=352 new=3 read_lba=110`
    - `slot=2 old=352 new=3 read_lba=111`
    - puis seulement :
      - `slot=0 old=3 new=2 read_lba=112`
    - **sans transition `3 -> 2` pour `slot 1` et `slot 2`**
  - plus tard, `StFreeRing()` libère pourtant toute la frame :
    - `slot=1 old=3 new=0 read_lba=127`
    - `slot=2 old=3 new=0 read_lba=127`
- Conclusion :
  - la cause immédiate du `188/200 MBs` est maintenant prouvée :
    - certains slots intermédiaires du span de frame restent bloqués en `3`
    - puis sont libérés directement en `0` sans jamais devenir visibles (`2`)
  - le problème n'est donc pas l'absence de `StFreeRing()`
  - le problème est bien la **cadence / coalescence** de la promotion `3 -> 2`

### Nouveau résultat important (2026-03-30, calibration du bonus DMA3 streaming)
- Test ciblé sur le bonus artificiel appliqué après chaque DMA3 streaming dans `Bus::exec_dma3_transfer()` :
  - valeur stable historique : `112000` cycles
  - test `0`
  - test `56000`
- Effet observé :
  - avec `112000` :
    - état connu : frame 1 et 2 correctes, frame 3 à `188/200`
    - wrap problématique avec `slot 1`/`slot 2` laissés en `3`
  - avec `0` ou `56000` :
    - **le symptôme change fortement**
    - `mdec_frame_0002.ppm` tombe à `59919` bytes (sous-alimentation beaucoup plus tôt)
    - le pattern des slots change aussi :
      - `slot 0` passe tout de suite `3 -> 2` après le wrap
      - mais le flux global devient trop court et casse avant
- Tentative invalidée :
  - réduire brutalement ce bonus n'est pas le correctif
  - mais ce test prouve que ce bonus contrôle bien un axe réel du bug :
    - trop élevé => le CD prend de l'avance sur la promotion des slots
    - trop faible => le flux vidéo est sous-alimenté trop tôt
- Décision :
  - revenir à `112000` comme baseline stable de debug
  - ne plus traiter ce bonus comme une constante arbitraire "bonne par défaut"
- la correction finale devra probablement passer par un **timing plus fidèle de la complétion DMA3 / du scheduling**, pas par un simple `0` ou `/2`

### Nouveau résultat important (2026-03-30, `sb_r_` déplacé à la livraison INT1)
- Changement testé :
  - ne plus avancer `sb_r_` au moment de `check_sector_read_complete()`
  - avancer `sb_r_` seulement lors de la livraison de l'INT1 `0xFE` (buffered redelivery), plus proche du modèle DuckStation
- Résultat :
  - **aucun changement observable** sur le symptôme
  - on reste à :
    - frame 1 = `200/200`
    - frame 2 = `200/200`
    - frame 3 = `188/200`
  - `logs/str_ring_state_trace.txt` reste identique sur le point clé :
    - `slot 0` passe `3 -> 2`
    - `slot 1` et `slot 2` passent toujours `3 -> 0` sans devenir visibles
- Conclusion :
  - le moment exact où `sb_r_` bascule n'est pas la cause racine du trou `188/200`

### Nouveau résultat important (2026-03-30, `qdepth` reste à 1 jusqu'à `lba=115`)
- Les traces `CD_RING consume/push` montrent :
  - de `lba=85` jusqu'à `lba=115`, la queue reste à **`qdepth=1`**
  - donc, avant le décrochage, il n'y a **pas** de backlog caché de plusieurs secteurs en attente
- Conséquence importante :
  - le chemin "missed INT1 / buffered redelivery" n'est **pas** la cause principale du trou sur la frame 3
  - le problème apparaît avant tout comme un **manque de cadence utile** au moment où la frame 3 devient visible/consommée

### Tentatives invalidées (2026-03-30, nouvelles)
- **Retarder la complétion DMA3** (proportionnel à `8/504 words`) :
  - trop destructif
  - le player ne dépasse même plus la première vraie frame utile
- **Retarder seulement la promotion du secteur suivant** après DMA3 :
  - même effet destructif
  - la cadence du player casse bien avant la frame 3
- **Forcer `DRQSTS` à dépendre de `BFRD` + clear auto de `BFRD` à fin de lecture** :
  - trop destructif dans notre modèle actuel
  - retour à une seule frame utile
- **Augmenter le budget `DMA1 MDEC_OUT` de `20000` à `22000 cycles/MB`** :
  - **aucun effet observable**
  - frame 3 reste à `188/200`

### Nouvelle piste la plus probable (2026-03-30 fin)
- Les preuves convergent vers une **coalescence des callbacks DMA3**, pas vers un secteur manquant en entrée :
  - autour de `lba=109..112`, les slots écrits passent :
    - `slot0 -> 3`
    - `slot1 -> 3`
    - `slot2 -> 3`
    - puis seul `slot0` devient `2`
  - `slot1` et `slot2` sont ensuite libérés directement en `0`
- Les logs montrent aussi que l'IRQ DMA (`I_STAT bit3`) reste déjà haute sur plusieurs secteurs (`i_stat=0x000C`), ce qui peut laisser plusieurs complétions DMA3 se **coalescer** avant que PsyQ ne voie un nouveau callback utile
- Prochaine étape recommandée :
  - instrumenter **spécifiquement** `dma_finish(3)` / `DICR` / `I_STAT bit3` autour de `lba=109..112`
  - vérifier si plusieurs `DMA3 done` surviennent alors que le flag DMA est déjà haut
  - si oui : corriger la cadence de **signalisation DMA3** plutôt que le CD brut, le XA, ou le MDEC

---

## Cause racine identifiée

**DecDCTvlc est appelé AVANT que tous les secteurs d'une frame arrivent dans le ring buffer.**

### Preuve
```
DECDCTVLC #2 CALL: a0(bs)=0x801F5A00    ← lit frame 2 du ring buffer
DMA3_RING vblank=490: wrote LBA 98 data  ← secteur 2 arrive APRÈS !
```

### Pourquoi
Notre CPU est trop rapide par rapport au CDROM. Sur PS1 réel :
- MDEC decode prend des vrais cycles → le CD avance pendant ce temps
- DuckStation a un event scheduler global qui fire les secteurs même pendant les DMA

Chez nous : DMA0/DMA1/DMA3 sont synchrones (0 cycles). Le CDROM n'avance que via `tick()` explicites.

### Workaround actuel
- DMA3 streaming tick : 112000 cycles (~50% sector period) après chaque DMA3 en ReadS
- DMA1 MDEC tick : 20000 cycles/MB pour simuler le temps MDEC réel
- DMA0 MDEC tick : avance CDROM pendant le transfert DMA0
- Résultat : frame 2 OK, frame 3 à 98%

---

## Ce qui a été vérifié (NE PAS RE-VÉRIFIER)

| Élément | Statut | Preuve |
|---------|--------|--------|
| CPU shifts (SLL/SRL/SLLV/SRLV) | ✅ Identique DuckStation | Comparaison code source |
| Huffman tables | ✅ Correctement chargées | 13676/16384 nonzero, EXE 30/30 pages |
| BS data sur disque | ✅ Correct | Standalone Python decode OK (200 MBs) |
| MDEC RLE decode | ✅ Identique DuckStation | Comparaison code source |
| MDEC IDCT | ✅ Identique DuckStation | Comparaison code source |
| MDEC YUV→RGB | ✅ Identique DuckStation | Comparaison code source |
| Scale table transpose | ✅ Identique DuckStation | `scale[y*8+x] = raw[x*8+y]` |
| Zigzag table | ✅ Identique DuckStation | Même zagzig[64] |
| sign_extend\<9\> / sign_extend\<10\> | ✅ Fixé | Masque N bits avant extension |
| Sector reading (Mode 2 offset) | ✅ Correct | Offset 24 pour user data |
| EXE loading | ✅ Complet | 30/30 pages, 122880 bytes |
| DMA6 OTC overlap | ✅ Pas de overlap | Range [0x120A08-0x122E24] vs ring 0x1F61E0 |
| Memory corruption | ✅ Aucune | Watchpoint: seul BIOS memclear écrit 0 (pendant boot) |
| `data_ready_callback` address | ✅ Corrigé | `0x80019218` via `hello_strplay.map` + Ghidra |
| `data_ready_callback` source | ✅ Compris | Installé par `CdRead2` via `CdDataCallback`, pas seulement par IRQ DMA3 |

---

## Mécanisme PsyQ STR (décompilé via Ghidra)

### Game main loop (0x800101C0)
```c
do {
    result = StGetNext(&addr, &header);   // Poll for complete frame
    if (result == 0) {                     // 0 = frame available
        DecDCTvlc(addr, buf);              // Convert BS → MDEC format
        StFreeRing(addr);                  // Free ring buffer slot
        return;
    }
} while (retries-- > 0);  // 8 million retries
```

### StGetNext (0x80015F88)
Retourne 0 quand le slot ring buffer a **status == 2** (frame complete).

### CdRead2 (0x80016B58)
- `CdRead2()` installe `data_ready_callback` via `CdDataCallback(data_ready_callback)`
- Donc `data_ready_callback` est un callback **data-ready CD**, pas uniquement un callback "fin DMA3 du dernier secteur"

### StCdInterrupt (0x80016068)
- Vérifie `DAT_80033e38` (armed flag). Si == 1 → **return immédiat** (skip sector)
- Si != 1 : lit le secteur via DMA3, copie dans ring buffer
- **Dernier secteur** (`sector_num == sector_count - 1`) :
  - Met `DAT_80033e38 = 1` (arme le flag)
  - DMA3 avec **IRQ enabled** (via C_011_OBJ_A20)
  - `C_011_OBJ_8B0()` peut ensuite appeler `data_ready_callback()` si `DAT_80033e38 != 0`
- **data_ready_callback** (0x80019218) :
  - Met `status = 2`
  - copie `frameCount`/header vers `DAT_800362A0/800362A4`
  - met `DAT_80033e10 = DAT_80033e08`
  - remet `DAT_80033e38 = 0`

### Ce qu'on a observé en vrai
```
data_ready_callback #2..#12  -> frame_hdr=1
data_ready_callback #13..#21 -> frame_hdr=2
data_ready_callback #23..    -> frame_hdr=3
```

Donc le callback tourne **plusieurs fois par frameCount** dans notre run BIOS.

### Nouveau résultat important (2026-03-30 fin de journée, `8 words` vs `504 words`)
- Run propre avec le binaire CLI rebuildé :
  - artifact : `logs/str_dma3irq_words_real_full.txt`
- Fenêtre `lba=109..112` observée :
  - `lba=109` :
    - `DMA3IRQ finish ... words=8 ... irq_fired=1`
    - `DMA3IRQ finish ... words=504 ... irq_fired=0`
  - `lba=110` :
    - `words=8 -> irq_fired=0`
    - `words=504 -> irq_fired=0`
  - `lba=111` :
    - `words=8 -> irq_fired=0`
    - `words=504 -> irq_fired=0`
  - `lba=112` :
    - `DICR write pc=0x80011060 ... old_mf=1 new_mf=0`
    - puis `words=8 -> irq_fired=1`
    - puis `words=504 -> irq_fired=0`
- Conclusion dure :
  - dans cette fenêtre, **le seul edge IRQ DMA utile tombe sur le transfert header `8 words`**
  - le payload `504 words` **ne génère jamais** un nouvel edge IRQ visible
  - tant que `master_flag` reste à `1`, les complétions suivantes (`8` comme `504`) sont coalescées

### Nouveau résultat important (2026-03-30 fin de journée, cadence `strCallback`)
- Même artifact : `logs/str_dma3irq_words_real_full.txt`
- Hook `STR_CB` sur `pc=0x800100C4` (`strCallback`)
- Observé :
  - `#1` à `read_lba=118` avec `irq_serial=199`
  - `#3` à `read_lba=119` avec `delta_since_prev=1`
  - puis `#4..#20` restent sur `read_lba=119` avec `delta_since_prev=0`
  - ensuite `#21` saute directement à `read_lba=128` avec `delta_since_prev=9`
- Conclusion :
  - côté callback slice, il n'y a **presque plus de nouvelles IRQ CD utiles consommées** pendant toute la séquence bloquée
  - le problème n'est donc pas juste "un payload DMA sans IRQ"
  - il y a aussi une **grande famine de callbacks utiles** entre `lba=119` et `lba=128`

### Nouveau résultat important (2026-03-30 nuit, `data_ready_callback` arrive trop tard)
- Nouveau run propre avec hook unique sur `data_ready_callback`
  - artifact : `logs/str_ready_focus_120.txt`
- Tous les passages observés dans `data_ready_callback` ont :
  - `pc=0x80019218`
  - **`ra=0x8001107C`**
  - donc ils arrivent bien via le **dispatcher DMA/IRQ**, pas via un appel direct exotique du player
- Dans ce run :
  - `DAT_80033df8` reste **toujours à `0`**
  - `DAT_80033e38` vaut généralement `0`
  - sauf sur le dernier slot stable de frame 2 :
    - `read_lba=108`
    - `slot=20`
    - `e08=21`
    - `e10=20`
    - `e38=1`
- Fenêtre critique ensuite :
  - **aucun `STR_READY` à `lba=109`, `110`, `111`**
  - puis seulement :
    - `read_lba=112`
    - `slot=0`
    - `slot_status=3`
    - `e08=3`
    - `e10=0`
    - `e38=0`
  - puis :
    - `lba=113` -> `slot=3`
    - `lba=114` -> `slot=4`
    - `lba=115` -> `slot=5`
    - `lba=116` -> `slot=6`
- Corrélation directe avec `logs/str_ring_state_trace.txt` :
  - `slot0` est promu `3 -> 2`
  - `slot1` et `slot2` ont bien été écrits en `3`
  - mais **ne reçoivent jamais de `data_ready_callback` dédiée**
  - ils finissent plus tard en `3 -> 0`
- Conclusion dure :
  - le trou `188/200` n'est pas "des données jamais lues"
  - c'est une **promotion ring coalescée/tardive**
  - `data_ready_callback` arrive trop tard, avec `e08` déjà avancé à `3`, puis fait `DAT_80033e10 = DAT_80033e08`
  - ce snap **saute mécaniquement `slot1/slot2`**

### Tentatives invalidées (2026-03-30 nuit, budget synthétique DMA3 header/payload)
- En comparant le modèle avec DuckStation :
  - DuckStation facture les DMA CD->RAM avec un coût **proportionnel aux mots transférés** (`GetDMARAMTickCount`)
  - notre modèle STR ajoute au contraire un **gros budget synthétique fixe** (`112000`) après chaque DMA3 streaming
- Deux tests ont été faits sur `hello_strplay` :
  1. **Budget seulement sur le payload `504 words`**
     - artifact : `logs/str_dma3_payload_only_120.txt`
     - résultat :
       - frame 1 = `200/200`
       - frame 3 tombe à **`110/200 MBs`**
       - puis frame suivante à **`78/200 MBs`**
     - `logs/str_ring_state_trace.txt` montre alors que le wrap ne progresse presque plus :
       - `slot0` devient visible
       - `e08/e10` restent bloqués à `1`
       - `slot1` n'est revisité qu'à `lba=130`
  2. **Budget proportionnel aux mots (`8` vs `504`)**
     - artifact : `logs/str_dma3_proportional_120.txt`
     - résultat : **même dégradation**
       - frame 3 = `110/200`
       - puis `78/200`
- Conclusion :
  - le budget synthétique actuel sur le header est **anormalement gros**
  - mais il est aussi **structurellement nécessaire** dans notre modèle actuel pour ne pas sous-alimenter immédiatement le flux
  - le problème n'est donc pas "supprimer le budget header"
  - il faut corriger le **modèle temporel global** qui rend ce budget header à la fois nécessaire et trop agressif
- Décision :
  - retour à la baseline stable `112000` par DMA3 streaming
  - ne pas conserver ces variantes dans le core

---

## Prochaine étape à investiguer

### Nouvelle hypothèse de travail

Le problème restant n'est plus :
- "IRQ CD absente quand `BFRD=0`" (corrigé)
- "secteurs vidéo sautés par erreur" (invalidé)
- "MDEC / ordre des macroblocs" (invalidé)
- "formule DICR fausse" (DuckStation fait aussi l'IRQ DMA sur `master_flag 0 -> 1`)

L'hypothèse la plus probable devient :
- notre modèle fait arriver / finir plusieurs DMA3 utiles pendant que le guest n'a pas encore eu le temps d'ACK la précédente
- donc `master_flag` reste haut trop longtemps
- et les **edges IRQ DMA utiles se coalescent**
- en particulier, l'edge visible semble être consommé par le **header DMA3 `8 words`**, pas par le payload `504`
- et côté ring cela se voit maintenant explicitement :
  - `data_ready_callback` ne passe plus sur `slot1/slot2`
  - puis `e10 <- e08` saute directement au slot suivant visible
- ce qui se répercute ensuite sur :
  - `StCdIntrFlag`
  - `strCallback`
  - `StCdInterrupt`
  - et finalement la promotion trop rare des slots vers `status=2`

Autrement dit :
- le vrai noeud restant est maintenant la **cadence temporelle DMA3 utile vs ACK guest**
- pas le contenu disque, pas le XA, pas le MDEC

### Nouvelles preuves (2026-04-01, session Codex)

- La timeline unifiée confirme maintenant une séquence précise après le wrap :
  - `lba=109` : `slot 21 -> 1`, puis `slot 0 : 3 -> 2`
  - `lba=110..125` : `data_lba` reste bloqué à `110` pendant que `read_lba` avance
  - `lba=126` : le player relance bien `DMA3 8 words` **puis** `DMA3 504 words`, et `slot 1` devient `2`
  - `lba=127` : le player ne fait plus que `DMA3 8 words`, puis le path `pc=0x800164A8/0x800164AC` force `slot 2 : 352 -> 0`, avant que `data_ready_callback` le repasse artificiellement à `2`
  - ensuite `StGetNext()` boucle indéfiniment sur `idx=2 status=2`
- Donc le nouveau verrou précis est :
  - `slot 2` devient "visible" après le **header seul** du secteur `127`
  - le payload `504 words` n'est jamais programmé pour ce slot
  - ce n'est donc plus juste "frame 3 incomplète", c'est un **état ring faux créé après un header sans payload**

### Tentatives invalidées (2026-04-01)

- Retirer le reset `sb_[sb_r_].pos = 0` quand `BFRD` retombe à `0`
  - aucun effet sur la fenêtre `110 -> 127`
  - `slot 2` reste promu après le header `8 words` seul
- Différer la livraison `INT1` quand la queue 8 secteurs est pleine
  - aucun effet visible sur la même fenêtre
  - pas de changement sur la promotion fautive de `slot 2`
- Rendre le FIFO de sortie MDEC "DuckStation-like"
  - changements testés :
    - ne garder qu'un chunk visible à la fois
    - relancer `execute()` seulement quand la sortie se vide
  - résultat :
    - régression nette
    - le drainage utile s'arrête encore plus tôt
    - `mdec_frame_0002.ppm` tombe à `144399` bytes
    - à partir de `lba=116`, on retombe dans une longue suite de `CDIRQ ... want=0` sans reprise exploitable
  - conclusion :
    - le problème restant n'est pas "le MDEC prédecode trop"
    - patch rejeté, retour à la baseline `188/200`

Conclusion à garder :
- les trois derniers essais côté `BFRD` / "queue pleine" / FIFO MDEC sont **invalidés**
- la prochaine cible n'est plus "comment garder plus de secteurs", mais **pourquoi le path header-only à `0x800164A8/0x800164AC` fait encore naître un slot `status=2` sans payload**

### Nouveau résultat important (2026-04-01, `sb_r_` suit bien `INT1`, mais le bug reste)
- Correctif testé :
  - quand un `INT1` livre un nouveau secteur bufferisé, `sb_r_` avance maintenant même avec `want_data_=0`
  - ça aligne enfin l'état interne sur le modèle annoncé par le code : `read_idx` suit la livraison `INT1`, pas seulement les DMA
- Résultat runtime :
  - **aucune amélioration finale** : `frame 1 = 200/200`, `frame 2 = 200/200`, `frame 3 = 188/200`
  - mais la continuité observée change nettement au point de casse :
    - avant : `STCDHDR ... sec=4 exp_sec=7` autour de `lba=130`
    - après : `STCDHDR ... sec=8 exp_sec=7`
- Lecture à garder :
  - ce patch enlève une incohérence réelle du ring CD interne
  - mais le verrou final n'est toujours pas le buffer CD "brut"
  - il reste plus probablement sur la jonction :
    - `StCdInterrupt(state=4)`
    - `StGetNext()/StFreeRing()`
    - et la décision de lancer ou non le payload `DMA3 504 words`

### Debug à faire
1. **Cibler le trou entre `DMA3 8 words` et `DMA3 504 words`**
   - on a maintenant une preuve que la régression restante n'est plus juste le wrap `109..112`
   - avec la variante `sb_r_`, les deux premières frames passent mais la suivante rate déjà son payload
   - il faut donc expliquer **pourquoi le `504 words` n'est plus programmé après le header**
2. **Mesurer la cadence réelle de retour côté player**

### Nouveau résultat important (2026-04-02, le `504 words` n'est plus programmé par le guest)
- Nouvelle instrumentation dans [`src/r3000/bus.cpp`](E:/Projects/github/Live/R3000-Emu/src/r3000/bus.cpp) :
  - trace `DMA3REG` sur les écritures guest de `MADR/BCR/CHCR`
  - artifact : `logs/str_timeline.txt`
- Fenêtre critique observée autour de `read_lba=129` :
  - `CDREQ write=0x80`
  - `DMA3REG pc=0x80016AE0 reg=MADR val=0x801F0040`
  - `DMA3REG pc=0x80016AE8 reg=BCR val=0x00000008`
  - `DMA3 start ... words=8`
  - `DMA3REG pc=0x80016B28 reg=CHCR val=0x11000000`
  - puis **aucune** écriture guest ultérieure `BCR=0x000001F8` / **aucun** second `CHCR` pour lancer le payload
- État guest capturé au même moment :
  - `slot_ptr=0x801F0040`
  - `slot_status` vaut `0` avant le header, puis `352` juste après
  - le header lu est `frame=4 sec=0 nsec=10`
  - `exp_sec` vaut encore `7` avant le reset de continuité
- Conséquence forte :
  - le payload `504 words` n'est **pas perdu par l'émulateur**
  - il n'est simplement **plus programmé côté guest** dans cette branche
  - le verrou courant se resserre donc encore :
    - soit `StCdInterrupt()` décide légitimement d'abandonner après le header à cause d'un état de continuité/ring déjà faux
    - soit `data_ready_callback()` / la promotion `slot -> status 2` rend visible un slot header-only alors que le payload n'a jamais été demandé
- Conclusion de debug :
  - la prochaine cible n'est plus `exec_dma3_transfer()` ni le bus DMA3 brut
  - la prochaine cible est le contrat guest autour de :
    - `StFreeRing()` / gros reset à `0x80015F68`
    - `StCdInterrupt()` quand `exp_sec=7` puis arrive `frame4 sec0`
    - et surtout **pourquoi `data_ready_callback()` promeut encore le slot après un header-only**
   - corréler :
     - `DMA3 8 words`
     - `STR_CB entry`
     - `Request=0x80`
     - puis éventuel `DMA3 504 words`
   - objectif :
     - savoir si le player perd le payload parce qu'il revient trop tard
     - ou parce qu'un état interne de ring/continuité le bloque déjà avant
3. **Ne plus retoucher `sb_r_` seul**
   - la tentative "buffer visible = dernier secteur livré" est maintenant invalidée
   - elle améliore partiellement le début du flux, mais dégrade ensuite la programmation du payload
4. **Éviter les faux fixes**
   - ne pas synthétiser d'IRQ DMA3
   - ne pas retoucher globalement `112000` / `20000` sans nouvelle preuve
   - ne pas conserver les variantes `DRQSTS/BFRD` ou `sb_r_` qui cassent `frame 3`

### Nouveau résultat important (2026-04-02, après `188/200`, la rafale visible vient de `DMA1`)
- Run ciblé avec neutralisation temporaire du flag `DMA3` sur le reset de continuité `0x800164AC`
  - artifact : `logs/str_state6_squelch_test_60s.stderr.txt`
- Ce que ça prouve :
  - on peut bien supprimer le flag `DMA3` header-only au moment du reset de continuité
  - **ça ne stoppe pas** la rafale de callbacks `strCallback`
- La preuve utile est dans les écritures `DICR` répétées juste après `Frame decode END: 188 MBs` :
  - `old=0x828A0000`
  - ACK guest : `v=0x028A0000`
  - le flag encore servi est donc **bit25 = DMA1**, pas **bit27 = DMA3**
- Corrélation avec la table `intr_dma` :
  - slot `1` = `0x800100B8` = `strCallback`
  - slot `3` = `0x80019218` = `data_ready_callback`
- Conclusion :
  - le faux réveil tardif `DMA3/data_ready_callback` existe bien dans le path header-only
  - mais **la boucle visible après le `188/200`** est portée par la chaîne **`DMA1 -> slot1 -> strCallback`**
- la prochaine cible la plus rentable n'est donc plus `dma_finish(3)`, mais la logique **DMA1/MDEC out** :
  - reprogrammation des slices via `DecDCTout`
  - statut MDEC (`out_req` / `busy`)
  - et `dma1_finish_pending_` / cadence de complétion DMA1

### Tentative invalidée (2026-04-02, "temps DMA global" à la DuckStation)
- Deux essais structurels ont été faits dans `src/r3000/bus.cpp` :
  - faire consommer les ticks différés de `DMA1` et `DMA3` par **tout** `Bus::tick()`
  - puis variante plus étroite : faire consommer au **temps global** seulement les ticks `DMA1/MDEC`, tout en gardant le `+112000` streaming dans le couloir CDROM
- Résultat des deux variantes :
  - régression plus tôt que la baseline `188/200`
  - plus de progression utile jusqu'au `Frame decode END` attendu dans la fenêtre
  - le run finit bloqué autour de `read_lba=128` avec boucle `StGetNext idx=7 slot_status=2`
  - et un pattern `DMA3 8 words` header-only suivi d'une rafale `DMA1` sur données `0x80808080`
- Conclusion utile :
  - la différence avec DuckStation **n'est pas** réductible à "il manque juste des pending ticks globaux"
  - dans notre core actuel, globaliser brutalement ces budgets déplace le bug mais ne corrige pas le contrat `header -> payload -> slot state`
  - ne pas repartir sur une transposition directe `CPU::AddPendingTicks()` sans d'abord corriger le réveil guest autour du header-only

### Nouvel outil de référence STR (2026-04-02)
- Ajout de [`scripts/str_dump.py`](E:/Projects/github/Live/R3000-Emu/scripts/str_dump.py)
  - dump sector-by-sector pour fichiers `.STR` raw `2336` et secteurs raw `2352`
  - décode :
    - sous-header XA (`file/channel/submode/coding`)
    - flags utiles (`REALTIME|VIDEO`, `REALTIME|FORM2|AUDIO`, etc.)
    - header vidéo STR (`frame`, `chunk`, `chunk_count`, `demux`, `width`, `height`)
  - export CSV optionnel pour comparaison offline
- Référence générée sur `hello_strplay` :
  - [`logs/copyings_str_dump_sample.csv`](E:/Projects/github/Live/R3000-Emu/logs/copyings_str_dump_sample.csv)
- Signal utile observé sur `copyings.str` :
  - début de flux = **11 secteurs vidéo + 1 secteur XA audio**
  - sur les 32 premiers secteurs :
    - `frame 1 = 11/11`
    - `frame 2 = 10/10`
    - `frame 3 = 7/11` sur cette fenêtre courte (chunks `0..6` seulement vus)
- Ce dump devient la référence simple pour distinguer :
  - ce que contient réellement le fichier STR
  - de ce que le runtime guest finit par consommer ou rater
- Extension ajoutée :
  - `str_dump.py --compare-trace ... --lba-base N`
  - compare les entrées `ISORAW`, `CDUSER`, `CDFIFO`, `DMA3DATA` d'une timeline contre un STR de référence
- Résultat utile immédiat :
  - avec les fichiers locaux `copyings.str` et `copyings2.str`, la comparaison contre `str_pipeline_baseline.timeline.txt` ne matche pas byte-for-byte
  - donc ces STR loose ne sont pas encore une référence parfaite du flux réellement lu comme `/COPY.STR` dans le disque de test
  - pour une référence finale stricte, il faudra comparer soit :
    - contre le bon asset extrait directement du disque
    - soit faire évoluer l'outil pour ouvrir la CUE/BIN et résoudre `/COPY.STR` lui-même

### Nouveau résultat important (2026-04-02, cycle exact du slot bloqué final)
- La boucle finale n'est plus seulement "slot 7 vaut 2"; on a maintenant son cycle exact dans les traces ring :
  - à `lba=128`, `pc=0x80015F50` remet massivement `slot 0..10` à `0`, y compris :
    - `slot 6: 2 -> 0`
    - `slot 7: 2 -> 0`
  - à `lba=131`, `pc=0x80016494` remet encore :
    - `slot 7: 352 -> 0`
  - immédiatement après, `data_ready_callback` (`pc=0x80019224`) refait :
    - `slot 7: 0 -> 2`
    - sans faire avancer `e10` (reste `7`)
- Conséquence :
  - la boucle `StGetNext idx=7 slot_status=2` n'est pas un reliquat passif
  - c'est un faux "ready" réarmé après reset sur le même slot
- Lecture de debug la plus forte à ce stade :
  - le verrou restant ressemble à une promotion `0 -> 2` trop optimiste dans `data_ready_callback()`
  - ou à un slot que `StCdInterrupt` invalide/réinitialise puis que `data_ready_callback()` réexpose immédiatement sans vraie progression consommable
- Prochaine cible recommandée :
  - capturer le contexte exact de `pc=0x80016494` (header/slot courant)
  - puis comparer ce que `data_ready_callback()` considère "prêt" avec ce que `StGetNext()` considère "consommable"

### IMPORTANT — Règles de debug
- **Relire ce doc** avant chaque session
- **Mettre à jour** après chaque découverte
- **NE PAS** changer les timing values (112000, 20000/MB) sans comprendre pourquoi
- **NE PAS** ajouter des dizaines de hooks en même temps
- **UN hook à la fois**, vérifier, retirer, passer au suivant
- **Toujours vérifier** que le build compile et que frame 2 reste à 200/200

---

## Adresses clés (hello_strplay)

| Item | Address |
|------|---------|
| Game main loop | 0x800101C0 |
| DecDCTvlc | 0x80015A78 |
| StGetNext | 0x80015F88 |
| StCdInterrupt | 0x80016068 |
| StFreeRing | 0x80015ED8 |
| data_ready_callback | 0x80019218 |
| C_011_OBJ_8B0 | 0x80016918 |
| C_011_OBJ_948 (DMA3 start) | 0x800169B0 |
| C_011_OBJ_A20 (DMA3+IRQ) | 0x80016A88 |
| DAT_80033e38 (armed flag) | 0x80033E38 |
| DAT_80033df8 | 0x80033DF8 |
| Ring buffer base | ~0x801F0360 |
| Primary Huffman table | 0x8001CEF0 |
| Secondary Huffman table | 0x8002CEF0 |

## Fichiers modifiés cette session
- `src/cdrom/cdrom.cpp` — 8 buffers, XA filter, check_sector_read_complete
- `src/cdrom/cdrom.h` — SB struct, is_streaming_mode()
- `src/r3000/bus.cpp` — DMA0/DMA1/DMA3 CDROM ticks
- `src/r3000/cpu.cpp` — hook `STR_CB`, traces ring/stage67
- `src/mdec/mdec.cpp` — raw dump, frame dump PPM
- `logs/str_data_ready_bios_240.stderr.txt` — preuve que `data_ready_callback` tourne plusieurs fois par `frameCount`
- `logs/str_dma3irq_words_real_full.txt` — preuve que l'edge IRQ utile tombe sur `DMA3 8 words`, pas `504`
- `logs/str_ready_focus_120.txt` — preuve que `data_ready_callback` arrive via `ra=0x8001107C` et saute `slot1/slot2`
