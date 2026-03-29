# STR Video Streaming — État du Debug

> **⚠️ CLAUDE: LIRE CE FICHIER AU DÉBUT DE CHAQUE SESSION DE DEBUG STR !**
> **Mettre à jour ce fichier APRÈS chaque découverte ou changement.**
> **NE PAS ajouter de hooks debug sans d'abord relire ce doc.**

---

## État actuel (commit 0f4723a, 2026-03-29 20:12)

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
- **Image PPM** : les blocs semblent mélangés (possible problème de rendu, pas de MDEC)

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

### StCdInterrupt (0x80016068)
- Vérifie `DAT_80033e38` (armed flag). Si == 1 → **return immédiat** (skip sector)
- Si != 1 : lit le secteur via DMA3, copie dans ring buffer
- **Dernier secteur** (`sector_num == sector_count - 1`) :
  - Met `DAT_80033e38 = 1` (arme le flag)
  - DMA3 avec **IRQ enabled** (via C_011_OBJ_A20)
  - Quand DMA3 complète → DICR flag → I_STAT bit 3 → `data_ready_callback`
- **data_ready_callback** (0x80019290) :
  - Met status = 2 (frame complete) → StGetNext peut retourner la frame
  - Met `DAT_80033e38 = 0` (désarme) → StCdInterrupt reprend le traitement

### Le flow normal
```
Secteur 0..N-1 : StCdInterrupt → DMA3 → ring buffer (status=3)
Secteur N (last) : StCdInterrupt → DMA3+IRQ → data_ready_callback → status=2, flag=0
StGetNext : voit status=2 → retourne la frame
DecDCTvlc : lit le ring buffer (tous les secteurs sont là)
Secteur 0 frame+1 : StCdInterrupt → DMA3 → ring buffer (flag=0, process OK)
```

---

## Prochaine étape à investiguer

### Hypothèse : DMA3 completion IRQ ne fonctionne pas correctement

Le dernier secteur utilise DMA3 avec IRQ (`C_011_OBJ_A20` active DICR enable pour ch3).
- Si le DMA3 completion IRQ (I_STAT bit 3) ne fire pas → `data_ready_callback` ne tourne jamais
- → `DAT_80033e38` reste à 1 → StCdInterrupt ignore les secteurs suivants
- → Le jeu appelle StGetNext dans une boucle, mais le flag ne set jamais status=2
- → ??? Pourtant frame 2 marche (200/200) donc le IRQ fire au moins pour les premières frames

### Debug à faire
1. **Vérifier** : est-ce que `data_ready_callback` (0x80019290) est appelé pour CHAQUE frame ?
   - Hook PC==0x80019290, compter les appels
2. **Vérifier** : est-ce que le DICR flag ch3 est set après le DMA3 du dernier secteur ?
   - Logger DICR dans dma_finish(3) quand c'est le dernier secteur d'une frame
3. **Vérifier** : est-ce que I_STAT bit 3 est set après dma_finish(3) ?
4. **Si le IRQ fire** : pourquoi frame 3 n'a que 188/200 ? Vérifier si c'est les derniers secteurs qui manquent ou des secteurs au milieu.

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
| data_ready_callback | 0x80019290 |
| C_011_OBJ_8B0 | 0x80016908 |
| C_011_OBJ_948 (DMA3 start) | 0x800169B0 |
| C_011_OBJ_A20 (DMA3+IRQ) | 0x80016A88 |
| DAT_80033e38 (armed flag) | 0x80033E38 |
| DAT_80033df8 (stream mode) | 0x80033DF8 (=0 for this game) |
| Ring buffer base | ~0x801F0360 |
| Primary Huffman table | 0x8001CEF0 |
| Secondary Huffman table | 0x8002CEF0 |

## Fichiers modifiés cette session
- `src/cdrom/cdrom.cpp` — 8 buffers, XA filter, check_sector_read_complete
- `src/cdrom/cdrom.h` — SB struct, is_streaming_mode()
- `src/r3000/bus.cpp` — DMA0/DMA1/DMA3 CDROM ticks
- `src/mdec/mdec.cpp` — raw dump, frame dump PPM
