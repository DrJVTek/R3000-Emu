# STAGE67 HANDOFF (2026-03-25)

> Remplace `STAGE67_HANDOFF_2026-03-24.md` et `STAGE67_HANDOFF_2026-03-25.md (matin)`.

---

## Point de reprise immédiat

**Problème** : Tekken (Europe) reste bloqué au chargement après stage67.

**État** : CD driver progresse maintenant (ReadN actif, DMA3 OK), mais après avoir lu LBA 4-5 (8 secteurs) le jeu fait `Pause → GetID → Init` en boucle. Le slot state machine à `0x800E57D4` reste `state=0` — les données lues ne font jamais avancer le chargement.

---

## Ce qui a été fixé cette session

### Fix 1 : Init spam (busy_ flag)

**Fichier** : `src/cdrom/cdrom.cpp` — deux sites `busy_ = 0`

**Problème** : Init (0x0A) a deux INT3 responses. La deuxième est marquée par le sentinel `pending_irq_reason_ == 0xAAu`. Avant le fix, `busy_` était effacé même quand cette deuxième INT3 était encore en vol, permettant au jeu de re-queuer Init infiniment.

**Fix** :
```cpp
// ACK path (ligne ~2802) et cmd_exec path (ligne ~2868) :
if (!(pending_irq_type_ == 0x03 && pending_irq_reason_ == 0xAAu))
    busy_ = 0;
```

### Fix 2 : Init premier INT3 status byte

**Problème** : `push_resp(status_)` était appelé avant `set_secondary_idle(true)`. Résultat : le premier INT3 de Init retournait l'ancien status (motor_off possible), la BIOS voyait motor=0 et réessayait Init.

**Fix** : `set_secondary_idle(true)` appelé EN PREMIER, puis `push_resp(status_)`.

---

## Chaîne d'appels complète (confirmée)

```
0x801C0D00 (runtime loader)
  → BSS clear (0x801130D4 écrit à 0)
  → J 0x80030000 (game_main)

0x80030000 (game_main)
  → JAL 0x8004e1e0(0x8004e220) = fade loop (50× VSync, ~2s PAL)
    [fade loop RETOURNE correctement]
  → JAL 0x80035C34 (cd_event_init)
    → alloue event queues (0x80068530/40/50)
    → JAL 0x80064C00 (cd_kickoff)
      → écrit gate=0x0CCC puis gate=0x0000FFFF

0x80065454 (start_slot) : check gate == 0xFFFF ou 3
  [gate=0xFFFF → active → start_slot tourne]
```

---

## Séquence CD actuelle (run 90s)

```
Test(0x19) → GetStat(0x01) → GetID(0x1A) → ReadTOC(0x1E)
→ GetStat(0x01) → GetID(0x1A)
→ SetLoc(MSF=00:02:04 → LBA=4) → SeekL → SetMode(0x80=2x) → ReadN
   DMA3: 0xA0010000, 0xA0010000*, 0xA0010800, 0xA0011000, 0xA0011800 (4+ secteurs)
   Pause
→ SetLoc(MSF=00:02:05 → LBA=5) → SeekL → SetMode(0x80) → ReadN
   DMA3: 0xA0012000, 0xA0012800, 0xA0013000 (3 secteurs)
   Pause
→ GetID(0x1A)
→ Init × spam  ← BLOQUÉ ICI
```

`*` = adresse dupliquée (possible test DMA ou re-trigger)

---

## Problème actuel : Init après GetID

Après la boucle ReadN/Pause/GetID, le CD driver appelle Init et recommence depuis le début. Le slot state machine à `0x800E57D4` reste `state=0 sub=0 done=0 gate=0`.

### Hypothèses

**H1 (la plus probable)** : Le callback INT1 (sector ready) n'avance pas le state machine `0x800E57D4`.
- Les DMA3 transfèrent les données en RAM mais la callback de traitement ne tourne pas
- Possible problème d'événements CDROM non délivrés (voir NOTE dans CLAUDE.md sur deliver_events_for_class)

**H2** : GetID retourne un résultat que le CD driver interprète comme "disque non valide".
- Notre GetID retourne INT3 + INT2 (stat=0x02, flags=0x00, type=0x20, "SCEE")
- Vérifier que le jeu traite bien INT2 et non INT3 pour le second response

**H3** : Le jeu fait volontairement Init(×2) après le disc-check LBA 4-5, puis reprend normalement MAIS notre Init spam guard empêche les Inits suivants.
- Observer si après les 2 Init propres (busy=0), le jeu devrait faire Test/GetStat/GetID/ReadTOC à nouveau pour charger vraiment
- Vérifier si 90s de timeout est suffisant

**H4** : Les secteurs lus (LBA 4-5) contiennent une vérification que notre implémentation rate.
- LBA 4 = secteur dans la System Area du disque PS1
- La vérification pourrait regarder le contenu exact des bytes à des offsets spécifiques

---

## Prochaines étapes recommandées

### 1. Tracer ce que le jeu fait APRÈS les DMA3

Ajouter une trace dans `cpu.cpp` pour les accès RAM à `0x00010000-0x00013FFF` (où les secteurs atterrissent). Voir si le jeu lit ces données et ce qu'il en fait.

### 2. Vérifier le callback INT1

La CB enregistrée via `0x80068550` (cd_event_init) : est-elle appelée lors de chaque INT1 ? Si oui, avance-t-elle `0x800E57D4.state` ?

Ajouter trace dans `cpu.cpp` aux adresses JAL vers ces callbacks.

### 3. Tester avec timeout plus long (>90s)

Le fade loop + 2 cycles complets de ReadN prennent ~40-60s en debug. Peut-être qu'un 3e cycle complet (Init × 2 → Test/GetStat/GetID/ReadTOC/... → chargement réel) a besoin de plus de temps.

Tester avec `--timeout-ms=180000` (3 minutes).

### 4. Comparer avec DuckStation logs

DuckStation sur Tekken (Europe) : capturer la séquence de commandes CD pour comparer. La commande CLI DuckStation peut faire des traces.

---

## Variables RAM importantes

| Adresse | Contenu | État observé |
|---------|---------|--------------|
| `0x801130D4` | Gate pour start_slot | Cycle 0xFFFF→0x19→0x01→0x1A→0xF2→0x15→... |
| `0x800E57D4` | Slot 0 state machine | state=0, sub=0, done=0 (jamais avancé) |
| `0x800E57DC` | Slot 1 state machine | state=0 |
| `0x8009ACFC` | ACFC (VBlank counter) | Progresse normalement |

---

## Fichiers de référence

- Logs récents : `logs/tekken_initfix2_2026-03-25.txt` (run 90s, 2 fixes appliqués)
- Handoff précédent : `docs/STAGE67_HANDOFF_2026-03-25.md` (matin, avant fixes)
- CUE : `E:/Projects/PSX/roms/Tekken (Europe).cue`
- BIOS : `E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin`

---

## Invalide / ne pas poursuivre

- Init spam (gate=0) : **RÉSOLU** (busy_ guard + status byte fix)
- Fade loop infinie : **RÉSOLU** (retourne après 50 iters)
- `start_slot` jamais appelé : **RÉSOLU** (gate=0xFFFF bien écrit)
- BIOS Init loop (motor_off) : **RÉSOLU** (fix status byte ordre)
- DMA3 jamais actif : **RÉSOLU** (ReadN delivre des secteurs)
