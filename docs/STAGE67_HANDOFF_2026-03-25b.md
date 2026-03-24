# STAGE67 HANDOFF (2026-03-25 soir)

> Remplace `STAGE67_HANDOFF_2026-03-25.md`.

---

## BREAKTHROUGH : Init INT2 Fix

**Fix appliqué** : `src/cdrom/cdrom.cpp` — Init (0x0A) second response changé de INT3 (0x03) à INT2 (0x02).

### Preuve BIOS (disassembly)

Le BIOS CDROM IRQ handler à `0xBFC04F00` route selon a3 = INT_FLAG value :

```
a3=1 (INT1) → JAL 0xBFC0593C   ; data sector ready
a3=2 (INT2) → JAL 0xBFC05558   ; ← CRITIQUE pour Init
a3=3 (INT3) → JAL 0xBFC05194   ; command complete (binary search)
a3=4 (INT4) → JAL 0xBFC05A44   ; disc end
a3=5 (INT5) → JAL 0xBFC057B0   ; error
```

Fonction `0xBFC05558` avec state=0xCCC (3276) :
```
LW v0,*0xA0009154    ; v0 = state = 3276
ADDIU at,zero,18     ; at = 18
BNE v0,at → branch  ; state != 18, branch
; delay slot: ADDIU at,zero,3276
; → arrive à 0xBFC056E4 avec at=3276
BEQ v0,at → 0x5648  ; state == 3276, branch to success!
; À 0x5648 :
ADDIU t6,zero,1
SW t6,-28220(at)     ; *0xA00091C4 = 1 ← CdInit réussit!
```

La fonction `0xBFC05194` (INT3 handler) fait une binary search et pour state=3276 → exit à `0x554C` (JR ra) sans écrire le flag. C'est pourquoi INT3 ne marchait pas.

### Résultat du run 120s

```
fade loop 50 VBlanks ✓
cd_event_init → cd_kickoff → gate=0xFFFF ✓
start_slot active ✓
CdInit : INT3 + INT2 → *0xA00091C4=1 → succès ✓
CD driver : SetLoc(LBA≈60713) → SeekL → SetMode → ReadN ✓
DMA3 CD→RAM : madr=0x80170000..0x80173000 (séquentiel) ✓
DMA2 GPU linked-list : 0x80076B90, 0x80077B60, 0x801E0040 ✓
VBlank #451 atteint ✓
```

---

## État actuel

Le jeu progresse jusqu'à VBlank 451 dans un run de 120s (debug mode). Le GPU reçoit des draw lists (OT linked-list) et le CD charge des données de LBA 60713+. C'est de la vraie géométrie de jeu, pas juste le BIOS boot screen.

### Ce qui reste à investiguer

**Q1 : Slot state machine à 0x800E57D4 reste state=0**

Les traces `STAGE67PRE` montrent state=0 jusqu'à la fin du run. Deux possibilités :
- A) Le jeu a besoin de plus de 120s (en debug mode très lent) pour avancer
- B) Il y a encore un blocage dans le state machine

Tester avec timeout=300s (5 minutes) pour voir si state avance.

**Q2 : Mode du jeu**

Les traces STAGE67GATE montrent `mode=0`. Le mode devrait avancer quand le jeu passe de "loading" à "title screen".

### Prochaines étapes recommandées

1. **Augmenter timeout** à 180-300s et regarder si state/mode avancent
2. **Build Release** pour accélérer l'émulation (actuellement Debug = ~4 VBlanks/sec)
3. **Examiner DMA2 transfers** : les OT addresses (0x80076B90 etc.) correspondent-elles à du contenu Tekken reconnaissable ?
4. **Compter les GPU primitives** : si le jeu rend des polys 3D, c'est le title screen

---

## Code modifié

`src/cdrom/cdrom.cpp` — 3 sites modifiés :

```cpp
// Guard at top of Init handler:
if (pending_irq_type_ == 0x02 && pending_irq_reason_ == 0xAAu)  // was 0x03
    break;

// Second response type:
pending_irq_type_ = 0x02;  // was 0x03 — BIOS INT2 handler writes CdInit success flag

// Delivery path sentinel check:
const bool is_init_completion =
    (pending_irq_type_ == 0x02 && pending_irq_reason_ == 0xAAu);  // was 0x03
```

---

## Invalide / ne pas poursuivre

- Init spam (gate=0) : **RÉSOLU** (busy_ guard + status byte fix)
- Fade loop infinie : **RÉSOLU**
- start_slot jamais appelé : **RÉSOLU**
- BIOS Init loop (motor_off) : **RÉSOLU**
- DMA3 jamais actif : **RÉSOLU**
- CdInit infinite retry : **RÉSOLU** (INT2 fix — 2026-03-25 soir)

---

## Fichiers

- Log : `logs/tekken_init_int2_fix.txt` (120s, INT2 fix)
- BIOS : `E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin`
- CUE : `E:/Projects/PSX/roms/Tekken (Europe).cue`
