# STAGE67 HANDOFF (2026-03-23)

> Lire d'abord [DEBUG_UE5_STUCK.md](E:/Projects/github/Live/R3000-Emu/docs/DEBUG_UE5_STUCK.md), puis ce fichier.
>
> Règle: relire et mettre à jour `DEBUG_UE5_STUCK.md` à chaque changement de diagnostic.

## Situation actuelle

- UE5 et CLI bootent maintenant correctement jusqu'au code jeu/shell.
- Le problème n'est plus le BIOS/CD.
- Le problème n'est plus spécifique UE5.
- Le stall restant existe aussi en CLI dans la zone appelée `stage67`.

## Ce qui est prouvé

1. Boot/CD
- `UE insert_disc OK`
- lecture valide de:
  - `LBA 16` (PVD)
  - `LBA 18`
  - `LBA 22`
  - `LBA 60642` (`SYSTEM.CNF`)
  - `LBA 60643` (`PS-X EXE`)
- handoff BIOS -> code runtime OK

2. UE5 vs CLI
- UE5 atteint:
  - `GAMEPC Reached stage57 pc=0x80057484`
  - `GAMEPC Reached stage67 pc=0x80067800`
- Les traces `STAGE67MMIO` montrent que UE5 matche le CLI sur:
  - `GPUSTAT`
  - `TMR1`
  - `0x8008ACFC`
- Donc le blocage restant n'est pas un bug UE5-only.

3. Runtime réel
- Le code exécuté autour de `0x80054A90` et `0x800677F0` ne matche pas le contenu statique de `TEKKEN.EXE`.
- Le runtime dump est la source de vérité.
- Dumps utiles:
  - `logs/stage67_ram_80050000.bin`
  - `logs/stage67_ram_80040000.bin`

## Corrections importantes déjà faites

- UE5 threaded mode force `BusTickBatch=1`
- CDROM async timing passé en deadlines absolues
- BIOS/CD path corrigé jusqu'au chargement du shell
- plusieurs diagnostics précédents sont maintenant invalidés:
  - pas un bug `SYSTEM.CNF`
  - pas un bug `PS-X EXE`
  - pas un bug `GPUSTAT bit31 stuck`
  - pas un bug `ACFC` mort
  - pas un bug `TMR1` illisible

## Désassemblage runtime utile

### `0x800677F0..0x80067910`
- Ce n'est pas une machine d'état "slot" au sens supposé au début.
- C'est une routine de synchro/wait runtime qui lit:
  - `0x8008ABDC -> 0x1F801814` (`GPUSTAT`)
  - `0x8008ABE0 -> 0x1F801110` (`TMR1`)
  - `0x8008ACFC`
- `0x80067914` fait une attente locale avec timeout stack.
- `0x800678C0` n'est pas une entrée de fonction fiable dans cette version runtime; c'est au milieu d'une boucle.

### `0x80054A90`
- Caller local:
  - `jal 0x8004C920`
  - `jal 0x800677F0`
  - `jal 0x8003D908`

### `0x80054B38..0x80054C70`
- Boucle outer locale.
- `s0 = 50`
- `s5` progresse normalement (`0 -> 32` observé)
- donc cette boucle locale n'est probablement pas le point de blocage principal.

### Caller au-dessus (`0x8004E320..`)
Désassemblage runtime de `logs/stage67_ram_80040000.bin`:

- `0x8004E358 -> jal 0x80054B38`
- retour puis:
- `0x8004E394 -> jal 0x8004E9C0`
- `0x8004E39C -> jal 0x8004EEA8`

Paramètres préparés pour `0x80054B38`:
- `a0 = 0x80120998`
- `a1 = 0x80068A9C`
- `a2 = 0xB4`
- `a3 = 0xB4`
- `[sp+0x10] = 0xB4`
- `[sp+0x14] = 0x32` (50)

## Hypothèse active (mise à jour session 2026-03-23 soir)

### Ce qui est résolu
- `0x8004E9C0` = transition/wipe stepper (init + per-frame advance, counter 0x800B8210 → target 0x800B8228)
- `0x8004EEA8` = palette CLUT fade processor (3 entrées, 16-bit RGB interpolation)
- `0x8004E360..0x8004E54C` = boucle fade-out bloquante (100 frames PAL, 120 NTSC)
- **La boucle fade SORT en CLI** (stop-on-pc=0x8004E54C confirmé)
- **La fonction retourne** au shell BIOS (ra=0x8003009C)
- `0x8008AD40` = flag PAL/NTSC (0=NTSC, 1=PAL), correctement à 1 pour Tekken EU

### Ce qui est validé comme NON-cause
- GPUSTAT bit31 n'est PAS la cause du stall stage67 (matchée avec DuckStation, stall persiste)
- La boucle fade `0x8004E360` n'est PAS le blocage (elle sort OK)
- Le spin-loop `0x800678C4` (XOR bit31) n'est PAS le blocage principal
- La sub `0x80067914` (WaitVBlankCount) a un timeout de 0x8000 et ACFC progresse

