# STAGE67 HANDOFF (2026-03-24)

> Lire d'abord [DEBUG_UE5_STUCK.md](E:/Projects/github/Live/R3000-Emu/docs/DEBUG_UE5_STUCK.md), puis ce fichier.
> Ce fichier remplace `STAGE67_HANDOFF_2026-03-23.md` pour cette branche.

## Point de reprise immédiat

1. **Utiliser le CLI, pas UE5** — CLI et UE5 convergent au même stall stage67
2. **CUE correct** : `E:/Projects/PSX/roms/Tekken (Europe).cue`
   - IMPORTANT: PAS `Tekken (Europe) (En,Fr,De,Es,It).cue` (n'existe pas)
   - Le run `tekken_live_pc_2026-03-24.err.txt` était avec le mauvais chemin → fausse piste
3. **Le CD/BIOS/GetID n'est plus le problème** — disc s'ouvre correctement avec le bon CUE
4. **Le stall actuel** : boucle `0x80054B38` (50 passes) ne termine pas

## Ce qui est prouvé

### Boot/CD
- Secteurs chargés OK (260 secteurs, code runtime en RAM)
- `insert_disc result: 1` dans les runs corrects
- `stage57` et `stage67` atteints

### Boucle outer `0x80054B38`
Structure désassemblée depuis `stage67_ram_80050000.bin` :

```
0x80054B38: Prologue, sauve registres
0x80054B44: LH $s0, 116($sp)    ; s0 = 50 (compteur max depuis stack)
0x80054B64: BLTZ $s0, exit      ; si s0 < 0 sortir
0x80054B68: ADDU $s5, $zero     ; s5 = 0 (compteur courant)

; === Corps de boucle (entrée: 0x80054B90) ===
0x80054B90: LW $a0, 96($sp)
0x80054B94: JAL 0x8004C89C      ; init iteration (inconnue)
0x80054B9C: DIV $s1, $s0        ; interpolation
...
0x80054C3C: JAL 0x8004C88C      ; inconnue
0x80054C48: JAL 0x8004C8E8      ; inconnue (a1=2)
0x80054C54: JAL 0x80054A90      ; ← appel principal (VSync + GPU)
0x80054C5C: ADDIU $s5, $s5, 1   ; s5++
0x80054C60: SLT $at, $s0, $s5   ; at = (50 < s5)?
0x80054C64: ADDU $s1, $s1, $s7  ; update interpolation
0x80054C68: ADDU $s2, $s2, $s8
0x80054C6C: BEQ $at, $zero, 0x80054B90  ; si s5<=50, reboucler
0x80054C70: ADDU $s3, $s3, $s6
; === Fin de boucle (sortir quand s5 > s0=50) ===
```

**Résultat observé** : DMA GPU actif, VBlanks #125-134+, mais boucle ne termine pas en 50 itérations.

### `0x80054A90` — appel principal
```
0x80054A98: JAL 0x8004C920      ; ← inconnue, appelée AVANT VSync
0x80054AA0: JAL 0x800677F0      ; VSync wait
0x80054AA8: JAL 0x8003D908      ; ← inconnue, appelée APRÈS VSync
```

### `0x800677F0` — VSync wait (désassemblée)
- Lit `mem[0x8009ABDC]` = ptr GPUSTAT (0x1F801814)
- Lit `mem[0x8009ABE0]` = ptr TMR1 (0x1F801110)
- `a0` = paramètre de contrôle
  - `a0 < 0` → retour immédiat avec v0=ACFC
  - `a0 == 1` → retour avec v0=TMR1 delta
  - `a0 >= 2` → appelle WaitVBlankCount (`0x80067914`) deux fois
- Après le 2ème wait: vérifie GPUSTAT bit19 (interlace 480i)
  - Si bit19=0 (240p, cas Tekken EU) → saute le spin-loop bit31, retour direct
  - Si bit19=1 (480i) → attend changement bit31 GPUSTAT

### `0x80067914` — WaitVBlankCount (désassemblée)
```
a0 = valeur cible ACFC à attendre

1. Si ACFC >= a0 déjà → retour immédiat
2. Sinon : poll ACFC en boucle (timeout 0x8000 itérations)
   - Chaque tour: timeout--
   - Si timeout atteint 0 → appelle 0x800684A0 (handler d'erreur) puis retour
3. Si ACFC >= a0 → retour normal
```
ACFC = `mem[0x8009ACFC]` incrémenté par VBlank ISR à `0x8006812C`.

### `STAGE67FN` trace (symptôme visible)
`pc=0x800678C0 fn=init_slot slot=N ra=0x80067888 state=0 sub=0 done=0`
→ appelé depuis `0x80067888` (retour de WaitVBlankCount) à chaque VBlank
→ `state=0 done=0` toujours = slot jamais avancé

**Mais** : DMA GPU actif = les frames sont bien rendues. Le "stall" n'est donc pas un vrai freeze,
c'est la boucle de 50 passes qui tourne trop longtemps (ou indéfiniment).

## Hypothèse active

La boucle `0x80054B38` (50 passes) prend trop longtemps à cause de `0x80054A90` :

Option A : `0x8004C920` (avant VSync) met un état qui ralentit ou bloque
Option B : `a0` passé à `0x800677F0` est > 1, causant une attente de N VBlanks par itération
Option C : `0x8003D908` (après VSync) prend du temps (DMA3, CD, etc.)

## Ce qu'il faut faire ensuite

1. **Tracer `0x80054A90`** : quelle valeur de `a0` est passée à `0x800677F0` ?
   ```bash
   ./lib/Release/r3000_emu.exe \
     --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" \
     --cd="E:/Projects/PSX/roms/Tekken (Europe).cue" \
     --timeout-ms=45000 \
     --cd-timing=compat
   ```
   Avec trace de `a0` à `0x80054AA0` (juste avant JAL 0x800677F0).

2. **Désassembler `0x8004C920`** depuis `stage67_ram_80050000.bin`
   - 0x8004C920 - 0x80050000 = négatif → pas dans ce dump
   - Chercher dans `stage67_ram_80040000.bin` : 0x8004C920 - 0x80040000 = 0xC920 ✓

3. **Désassembler `0x8003D908`**
   - 0x8003D908 - 0x80040000 = négatif → pas dans les dumps RAM actuels
   - Besoin d'un dump 0x80030000-0x80040000 si pas déjà disponible

## Invalide / ne pas poursuivre

- CD/BIOS/GetID : fermé (bon disc, boot OK)
- `tekken_live_pc_2026-03-24.err.txt` : mauvais CUE → ignorer ce log
- GetID INT5 : était dû au disc null (mauvais chemin), pas un vrai bug CD
- GPUSTAT bit31 : corrigé, matchait DuckStation, n'était pas la cause
- Boucle fade `0x8004E360` : sort correctement (`stop-on-pc=0x8004E54C` confirmé)

## Fichiers utiles

### Dumps RAM
- `logs/stage67_ram_80050000.bin` — base 0x80050000, taille 0x20000
  - Contient: `0x80054B38` (outer loop), `0x800677F0` (VSync wait), `0x80067914` (WaitVBlankCount)
- `logs/stage67_ram_80040000.bin` — base 0x80040000, taille 0x10000
  - Contient: `0x8004C920`, et zone 0x8004XXXX

### Logs de référence (disc OK)
- `logs/cli_compat_probe_2026-03-23.txt` — 45s, pas de stop-on-pc, disc OK
- `logs/cli_realistic_probe_2026-03-23.txt` — idem, cd-timing=realistic

### CUE correct
`E:/Projects/PSX/roms/Tekken (Europe).cue`
