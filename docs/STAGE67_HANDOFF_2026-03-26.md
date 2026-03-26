# STAGE67 HANDOFF (2026-03-26)

> Remplace `STAGE67_HANDOFF_2026-03-25b.md`.

---

## BREAKTHROUGH : Rescue Workaround Removed

### Cause racine du crash UE5 / BIOS panic

**Bug** : `src/r3000/bus.cpp` lignes ~2976-3026 — "rescue workaround" qui forçait tous les événements BUSY→READY après 50 VBlanks sans GPU output.

**Symptôme** : `A(0xA1)` panic loop à `pc=0x000000A4` après VBlank 460 (CLI) / crash UE5.

**Mécanisme** :
1. Le jeu fait du chargement CD — aucun GPU output pendant 50+ VBlanks (normal)
2. Le rescue détecte "stuck for 50 VBlanks" et marque TOUS les événements 0x2000→0x4000
3. Les événements d'erreur CDROM (spec=0x80, spec=0x8000) deviennent READY
4. Le handler d'erreur BIOS ISO reader à `0xBFC07EE4` (errcode=68) panique
5. `A(0xA1)` loop à PC=0x000000A4

**Investigation** (3 sessions) :
- Watchpoints EVT_TBL dans write_u8/write_u16/write_u32 → aucun hit
- DMA3_EVT, DMA1_EVT, DMA2_EVT, DMA6_EVT → tous à 0 hits
- DMA4 SPU→RAM → pas de transferts du tout
- Finalement trouvé : les writes directs à `ram_[]` à lignes 3019-3022 BYPASSING tous les watchpoints bus

**Fix appliqué** : Bloc `if (vblank_stuck_count_ >= 50)` supprimé + remplacé par commentaire explicatif.

### Résultat du run 300s (après fix)

```
VBlank #297 : CD events BUSY (chargement commence) ✓
VBlanks #297-306 : Event[1] (spec=0x20) cycle READY/BUSY (DMA3 actif) ✓
VBlank #446 : Dernière frame GPU (mode=1 linked-list) ✓
VBlanks #462, #467 : Event[1] encore actif (CD reads continuent) ✓
VBlank #559 : CloseEvent sur les 5 événements CDROM à pc=0x8016D4F4 ✓ LOADING COMPLET!
VBlank #951 atteint sans panic ✓
```

La séquence CloseEvent à `pc=0x8016D4F4` (VBlank 559) = fin du chargement CD. Le jeu a correctement chargé ses données.

---

## État actuel

**CD loading phase : RÉSOLU.** Le jeu charge sans panic jusqu'à VBlank 559+.

### Nouvelle stagnation : après VBlank 446

Pas de GPU output entre VBlank 446 et VBlank 559. Après le CloseEvent (559), probablement une autre phase de loading ou state machine.

**Comportement similaire à Ridge Racer** (CLAUDE.md) : stuck après chargement initial, game-side state machine bloquée avant title screen.

### Prochaines étapes recommandées

1. **Build Release** — Debug = ~3 VBlanks/sec, trop lent pour observer le title screen
2. **Identifier pc=0x8016D4F4** — CloseEvent after loading = début de transition vers titre
3. **Tracer le slot state machine à 0x800E57D4** — avance-t-il après VBlank 559?
4. **Examiner ce qui se passe entre VBlank 446 et 559** — le jeu attend-il quelque chose?

---

## UE5

Le fix bus.cpp supprime le rescue → idem pour UE5 (code partagé via symlink).
Trigger rebuild : `touch integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000Core_Bus.cpp`

---

## Code modifié

`src/r3000/bus.cpp` — bloc supprimé (lignes ~2976-3026 dans version précédente) :
```cpp
// SUPPRIMÉ: rescue workaround if (vblank_stuck_count_ >= 50)
// Ce bloc forçait TOUS les événements BUSY→READY après 50 VBlanks vides.
// Détruit Tekken : CD loading = 50+ VBlanks vides = rescue fires = CDROM error events READY = BIOS panic.
```

---

## Invalide / ne pas poursuivre

- Init spam (gate=0) : **RÉSOLU**
- Fade loop infinie : **RÉSOLU**
- start_slot jamais appelé : **RÉSOLU**
- BIOS Init loop (motor_off) : **RÉSOLU**
- DMA3 jamais actif : **RÉSOLU**
- CdInit infinite retry : **RÉSOLU** (INT2 fix)
- BIOS panic / A(0xA1) loop : **RÉSOLU** (rescue removal 2026-03-26)

---

## Fichiers

- Log 120s (avec rescue → crash) : `logs/tekken_dma6_evt.txt`
- Log 300s (sans rescue → OK) : `logs/tekken_300s.txt`
- BIOS : `E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin`
- CUE : `E:/Projects/PSX/roms/Tekken (Europe).cue`