### GPUSTAT bit31 — état final
- 480i: `(display_y + (!in_vblank & field)) & 1` — matche DuckStation exactement
- Non-480i: `even_odd_field` (per-frame toggle) — DuckStation utilise `(display_y + current_scanline) & 1`
  mais nécessite un CRTC tick par scanline qu'on n'a pas → TODO futur
- **Ridge Racer 3D : cassé AVANT cette session** (0 origin_3d même sur commit stable f976306)

### Nouvelle cible
Le stall persiste MALGRÉ GPUSTAT correct. La cause est probablement:
1. Un état hardware que DuckStation fournit et pas nous (timer IRQ timing, DMA completion timing)
2. Ou une condition spécifique dans la boucle `0x80054B38` (le VSync/draw loop avec 50 passes)
3. Ou un problème dans `0x8003D908` (appelé depuis `0x80054A90` après VSync)

### DÉCOUVERTE CRITIQUE (session soir 2026-03-23)

**L'entry point de TEKKEN.EXE (`0x80165398`) n'est JAMAIS atteint !**

Le BIOS shell charge le header EXE (LBA 60643) mais ne jump pas à PC0. Le shell
exécute ses routines (fade PlayStation logo, etc.) puis retourne à `0x8003009C`
qui a été **écrasé** par les données du jeu → NOP → données ASCII → crash.

Header EXE analysé :
- PC0 = 0x80165398 (entry point)
- text_dest = 0x0007B800 (pas une adresse RAM KSEG0 !)
- text_size = 0 (bootstrap EXE, pas de section text)
- SP = 0x801FFFF0

Le `text_size = 0` est normal pour Tekken — c'est un bootstrap. Le vrai code
est chargé par le loader à 0x80165398. Mais le BIOS doit quand même jump à PC0.

### Prochaine étape recommandée
1. Comprendre POURQUOI le BIOS shell ne jump pas à PC0 = 0x80165398
2. Vérifier le flow Exec() du BIOS SCPH-7502 dans le code ROM
3. Possibilité : text_dest = 0x0007B800 (KUSEG, pas KSEG0) cause un problème
   dans le chargement — le BIOS pourrait rejeter l'adresse
4. Ou : text_size = 0 fait que le BIOS skip entièrement l'exec
5. Comparer avec DuckStation : est-ce que le regtest atteint 0x80165398 ?

## Fichiers et logs à utiliser

### Docs
- `docs/DEBUG_UE5_STUCK.md`
- `docs/STAGE67_HANDOFF_2026-03-23.md` (ce fichier)

### Runtime dumps
- `logs/stage67_ram_80050000.bin`
- `logs/stage67_ram_80040000.bin`

### Logs CLI utiles
- `logs/tekken_cli_stage67_mem_2026-03-23.err.txt`
- `logs/tekken_cli_stage67_main_2026-03-23.err.txt`
- `logs/tekken_cli_stage67_loop_2026-03-23.err.txt`
- `logs/tekken_cli_stage67_up_2026-03-23.err.txt`
- `logs/tekken_cli_stage67_post_2026-03-23.err.txt`
- `logs/tekken_cli_stage67_post2_2026-03-23.err.txt`

### Ghidra
- Le code statique `TEKKEN.EXE` n'est pas fiable pour cette zone runtime.
- Utiliser les dumps RAM:
  - charger `stage67_ram_80050000.bin` à `0x80050000`
  - charger `stage67_ram_80040000.bin` à `0x80040000`

### DuckStation
- Source:
  - `E:\\Projects\\github\\Live\\duckstation`
- Binaire GUI installé:
  - `E:\\Projects\\PSX\\duckstation\\duckstation-qt-x64-ReleaseLTCG.exe`
- Runner CLI buildé:
  - `E:\\Projects\\github\\Live\\duckstation\\bin\\x64\\duckstation-regtest-x64-Release-MSVC.exe`
- Usage recommandé:
  - s'en servir comme **référence runtime**
  - comparer le chemin avant/après `stage67`
  - ne pas perdre du temps à re-débugger le boot CD si DuckStation n'a pas de divergence à ce niveau
- Si Claude veut réinstrumenter DuckStation:
  - travailler dans `E:\\Projects\\github\\Live\\duckstation`
  - pas dans le binaire installé

## Ce qu'il faut faire ensuite

1. Continuer en CLI, pas en UE5.
2. Garder les traces autour de `0x8004E360..0x8004E3C0`.
3. Faire apparaître `STAGE67POST` de façon fiable:
- réduire encore le bruit si nécessaire
- ou tracer uniquement `0x8004E9C0` / `0x8004EEA8`
4. Désassembler runtime `0x8004E9C0` et `0x8004EEA8` depuis `stage67_ram_80040000.bin`.
5. Mettre à jour `DEBUG_UE5_STUCK.md` avant de repartir sur une nouvelle hypothèse.

## Erreurs à éviter

- Ne pas repartir sur le CD/BIOS.
- Ne pas utiliser `TEKKEN.EXE` seul comme vérité pour cette zone.
- Ne pas réintroduire des hypothèses déjà invalidées sans mise à jour du doc.
- Ne pas continuer sans relire `DEBUG_UE5_STUCK.md`.
