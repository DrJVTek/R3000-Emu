# STR Video Streaming — État du Debug

> **⚠️ CLAUDE: LIRE CE FICHIER AU DÉBUT DE CHAQUE SESSION DE DEBUG STR !**
> **Mettre à jour ce fichier APRÈS chaque découverte ou changement.**
> **NE PAS ajouter de hooks debug sans d'abord relire ce doc.**

---

## État actuel (commit b5c0fb6, 2026-03-29 23:40)

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

---

## Prochaine étape à investiguer

### Nouvelle hypothèse de travail

`StGetNext`/`DecDCTvlc` voient un slot `status == 2` **trop tôt** par rapport au nombre réel de secteurs déjà écrits pour la frame.

Autrement dit :
- le problème n'est pas "le callback ne tourne jamais"
- le problème est "le callback / l'état du ring buffer rendent une frame visible trop tôt"
- ce qui colle exactement avec la preuve existante : `DecDCTvlc` démarre avant l'arrivée du dernier secteur

### Debug à faire
1. **Hook `StGetNext`** (`0x80015F88`)
   - Logger `status`, `frameCount`, `secCount`, `nSectors`, index courant
   - Vérifier à partir de quel slot/frame le `status==2` devient visible
2. **Corréler `StGetNext` avec `DecDCTvlc`**
   - Vérifier si `DecDCTvlc` consomme la frame dès le **premier** slot complet d'un frameCount
3. **Vérifier la cohérence du ring**
   - `DAT_80033e08`, `DAT_80033e10`, `DAT_80033e18`
   - et le comportement de `StFreeRing()` qui libère `nSectors` slots d'un coup
4. **Ensuite seulement**, si besoin, revenir sur la signalisation DMA3/DICR

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
- `src/mdec/mdec.cpp` — raw dump, frame dump PPM
- `logs/str_data_ready_bios_240.stderr.txt` — preuve que `data_ready_callback` tourne plusieurs fois par `frameCount`
