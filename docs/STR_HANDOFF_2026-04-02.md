# STR Handoff - 2026-04-02

Ce document sert de reprise rapide pour `hello_strplay`.

## État courant

- Baseline runtime encore observée :
  - `Frame 1 = 200/200`
  - `Frame 2 = 200/200`
  - `Frame 3 = 188/200`
- Le blocage restant n'est plus un doute sur la lecture CD brute.

## Conclusion forte acquise aujourd'hui

La chaîne suivante est maintenant validée bit-for-bit sur la fenêtre baseline autour de `lba=110..124` :

- `ISORAW`
- `CDUSER`
- `CDFIFO`
- `DMA3DATA` header
- `DMA3DATA` payload

Donc le bug restant n'est pas :

- un mauvais LBA disque
- un mauvais secteur lu
- une mauvaise exposition FIFO CD
- un mauvais transfert `DMA3`

Le problème restant est après, dans la logique guest / player / consommation.

## Outil ajouté

Nouveau script :

- [`scripts/str_dump.py`](E:/Projects/github/Live/R3000-Emu/scripts/str_dump.py)

Fonctions utiles :

- dump d'un `.STR` brut
- résolution directe d'un fichier ISO9660 depuis une `CUE/BIN` via `--iso-file /COPY.STR`
- comparaison avec les traces runtime
- auto-alignement des secteurs avec :
  - `--auto-align`
  - `--align-source VIDEOHDR`

## Commande de référence

Commande qui prouve la bonne lecture disque/CD/DMA3 :

```powershell
python E:/Projects/github/Live/R3000-Emu/scripts/str_dump.py `
  E:/Projects/PSX/nolibgs_hello_worlds/hello_strplay/hello_strplay.cue `
  --iso-file /COPY.STR `
  --start 0 `
  --count 40 `
  --summary-only `
  --compare-trace E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.timeline.txt `
  --lba-base 110 `
  --auto-align `
  --align-source VIDEOHDR `
  --align-radius 64
```

Résultat attendu :

- `best lba base: 85`
- `compared entries: 53`
- `mismatches: 0`

Artefact déjà généré :

- [`logs/str_pipeline_baseline.compare.txt`](E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.compare.txt)

## Interprétation de l'alignement

Le mauvais diagnostic précédent venait d'un mauvais `lba_base` manuel.

- `lba=110` dans la timeline n'est pas "secteur 0 du fichier"
- le bon départ du fichier `/COPY.STR` sur ce disque est `lba_base = 85`

Une fois cet alignement corrigé, toute la fenêtre comparée matche.

## Point exact où ça casse encore

Fenêtre critique :

- `read_lba=117`
- secteur vidéo reçu = `frame 3 / sec 7`
- `CDUSER` est correct :
  - `w1=0x000B0007`
  - `w2=0x00000003`
  - `w3=0x00003834`

Mais au même moment dans la timeline :

- [`logs/str_pipeline_baseline.timeline.txt`](E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.timeline.txt)
- `STPATH` montre avant cela :
  - `e08=7`
  - `e10=6`
  - `slot_status=2`
  - `cur_frame=3`
  - `exp_sec=7`
- `STRINTR` passe par `0x80016050 .. 0x8001617C`
- on voit ensuite un `STFREE` puis un reset de slots
- juste après, `lba=118` arrive déjà

Autrement dit :

- le bon secteur arrive bien
- mais la logique guest autour de `StCdInterrupt` / ring status / `StFreeRing` réagit de travers

## Hypothèse de travail la plus solide

Le verrou restant est dans l'étage supérieur, probablement un de ceux-ci :

- `StCdInterrupt`
- `StGetNext`
- `StFreeRing`
- `data_ready_callback`
- ou la cadence `DMA0 -> MDEC -> DMA1 -> strCallback`

La suspicion la plus forte reste :

- un slot ring vu occupé trop longtemps
- ou libéré/réarmé dans le mauvais ordre
- ce qui fait rater le moment utile pour `frame 3 sec 7`

## Ce qu'il ne faut plus retester en priorité

À éviter tant qu'aucune nouvelle preuve ne l'impose :

- retoucher encore le timing CD global "au hasard"
- douter à nouveau de `DMA3`
- douter à nouveau du contenu lu depuis `/COPY.STR`

Ces couches ont maintenant une preuve de correction sur la fenêtre baseline.

## Prochain meilleur pas pour Claude

1. Partir de `read_lba=117` dans [`logs/str_pipeline_baseline.timeline.txt`](E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.timeline.txt).
2. Reprendre le pseudocode Ghidra de `StCdInterrupt`.
3. Corréler précisément :
   - `slot_status`
   - `e08`
   - `e10`
   - `e18`
   - `slot_ptr`
   - `StFreeRing`
   - `data_ready_callback`
4. Répondre à cette question unique :
   - pourquoi `frame 3 sec 7` est bien livré par le CD, mais n'est pas consommé au bon moment par le player ?

## Fichiers clés

- [`scripts/str_dump.py`](E:/Projects/github/Live/R3000-Emu/scripts/str_dump.py)
- [`docs/STR_DEBUG_STATUS.md`](E:/Projects/github/Live/R3000-Emu/docs/STR_DEBUG_STATUS.md)
- [`docs/STR_HANDOFF_2026-04-02.md`](E:/Projects/github/Live/R3000-Emu/docs/STR_HANDOFF_2026-04-02.md)
- [`logs/str_pipeline_baseline.timeline.txt`](E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.timeline.txt)
- [`logs/str_pipeline_baseline.compare.txt`](E:/Projects/github/Live/R3000-Emu/logs/str_pipeline_baseline.compare.txt)
- `hello_strplay.map`
- `hello_strplay.elf`

