# DEBUG: R3000-Emu UE5 Integration

> **⚠️ CLAUDE: RELIRE CES FICHIERS À CHAQUE NOUVELLE SESSION !**
>
> 1. **`CLAUDE.md`** (racine projet) - Config, chemins, préférences
> 2. **Ce fichier** (`docs/DEBUG_UE5_STUCK.md`) - Historique debug complet
>
> **L'utilisateur préfère le mode NON-HLE (bHleVectors=false).**

---

## 📌 ÉTAT ACTUEL (2026-02-18) - VOITURE TROP RAPIDE (timing global OK, root counters suspects)

### Résultat session: timing wall-clock parfait, physique toujours fausse

**Problème confirmé:** Ridge Racer voiture trop rapide. Comparaison DuckStation confirme: bug chez nous.

**Ce qui marche (vérifié par logs TIMING):**
- ✅ 100% speed (33.87M cyc/s)
- ✅ 59-60 VBl/s (NTSC correct)
- ✅ Delta-time loop stable (debt ≈ 0)
- ✅ BusTickBatch=32 → perf OK sur Ryzen 9 7950X3D
- ✅ Pad input fonctionne (SIO0 v24)
- ✅ Per-instruction cycles (GTE 5-44, MUL 13, DIV 36)

**Ce qui NE marche PAS:**
- ❌ Voiture va trop vite malgré timing global correct
- ❌ DuckStation montre que le jeu DEVRAIT être plus lent → bug chez nous

**Hypothèse principale: Root Counters (Timers) mal implémentés**
- Le jeu lit probablement un hardware timer (root counter) pour calculer le delta-time physique
- Si nos timers comptent trop vite/lent par rapport au vrai PS1, la physique est faussée
- Le timing VBlank est correct (571,088 cycles) mais les valeurs lues par le jeu via timer registers pourraient être fausses

**Piste de debug pour prochaine session:**
1. Logger les accès timer du jeu: quand il lit timer.count (0x1F801100/1110/1120), logger la valeur retournée
2. Comparer avec DuckStation: activer le log timer de DuckStation et comparer les valeurs
3. Vérifier: mode du timer (sysclk vs sysclk/8 vs dotclock vs hblank), target value, reset-on-target
4. Possibilité: timer overflow/wrap-around bug (count est uint32_t mais hardware est 16-bit)

**Note perf:** `EmuLogLevel` doit être `warn` minimum pour voir les logs TIMING (pas `info`)

### Changements cette session

1. **Per-instruction cycle counting (cpu v7, gte v10):**
   - GTE: cycles réels par commande (RTPS=15, RTPT=23, NCLIP=8, MVMVA=8, NCDT=44, etc.)
   - MULT/MULTU: 13 cycles, DIV/DIVU: 36 cycles
   - Tous les autres: 1 cycle
   - `cpu.last_cycles()` retourne le coût de la dernière instruction
   - CPI mesuré: ~1.0-1.17 (la plupart des instructions coûtent 1 cycle)

2. **Delta-time worker thread (R3000EmuComponent.cpp):**
   - Remplace l'ancien modèle absolute-time + idle Bus::tick phantom
   - `double CycleDebt` accumule `DeltaTime * 33868800.0` chaque itération
   - Execute `step()` tant que `CycleDebt > 0`, soustrait `last_cycles()`
   - Si en avance (debt négatif), sleep via WaitableTimer
   - Pause/resume reset proprement LastTime + CycleDebt

3. **Timer refactoring (bus v25):**
   - Bit 10 = `interrupt_request_n`, PAS un stop flag
   - One-shot/repeat (bit 6) avec `irq_done` flag
   - Pulse/toggle (bit 7) IRQ modes
   - Sync/gate counting logic (`counting_enabled`)

4. **BusTickBatch default 1→32:**
   - Avec batch=1: 85% speed (34M appels bus.tick/sec)
   - Avec batch=32: 100% speed (~1M appels bus.tick/sec)

5. **Diagnostics TIMING** (niveau `warn`, system.log + UE_LOG):
```
TIMING: 100.0% speed | 33.87M cyc/s (target 33.87M) | CPI=1.17 | 59.5 VBl/s (target 60) | debt=-1 cyc
```

**Versions:**
| Fichier | Version | Description |
|---------|---------|-------------|
| bus.cpp | v25 | timer_refactor + vblank_count() |
| cpu.cpp | v7 | per_instr_cycles (last_instr_cycles_) |
| gte.cpp | v10 | per_cmd_cycles (5-44 cycles) |
| gpu.cpp | v7 | NTSC_timing, bit31_toggle |

**⚠️ IMPORTANT:** timer.count est `uint32_t` dans notre code mais le hardware PS1 est **16-bit**. Vérifier si overflow/masking est correct.

---

## 📌 HISTORIQUE - PAD INPUT FIX (bus v24)

### Investigation: Pad ne marche pas malgré SIO0 correct (bus v13→v23)

**Symptôme:** Boutons Xbox gamepad détectés par UE5 (0xFFDF), SIO0 retourne les bons
octets (buttons_lo=0xDF), mais le jeu Galaxian ne réagit pas.

**Diagnostic (v21):**
1. SIO0 transfers complets via polling (5 bytes, correct data)
2. I_MASK = 0x000D → pas de SIO0 IRQ (bit 7) → BIOS SIO0 handler ne tourne pas
3. v21 force I_MASK bit 7 → IRQ fires MAIS 8/10 perdus (IEc=0 pendant VBlank)
4. 2 SIO0 exceptions arrivent au CPU (PC=0xBFC091AC) mais APRÈS le transfert
5. Event 0xF0000009 (SIO0/pad) reste BUSY → le jeu ne voit jamais les boutons
6. RESCUE force BUSY→READY après 50 VBlanks → mais déjà trop tard

**Root cause:** La chaîne d'interruption SIO0 ne fonctionne pas à cause du timing:
- Le BIOS VBlank handler poll SIO0 avec IEc=0 (interrupts off)
- Les SIO0 IRQ s'empilent mais ne déclenchent pas d'exception
- Quand RFE rétablit IEc=1, le transfert est fini
- Le BIOS SIO0 handler trouve un transfert terminé → ne délivre pas l'événement
- L'event 0xF0000009 reste BUSY → le jeu ignore le pad buffer

**v22 résultat:** Forced I_MASK bit 7 + event delivery. RÉSULTAT: **BIOS bloqué pendant
StartPAD init!** Le BIOS pad handler à PC=0x4478-0x4624 boucle avec s1=0x1F801040
(SIO0 base), Status=0x40000404 (IEc=0). Le I_MASK bit 7 force confuse le dispatch.

**v23 (actuel, NON COMPILÉ):** Retire I_MASK force + retire I_STAT bit 7 de ACK.
Garde uniquement event delivery + I_STAT clear quand transfert termine.

### 🔬 Analyse DuckStation SIO0 (référence, 2026-02-18)

**Source:** `src/core/pad.cpp` (GitHub stenzek/duckstation)

**Architecture fondamentalement différente de notre code:**

| Aspect | DuckStation | Notre code (R3000-Emu) |
|--------|-------------|----------------------|
| **Timing transfert** | **DELAYED** via event scheduling (~500 ticks) | **IMMÉDIAT** dans sio0_write_data() |
| **ACK timing** | 450 ticks controllers, 170 ticks memory cards | 88 cycles fixe |
| **IRQ bits** | 3 séparés: TXINTEN, RXINTEN, ACKINTEN dans JOY_CTRL | Pas implémentés |
| **State machine** | Idle → Transmitting → WaitingForACK → Idle | Phase 0-4 immédiat |
| **IRQ trigger** | `InterruptController::SetLineState(IRQ::PAD, true)` | Direct i_stat_ bit 7 |
| **Initial transfer** | **Delayed via scheduled event** (critical!) | Immédiat |

**⚠️ BIOS COMPATIBILITY (commentaire DuckStation crucial):**
```
// Performing the transfer immediately will result in both the INTR bit and the
// bit in the interrupt controller being discarded, since the BIOS hasn't yet
// read the CTRL register to check if the IRQ is enabled. Therefore, the test
// in (7) will fail, the BIOS will assume the controller is not connected.
```

**Séquence BIOS attendue (DuckStation):**
1. BIOS configure JOY_CTRL (enable, select pad, set ACKINTEN)
2. BIOS écrit 0x01 dans JOY_DATA → déclenche transfert **DELAYED**
3. BIOS lit JOY_CTRL pour vérifier configuration
4. Délai... transfert s'exécute
5. ACK pulse → JOY_STAT IRQ bit set + I_STAT bit 7 set
6. BIOS clear I_STAT bit 7
7. BIOS vérifie si I_STAT bit 7 a été **re-set** par le prochain ACK
8. Si oui → pad connecté. Si non → pas connecté.

**Notre problème:** On fait le transfert IMMÉDIATEMENT (step 2), donc:
- I_STAT bit 7 est set AVANT que le BIOS n'ait eu le temps de le clear (step 6)
- Le BIOS clear I_STAT bit 7, il ne revient jamais (car transfert déjà fini)
- Le BIOS conclut: **pad non connecté** → n'installe pas le handler → pad ignoré

**Solution nécessaire:** Implémenter des transferts SIO0 **DELAYED** comme DuckStation:
1. `sio0_write_data()` ne doit PAS répondre immédiatement
2. Programmer un "transfer complete" event après ~500 ticks
3. Le response byte arrive APRÈS le délai
4. ACK pulse après 450 ticks supplémentaires
5. I_STAT bit 7 set APRÈS l'ACK (pas pendant le transfert)

### Hacks/Workarounds actifs dans bus.cpp (v23):

| # | Hack | Lignes | But | Impact pad? | Status |
|---|------|--------|-----|-------------|--------|
| 1 | **RESCUE events** | ~1758 | Force events BUSY→READY après 50 VBlanks sans GPU prims | Oui: débloque 0xF0000009 mais trop tard | Actif |
| 2 | **Auto I_MASK** | ~1816 | Force I_MASK=0x0075 si I_MASK reste 0 pendant 40 VBlanks | Non: I_MASK n'est jamais 0 | Actif |
| 3 | **Cycle I_MASK** | ~1851 | Même que #2 mais basé sur cycles | Non | Actif |
| 4 | ~~I_MASK bit 7 force~~ | ~~~250~~ | ~~Force SIO0 IRQ dans I_MASK~~ | ~~Causait blocage BIOS~~ | **RETIRÉ v23** |
| 5 | **Event delivery** | ~301 | Délivre 0xF0000009 quand transfert pad termine | Oui: bidouille | Actif |
| 6 | **I_STAT bit 7 clear** | ~302 | Clear SIO0 IRQ après transfert | Oui: empêche exception parasite | Actif |
| 7 | **Fast CD timing** | cdrom.cpp | Delays CD ÷10 pour UE5 wall-clock | Non | Actif |
| 8 | **shell_close_sent_** | cdrom.cpp | Pas d'INT5 au cold boot | Non | Actif |

**Fichiers modifiés:** `src/r3000/bus.cpp` (v23, NON COMPILÉ), `src/r3000/cpu.cpp` (SIO0 exception trace)

---

## 📌 ÉTAT PRÉCÉDENT (2026-02-15) - SIO0 ROOT CAUSE FIXED (bus v12)

### ✅ ROOT CAUSE FIXED: SIO0 RXRDY/IRQ flag separation (bus v12)

**Bug critique:** `sio0_write_ctrl()` ACK handler (bit 4) effaçait `sio0_rx_ready_` qui
contrôlait BOTH RXRDY (STAT bit 1) AND IRQ flag (STAT bit 9). Sur le vrai PS1,
le bit ACK dans CTRL ne doit effacer QUE le flag IRQ (bit 9), PAS RXRDY (bit 1).

**Séquence de deadlock (avant fix):**
1. VBlank IRQ tire → BIOS exception handler s'exécute (IEc=0)
2. Handler pad (installé par B(0x4B) StartPAD) exécute dans le contexte exception
3. Write SIO0_DATA → `sio0_rx_ready_=1` (RXRDY set)
4. Write JOY_CTRL avec ACK bit → `sio0_rx_ready_=0` (RXRDY cleared! BUG!)
5. Poll RXRDY → toujours 0 → boucle infinie
6. Comme IEc=0 (dans exception), aucun IRQ ne peut interrompre → DEADLOCK

**Fix (bus v12):** Séparation de `sio0_rx_ready_` (STAT bit 1) et `sio0_irq_flag_` (STAT bit 9):
- `sio0_write_data()` set les deux flags
- `sio0_write_ctrl()` ACK (bit 4) ne clear que `sio0_irq_flag_`, PAS `sio0_rx_ready_`
- `sio0_read_data()` ne clear que `sio0_rx_ready_`
- `sio0_stat_value()` utilise chaque flag séparément pour ses bits respectifs

**Diagnostic utilisé:** Exception trace (cpu.cpp) armé à B(0x4B) StartPAD, suivi de
dump d'instructions à la boucle bloquée (PC=0x45C4/0x45D4), décodage assembleur
montrant poll de JOY_STAT bit 1 (RXRDY) depuis $s1=0x1F801040.

**Résultat:** Le jeu progresse maintenant au-delà du logo PlayStation en mode non-HLE !
Confirmé sous UE5.

**Fichiers:** `src/r3000/bus.cpp` (v12), `src/r3000/bus.h` (ajout `sio0_irq_flag_`)

### ✅ FIX: SIO0 CTRL register handling (2026-02-15, bus v11)

**3 bugs critiques dans l'émulation SIO0 (contrôleur PS1):**

| # | Bug | Impact | Fix |
|---|-----|--------|-----|
| 1 | **CTRL Reset (bit 6) ignoré** | Le jeu écrit CTRL=0x40 pour reset SIO avant chaque transaction pad. Sans traitement, `sio0_tx_phase_` reste bloqué dans une phase intermédiaire → protocole désynchronisé → pad jamais lu | `sio0_write_ctrl()`: reset tx_phase=0, rx_ready=0, stat/mode/baud/ctrl=0 |
| 2 | **CTRL Acknowledge (bit 4) ignoré** | Le jeu écrit CTRL bit 4 pour acquitter IRQ/RXRDY. Sans traitement, IRQ SIO0 stale dans STAT → jeu confus | `sio0_write_ctrl()`: clear rx_ready → efface STAT bits 1 et 9 |
| 3 | **STAT IRQ bit mauvaise position** | Code mettait bit 8 (0x0100) au lieu de bit 9 (0x0200). JOY_STAT.IRQ = bit 9 selon nocash specs | Changé 0x0100 → 0x0200 |

**Bonus:** STAT base changé de 0x00C5 → 0x0085 (bit 6 est "unused" dans JOY_STAT, doit être 0)

**Fichiers:** `src/r3000/bus.cpp` (ajout `sio0_write_ctrl()`), `src/r3000/bus.h` (déclaration)

**Détail CTRL write:** Les 2 handlers (byte write 0xA/0xB et word write 0xA) appellent maintenant `sio0_write_ctrl()` au lieu de stocker directement.

### ⚠️ FIX: PollPadInput bailing silencieusement (2026-02-15)

**Symptôme:** Aucun log `PadInput:` dans PSXVR.log. Le polling d'input ne fonctionnait pas du tout.

**Cause:** `PollPadInput()` faisait `if (!PC || !PC->GetPawn()) { return; }` sans aucun log.
Sans GameMode configuré dans World Settings (ou si le Pawn n'est pas spawné), la fonction
retournait silencieusement à chaque tick → aucun input jamais envoyé au PS1.

**Fix:**
1. Retiré le check `!PC->GetPawn()` - `IsInputKeyDown()` fonctionne sans Pawn
2. Ajouté warning log quand PC est null
3. Ajouté log one-shot "polling active" au premier poll réussi (avec nom du Pawn)

**Fichier:** `R3000EmuComponent.cpp` (`PollPadInput()`)

**Pour tester:** Après Hot Reload, chercher dans PSXVR.log:
- `PadInput: polling active` → le polling fonctionne
- `PadInput: No PlayerController found` → pas de PlayerController
- `PadInput: buttons=0x...` → boutons détectés

### 🔴 ROOT CAUSE TROUVÉE: COP0.Status IEc=0 (interrupts désactivés)

**Symptôme:** Jeu bloqué à `pc=0x000045C4` après chargement EXE principal depuis CD.

**Diagnostic CLI avec PC samples (--pc-sample=5000000):**
```
step=255M PC=0xBFC0D864 status=0x40000401 (IEc=1) i_mask=0x0C  ← BIOS, IRQs ON
step=260M PC=0x80040014 status=0x40000400 (IEc=0) i_mask=0x0C  ← Game entry, IRQs OFF
step=265M PC=0x000045C4 status=0x40000404 (IEc=0,IEp=1) i_mask=0x0D  ← STUCK!
step=270M PC=0x000045C4 status=0x40000404 (IEc=0,IEp=1) i_mask=0x0D  ← STUCK!
```

**Analyse COP0.Status = 0x40000404:**
- bit 0 (IEc) = **0** → interrupts GLOBALEMENT désactivées
- bit 2 (IEp) = **1** → ancienne IEc=1 sauvé par exception
- bit 10 (IM2) = **1** → hardware IRQ unmasked
- **EPC = 0xBFC09190** (BIOS ROM)

**Séquence reconstituée:**
1. BIOS charge EXE depuis CD → saute à 0x80040014 avec IEc=0
2. Game startup init → enable VBlank dans I_MASK (0x0C→0x0D)
3. Game active IEc=1 (MTC0 Status)
4. VBlank IRQ immédiatement tire (était pending dans i_stat)
5. Exception prise → IEc=1→IEp, IEc=0 → status=0x40000404
6. BIOS exception handler à 0x80000080 s'exécute...
7. **MAIS: le CPU finit à 0x000045C4 avec IEc=0 (jamais de RFE!)**

**Instruction à 0x000045C4:** `LHU $t4, 4($s1)` = poll d'un champ 16-bit en mémoire
→ C'est une boucle WaitEvent/TestEvent du kernel BIOS qui poll le status d'un événement.

**Problème:** Le BIOS handler a traité l'exception et sauté au jeu (via HookEntryInt/
SetCustomExitFromException?) AVEC IEc=0. Le jeu devrait appeler B(0x17) ReturnFromException
pour faire RFE. Mais le code à 0x000045C4 est une boucle poll qui attend un événement
qui ne peut jamais être délivré (car IEc=0 → pas d'IRQ → pas d'event delivery).

**Table d'événements au moment du blocage:**
```
Event table ptr=0xA000E028
Tous les events: cls=0xF0000003 (CDROM) status=0x0000 (FREED)
AUCUN event VBlank (0xF0000001) dans la table!
```
→ Le jeu n'a pas encore créé ses events VBlank - il est bloqué AVANT dans son init.

**CLI ET UE5 ont le même bug** - ce n'est PAS un problème de timing wall-clock.

**Piste: PADInit() bloquant?** L'utilisateur suspecte que le jeu bloque dans PADInit().
Le BIOS PADInit initialise SIO0 et peut configurer un callback VBlank pour le polling pad.
Si ce callback attend un event qui nécessite IRQs... deadlock.

**À investiguer:**
1. Quand exactement IEc passe de 0→1→0 (entre step 260M et 265M)
2. Le BIOS exception handler fait-il bien RFE?
3. Y a-t-il un HookEntryInt qui saute au jeu avec IEc=0?
4. Le code BIOS à 0x000045C4 est-il WaitEvent, TestEvent, ou StartPAD?

**Note:** Le rescue code v8 (BUSY→READY) ne marche pas car les events sont status=0x0000
pas 0x2000. Le jeu n'a même pas encore créé ses events.

---

### ✅ FIX: Flickering "une frame sur 2" (2026-02-15)

**Symptôme:** Ridge Racer affiche une frame sur deux, l'image clignote.

**Analyse logs:** Pattern "2 frames avec commandes, 1 frame vide" dans system.log.
Ridge Racer dessine à ~33fps (2 frames par 3 VBlanks).

**Cause:** `R3000GpuComponent::RebuildMesh()` appelait `ClearAllMeshSections()` quand
`NumCmds == 0`, effaçant le mesh visible sur les frames vides.

**Fix:** Simplement `return` sur les frames vides, garder le mesh précédent visible.

**Fichier:** `R3000GpuComponent.cpp` ligne ~309-313

### ✅ FIX: GPUSTAT bit 31 (even/odd field) (2026-02-15)

**Avant (bug):** bit 31 = `in_vblank_` (pulse pendant VBlank)
**Après (fix):** bit 31 = `even_odd_field_` qui toggle chaque VBlank (comme le vrai hardware)

**Fichier:** `src/gpu/gpu.h` + `src/gpu/gpu.cpp`

### ✅ FIX: Timing VBlank PAL/NTSC dynamique (2026-02-15)

**Avant:** Hardcodé PAL (680688 cycles)
**Après:** Dynamique basé sur `display_.is_pal` (PAL=680688, NTSC=571088)

### ✅ AJOUT: PS1 Controller Input via Xbox Gamepad (2026-02-15)

**3 couches implémentées:**

1. **Bus** (`src/r3000/bus.h/cpp`):
   - `std::atomic<uint16_t> pad_buttons_{0xFFFF}` (active-low, 0=pressed)
   - `sio0_write_data()` phases 3/4 lisent `pad_buttons_` au lieu de 0xFF

2. **Core** (`src/emu/core.h/cpp`):
   - `set_pad_buttons(uint16_t)` forwarde vers `bus_->set_pad_buttons()`

3. **UE5** (`R3000EmuComponent.cpp`):
   - `PollPadInput()` appelé dans TickComponent
   - Utilise `APlayerController::IsInputKeyDown(FKey)` directement (pas Enhanced Input)
   - Mapping Xbox hardcodé: A=Cross, B=Circle, X=Square, Y=Triangle, etc.
   - Enhanced Input IMC aussi ajouté pour flexibilité future

**PS1 Button Bit Layout (active-low):**
```
Byte low  (bits 0-7):  Select L3 R3 Start Up Right Down Left
Byte high (bits 8-15): L2 R2 L1 R1 Triangle Circle Cross Square
```

**Assets créés par script Python** (`scripts/ue5_create_psx_inputs.py`):
- 16 `IA_Pad*` InputActions (bool/Digital) dans `/Game/PSX/Input/`
- 1 `IMC_PSXPad` InputMappingContext avec mappings Xbox gamepad
- Le constructeur C++ charge ces assets par défaut

**Debug:** `PadInput: buttons=0x%04X` dans les logs quand un bouton est pressé.

**Flux complet (UE5 → émulateur):**
1. **TickComponent** (game thread) appelle **PollPadInput()** à chaque frame.
2. **PollPadInput()** récupère le `PlayerController` (world puis `GetFirstLocalPlayerController` en fallback), puis appelle `IsInputKeyDown(EKeys::Gamepad_*)` pour construire un masque 16 bits (actif bas).
3. **Core_->set_pad_buttons(Buttons)** écrit dans **Bus::pad_buttons_** (atomic).
4. Le **worker thread** (ou le mode legacy) exécute **Core->step()**; quand le jeu/BIOS lit JOY_DATA (0x1F801040), **Bus** répond via **sio0_write_data()** phases 3/4 en lisant **pad_buttons_**.
5. Si aucun PlayerController: on envoie 0xFFFF (aucun bouton) pour éviter des touches "bloquées".

**Si les manettes ne marchent pas sous UE5:** vérifier qu’un Game Mode est défini (World Settings → Game Mode Override) pour qu’un PlayerController existe. Log à chercher: `PadInput: No PlayerController found` ou `PadInput: polling active`.

### ⚠️ Bug Enhanced Input: GetPlayerInput() retourne null

Enhanced Input ne fonctionnait pas pour le polling (`GetPlayerInput()` null).
Cause probable: Default Player Input Class pas configuré sur `EnhancedPlayerInput`
dans Project Settings > Input > Default Classes.

**Workaround:** Utilisation directe de `IsInputKeyDown(EKeys::Gamepad_*)` au lieu
d'Enhanced Input `GetActionValue()`. Marche avec n'importe quel système d'input.

### ⚠️ Note UE5.7: Noms FKey thumbstick

- `EKeys::Gamepad_LeftThumbstick` (PAS `Gamepad_LeftThumbstickButton`)
- `EKeys::Gamepad_RightThumbstick` (PAS `Gamepad_RightThumbstickButton`)

### ✅ FIX: GTE v6→v8 (2026-02-14/15) - Lighting, projection, flags

**v6 (DuckStation comparison):** 7 bugs trouvés (voir section v5 ci-dessous)

**v7:** Réécriture complète lighting (NCS/CC/NCDS/etc.)
- `push_color()` helper avec FLAG_COLOR saturation bits
- `set_mac_shifted()` pour tous les cmds lighting
- Toutes les commandes lighting réécrites: NCS, CC, NCDS, NCCS, DPCS, INTPL, NCT, NCDT, NCCT, DCPL, DPCT

**v8:** Projection + flags fixes
| # | Bug | Impact | Fix |
|---|-----|--------|-----|
| 1 | **MAC0 overflow** non vérifié dans RTPS screen projection (Sx/Sy) | FLAG bit 31 jamais set, jeux qui rejettent les vertices overflowed ne filtrent pas | Ajout check MAC0 overflow pour Sx et Sy |
| 2 | **IR3 second write** utilisait mauvais flag | FLAG_IR3_SAT pas set correctement | Utilise `set_ir()` avec FLAG_IR3_SAT |
| 3 | **IR3/push_sxy ordre** ne matchait pas DuckStation | Séquence RTPS légèrement différente | Réordonné pour matcher DuckStation |

### ✅ FIX: DMA2 GPU→RAM (2026-02-14)

**Bug:** Les transferts DMA2 direction GPU→RAM (dir=0, GPUREAD) étaient **silencieusement ignorés**.
1040 transferts par frame jamais exécutés.

**Impact:** Pipeline de textures VRAM cassé. Le jeu lit des textures/palettes depuis le GPU
via DMA2 GPUREAD et les copies échouaient silencieusement.

**Fix:** Implémenté les transferts GPU→RAM dans `bus.cpp` via `gpu.read_data()` (GPUREAD port).

### ✅ FIX: DIV/DIVU edge cases CPU (2026-02-14)

| Case | Avant (bug) | Après (fix, match hardware) |
|------|-------------|----------------------------|
| DIV by zero | Undefined | LO = num>=0 ? -1 : +1, HI = num |
| DIVU by zero | Undefined | LO = 0xFFFFFFFF, HI = num |
| DIV overflow (INT32_MIN/-1) | Undefined | LO = INT32_MIN, HI = 0 |

### 🔧 Versions mises à jour

| Fichier | Version |
|---------|---------|
| `src/gpu/gpu.cpp` | v7 (even/odd field, PAL/NTSC timing) |
| `src/gpu/gpu.h` | `even_odd_field_`, constantes PAL/NTSC |
| `src/gte/gte.cpp` | **v9** (lighting diagnostics, sRGB fix) |
| `src/r3000/cpu.cpp` | DIV/DIVU edge cases fixed |
| `src/r3000/bus.cpp` | **v24** (SIO0 delayed transfers, DuckStation-style) |

---

## 📌 ÉTAT PRÉCÉDENT (2026-02-14) - VERSION v5: RÉÉCRITURE COMPLÈTE GTE

**À TESTER:** Relancer UE5 et vérifier si le demo mode fonctionne.

### 🔧 Changements v5 (comparaison avec DuckStation)

7 bugs trouvés et corrigés par comparaison directe avec DuckStation:

| # | Bug | Impact | Fix |
|---|-----|--------|-----|
| 1 | **push_sxy** mettait la nouvelle valeur dans SXYP au lieu de SXY2 | Vertices décalés d'un cran (RTPS cassé) | SXY2=new, SXYP=new |
| 2 | **MVMVA** hardcodé R*V0+TR | Mauvais résultats si mx/vv/tv != 0 | Support 4×4×4 combinaisons |
| 3 | **set_mac** ne faisait pas le shift | MAC values fausses pour GPL etc. | MAC = value >> shift |
| 4 | **SZ** dépendait de sf | sf=0 → SZ non-divisé (trop grand) | Toujours z >> 12 |
| 5 | **DQA/DQB** manquant | IR0 jamais mis à jour | Ajouté dans RTPS |
| 6 | **UNR table** valeurs incorrectes | Division approximation fausse | Table exacte DuckStation |
| 7 | **RTPT** stockage direct au lieu de shift register | SZ0 jamais mis à jour | RTPT appelle rtps_internal 3× |

### 🔴 PROBLÈME ORIGINAL: Polygons explosés + Menu debug disparu

**Status v4**: Menu debug OK, polygons explosés en mode démo 3D
**Status v5**: À tester

### 🔍 ANALYSE DES LOGS (v3)

Les logs UE5 montrent le problème clairement:
```
[GTE] RTPT SZ=0: V1 in=(-120,40,36) mac3=-364996 mac3_shifted=-90 trz=-4 r3x=(5676,4872,3823) sf=12
```

**Calcul vérifié:**
```
mac3 = r31*vx + r32*vy + r33*vz + (trz << 12)
     = 5676*(-120) + 4872*40 + 3823*36 + (-4*4096)
     = -681120 + 194880 + 137628 - 16384
     = -364996 ✓
```

**Le calcul GTE est CORRECT** mais le résultat mac3 est **négatif** → sz=0 → division overflow.

### 🎯 CAUSE RACINE

Les vertices sont transformés avec une matrice de rotation qui produit des **Z négatifs** (vertices derrière la caméra):
- `trz=-4` (translation Z très petite)
- `r31=5676` (composante X→Z importante)
- Pour vx=-120: la contribution `r31*vx = -681120` domine et rend mac3 négatif

**C'est le comportement attendu du PS1** pour des vertices derrière la caméra!

### 📋 VERSIONS TESTÉES

| Version | Division | SZ depuis | Résultat |
|---------|----------|-----------|----------|
| v1 | UNR (buggy) | IR3 | Cassé |
| v2 | UNR (fixé) | IR3 | Cassé |
| v3 | UNR (fixé) | MAC3 | Cassé + debug logs |
| v4 | Simple | MAC3 | Menu OK, demo cassé |
| v5 | UNR (DuckStation exact) | z >> 12 | **À TESTER** |

**Version actuelle: v5 (UNR_div, full_MVMVA, fixed_push_sxy)**
- Division UNR hardware-accurate (table + Newton-Raphson, identique DuckStation)
- SZ toujours depuis z >> 12 (pas MAC3 >> sf)
- MVMVA supporte toutes les combinaisons matrice/vecteur/translation
- push_sxy corrigé (SXY2 = nouvelle valeur, pas ancien SXYP)
- RTPT utilise rtps_internal() 3× (shift register correct)
- DQA/DQB depth cueing ajouté

### ⚠️ FIX APPLIQUÉ: SZ depuis MAC3 (pas IR3)

**Avant (bug):**
```cpp
const int32_t ir3 = clamp_s16((int32_t)(mac3 >> shift));
const uint32_t sz = (uint32_t)clamp_u16(ir3);  // FAUX!
```

**Après (fix):**
```cpp
const int32_t mac3_shifted = (int32_t)(mac3 >> shift);
const uint32_t sz = (mac3_shifted < 0) ? 0 :
    ((mac3_shifted > 0xFFFF) ? 0xFFFF : (uint32_t)mac3_shifted);
```

psx-spx documente:
- `IR3 = Lm_B3(MAC3 >> sf)` → clamp signé [-0x8000, +0x7FFF]
- `SZ3 = Lm_D(MAC3 >> sf)` → clamp **unsigned** [0, 0xFFFF]

### 🔧 FICHIERS MODIFIÉS

- `src/gte/gte.cpp`:
  - Table UNR 257 entrées (conservée)
  - `gte_divide()` - division simple (v4) ou UNR
  - RTPS/RTPT - SZ calculé depuis MAC3
  - Debug logs pour SZ=0

### ✅ RÉSOLU: Menu debug

Le menu debug fonctionne maintenant avec le fix SZ depuis MAC3.

### 🔴 TOUJOURS CASSÉ: Polygons explosés en demo mode

**Cause confirmée:** Les vertices ont des Z négatifs après transformation.

Exemple de log:
```
RTPT SZ=0: V1 in=(-120,40,36) mac3=-364996 mac3_shifted=-90 trz=-4 r3x=(5676,4872,3823) sf=12
```

**Calcul vérifié:**
```
mac3 = 5676*(-120) + 4872*40 + 3823*36 + (-4 << 12)
     = -681120 + 194880 + 137628 - 16384
     = -364996 ✓
```

**Le problème:** `trz=-4` donne une contribution `-16384` qui tire TOUS les Z vers le négatif.

**C'est le comportement correct du GTE PS1!** Quand un vertex est derrière la caméra:
1. mac3 devient négatif
2. sz = clamp_unsigned(mac3 >> sf) = 0
3. Division retourne 0x1FFFF (max)
4. Coordonnées écran = énormes → clampées à ±1024
5. Polygons "explosés"

### ❓ QUESTION: Est-ce que DuckStation a le même problème?

Si DuckStation affiche correctement le demo mode, il pourrait avoir:
1. Triangle clipping software (frustum culling)
2. Gestion spéciale des sz=0
3. Autre différence d'émulation

**À TESTER:** Lancer Ridge Racer demo mode dans DuckStation et comparer.

---

## 📌 ÉTAT PRÉCÉDENT (2026-02-10) - VERSION v8: FORCE ALL EVENTS READY

### ✅ FIX v8 APPLIQUÉ: RESCUE MODE - FORCE EVENTS READY (BIDOUILLE)

**Status**: Le jeu Ridge Racer fonctionne maintenant en mode non-HLE !

**Fichier modifié**: `src/r3000/bus.cpp`

**Le problème résolu**:
- En mode non-HLE, le BIOS exception handler à 0x80000080 ne délivrait pas correctement les événements VSync
- Le jeu restait bloqué dans la boucle VSync (PC=0x00001Exx) après le logo PlayStation
- Cause: notre émulation hardware (I_STAT/I_MASK/exception dispatch) n'est pas assez précise

**La solution (BIDOUILLE)**:
```cpp
// Après 50 VBlanks sans primitives GPU:
// Scan event table et force TOUS les événements BUSY → READY
if (vblank_stuck_count_ >= 50)
{
    // Parcourir la table d'événements kernel
    // Tout événement avec status=0x2000 (BUSY) → status=0x4000 (READY)
}
```

**Pourquoi c'est une bidouille**:
1. Ne corrige pas la cause racine (BIOS handler qui ne marche pas)
2. Force les événements prêts sans savoir lequel le jeu attend vraiment
3. Peut causer des effets de bord (événements délivrés trop tôt/tard)

**Un fix propre nécessiterait**:
1. Comprendre pourquoi le BIOS exception handler échoue
2. Corriger l'émulation des chaînes SysEnqIntRP
3. Ou implémenter un dispatch d'événements VBlank correct côté hardware

**Résultat**:
- ✅ Sony logo: OK
- ✅ PlayStation logo: OK (2 tri, 2-3 quad)
- ✅ "Press Start": OK (8 rect)
- ✅ Galaga mini-jeu: OK (24-62 quads, graphiques)
- ⚠️ Son Galaga: À investiguer (volume/timing?)
- ✅ Le jeu progresse jusqu'à frame #1500+

---

### ⚠️ PROBLÈME OUVERT: Son Galaga manquant

**INVESTIGATION (2026-02-10):**

**1. Le SPU GÉNÈRE de l'audio pendant Galaga:**
```
[6.914s] KEY_ON voice 0 addr=0x5AB70         ← Galaga sound effect start
samples=327680 cb_calls=455 ... out=354/390  ← Non-zero output!
samples=393216 cb_calls=544 ... out=941/1035 ← Audio IS playing
[10.451s] KEY_ON voice 0 addr=0x77190        ← Another sound
samples=524288 cb_calls=722 ... out=-717/-717 ← Still generating
```

**2. Corrélation GPU/SPU confirmée:**
- GPU Frame 340-370: Galaga rectangles (17→175 rects)
- SPU KEY_ON at 6.914s ≈ Frame 345 (340/50Hz = 6.8s)
- **Le son et les graphiques sont synchronisés!**

**3. Audio pipeline vérifié:**
- `UE audio connected: gain=4.000 muted=0` - Connecté, pas muté
- `cb=yes` dans les logs SPU - Callback IS configured
- `cb_calls=455...1882` - Callback IS being called (count increasing)
- `MAIN_VOL=0x3FFF` (16383) - Volume at full after 0.527s

**4. Timeline audio:**
| Temps | Événement |
|-------|-----------|
| 0.231s | SPU init, muted=1, all voices KEY_ON @0x01000 |
| 0.232s | muted=0 (unmuted) |
| 0.527s | MAIN_VOL=0x3FFF (full volume), CD audio bit enabled |
| 0.719s | KEY_ON voices 0-3 @0x06140 (PlayStation logo jingle) |
| 1.4-5s | PlayStation logo sounds (voices 4-23) |
| 6.251s | Full SPU reinit (KEY_OFF all, KEY_ON all, KEY_OFF all) |
| 6.252s | SPU enabled, MAIN_VOL=0x3FFF |
| 6.914s | **KEY_ON voice 0 @0x5AB70** ← Galaga starts! |
| 10.451s | KEY_ON voice 0 @0x77190 |
| 16.601s | CD audio enabled (cd=0→1) |
| 16.615s | KEY_OFF voice 0 |

**5. CD Audio vs SPU:**
- Galaga uses SPU sound effects (voices 0-23)
- CD audio (music) doesn't start until 16.6s
- The game might expect CD-DA music during Galaga?

**HYPOTHÈSES RESTANTES:**

1. **Audio buffer underrun** - UE5 demande des samples plus vite que le SPU les génère
   - Ring buffer se vide → silence → "out=0/0" dans les logs
   - Solution: augmenter le buffer ou throttle UE5 audio requests

2. **Le user n'entend pas mais le son JOUE** - Problème UE5/Windows audio
   - Vérifier que USynthComponent::Start() est appelé
   - Vérifier les stats: TotalPushedSamples vs TotalGeneratedSamples

3. **Galaga sound effects trop courts** - Les KEY_ON sont brefs
   - Seulement voice 0 active pendant Galaga (autres voices = silence?)
   - Les samples à 0x5AB70 et 0x77190 sont-ils des vrais sons?

**À TESTER:**

1. **Test CLI audio** (confirmer que Galaga a du son):
   ```bash
   ./build/Debug/r3000_emu.exe --bios=SCPH-7502.bin --cd="Ridge Racer (U).cue" \
       --wav-output=galaga_audio.wav --max-steps=30000000
   # Ouvrir galaga_audio.wav dans Audacity → voir si son Galaga est présent
   ```

2. **Ajouter logs pour stats audio UE5:**
   ```cpp
   // Dans TickComponent, log périodique:
   UE_LOG(LogR3000Emu, Log, TEXT("Audio: pushed=%llu gen=%llu dropped=%llu silence=%llu"),
       AudioComp_->GetTotalPushed(), AudioComp_->GetTotalGenerated(),
       AudioComp_->GetTotalDropped(), AudioComp_->GetTotalSilence());
   ```

3. **Vérifier USynthComponent::Start()** - S'assurer que l'audio joue vraiment

4. **Comparer buffer timing** - Si pushed << generated, c'est un underrun

---

### ✅ FIX SPU APPLIQUÉ (v9): force_off() quand SPU désactivé

**Problème identifié (comparaison DuckStation):**
Quand le jeu fait un SPU init, il écrit SPUCNT=0 (disable). DuckStation force alors
TOUTES les voix off immédiatement. Notre code NE FAISAIT PAS ça.

**DuckStation:**
```cpp
if (!new_value.enable && s_state.SPUCNT.enable)
{
    for (u32 i = 0; i < NUM_VOICES; i++)
        s_state.voices[i].ForceOff();
}
```

**Fixes appliqués:**

1. **`SpuVoice::force_off()`** (spu_voice.cpp/h):
   ```cpp
   void SpuVoice::force_off()
   {
       env_phase_ = ENV_OFF;
       env_level_ = 0;
   }
   ```
   Contrairement à `key_off()` qui démarre la phase RELEASE (fade out),
   `force_off()` arrête immédiatement la voix.

2. **Détection transition enable dans SPUCNT** (spu.cpp):
   ```cpp
   const bool old_enable = (old >> 15) & 1;
   const bool new_enable = (val >> 15) & 1;
   if (old_enable && !new_enable)
   {
       for (int i = 0; i < kNumVoices; i++)
           voices_[i].force_off();
   }
   ```

**Résultat attendu:**
- Quand le jeu réinitialise le SPU (SPUCNT 0xC000→0x0000→0xC000),
  les anciennes voix sont correctement arrêtées avant le nouveau init.
- Le son Galaga devrait maintenant jouer correctement.

---

## 🔊 ARCHITECTURE AUDIO UE5

### Pipeline Audio:
```
[PS1 SPU] → [R3000AudioComponent Ring Buffer] → [USynthComponent OnGenerateAudio] → [UE5 Audio]
    ↑                    ↑                                ↑
   44.1kHz         Lock-free int16[65536]            Float conversion
   Stereo          Push/Pull ring                    + gain * 4.0
```

### Fichiers:
- `src/audio/spu.cpp` - PS1 SPU emulation, calls `audio_callback_` with samples
- `R3000AudioComponent.cpp` - Ring buffer between SPU and UE5
- `R3000EmuComponent.cpp:655` - Sets up callback: `Spu->set_audio_callback([Audio](...))`

### Statistiques audio (pour debug):
```cpp
// R3000AudioComponent.h
TotalPushedSamples_      // Samples reçus du SPU
TotalGeneratedSamples_   // Samples demandés par UE5
TotalDroppedSamples_     // Samples perdus (overrun)
TotalSilenceSamples_     // Samples silence (underrun)
```

### Vérifications:
1. `UE audio connected: gain=X.XXX muted=N` - Dans system.log au boot
2. `cb=yes` dans spu.log - Callback configuré
3. `cb_calls=N` croissant - Callback appelé
4. `out=L/R` non-zero - Audio généré

---

### ✅ FIX #6 APPLIQUÉ (v6): DÉTECTION DE BLOCAGE VSYNC

**Fichiers modifiés**:
- `src/r3000/bus.cpp` - Ajout détection de blocage VSync
- `src/r3000/bus.h` - Nouvelles variables de tracking
- `src/gpu/gpu.h` - Getters pour frame_count() et last_frame_stats()

**Fonctionnalité ajoutée**:
Détection automatique quand le jeu est bloqué dans VSync (100+ VBlanks sans primitives).

Quand un blocage est détecté, dump complet de l'état:
- I_STAT / I_MASK / pending IRQs
- CPU PC au moment du blocage
- Table d'événements kernel (adresse, taille)
- Chaînes SysEnqIntRP (VBlank[0], GPU[1], CDROM[2], DMA[3])
- PCB / TCB pointers
- Scan des événements VSync avec leur status (READY/BUSY/ALLOCATED)

**Exemple de sortie**:
```
[BUS] ===== VSYNC STUCK DETECTED =====
[BUS] VBlank #427: stuck for 100 VBlanks (no primitives)
[BUS] Last real frame: VBlank #327
[BUS] I_STAT=0x0000 I_MASK=0x000D pending=0x0000
[BUS] CPU PC=0x00001ED0
[BUS] Event table ptr=0x801C4000
[BUS] SysEnqIntRP chains: [0]=0x801C0010 [1]=0x00000000 [2]=0x801B2040 [3]=0x00000000
[BUS] PCB=0x801FFFF0 TCB=0x801C2000
[BUS]   Event[4]: cls=0xF2000003 spec=0x0002 status=0x2000 (BUSY)
[BUS] ===== END STUCK DUMP =====
```

**Interprétation**:
- `status=0x2000 (BUSY)` = L'événement VSync n'est PAS marqué ready
- Le BIOS exception handler devrait appeler DeliverEvent pour le marquer ready
- Si l'événement reste BUSY, le jeu reste bloqué dans WaitEvent/VSync

---

### 🔴 ROOT CAUSE IDENTIFIÉE: Timing CD en mode wall-clock

**ANALYSE COMPLÈTE DES LOGS (2026-02-10):**

Le problème n'est PAS un bug d'IRQ. C'est un problème de TIMING:

1. **Le jeu désactive VBlank intentionnellement** pendant le chargement CD
2. **En CLI (vitesse max)**: Le chargement est quasi-instantané, VBlank réactivé vite
3. **En UE5 (wall-clock à 33.8MHz)**: Le chargement prend du temps RÉEL

**Timeline du problème:**
```
Frame #313: i_mask=0x7D (VBlank ON), rendu normal
            → Le jeu lance un chargement CD
            → Le jeu écrit i_mask=0x0C (VBlank OFF, seulement CD+DMA)
Frame #320+: i_mask=0x0C, VBlank désactivé, chargement en cours
            → Le jeu est dans la boucle BIOS de chargement CD
            → Pattern: ReadN → Pause → SetLoc → SeekL → ReadN... (répété)
Frame #437+: clip=(0,0)-(0,0), DMA2 nodes=704 words=0
            → Le jeu est TOUJOURS en train de charger
            → VBlank n'est jamais réactivé car le chargement n'est pas fini
```

**Délais CD qui causent le problème:**
- `kSpinUpDelay = 20,321,280 cycles` (~600ms) - quand moteur idle
- `kMinSeekTicks = 400,000 cycles` (~12ms) - seek minimum
- `kMaxSeekTicks = 2,000,000 cycles` (~60ms) - seek maximum
- Lecture secteur: 110,000-220,000 cycles (~3-7ms)

**Calcul:** Le jeu charge secteur par secteur avec Pause entre chaque:
- Chaque cycle: Seek (~20ms) + Read (~6ms) + CPU processing
- Pour ~100 secteurs: ~2.6 secondes RÉELLES
- Pendant ce temps, VBlank est désactivé!

**PREUVE dans cdrom.log:**
```
[6106 ms] ReadN/S START: LBA=4 motor_spinning=0   ← Moteur arrêté!
[7065 ms] Async IRQ1 delivered                     ← 959ms de délai!
...
[16487 ms] CMD 0x09 (Pause)                        ← Fin du chargement
```
→ Le chargement prend **10+ secondes** de temps réel!

**POURQUOI CLI fonctionne:**
- En CLI, les cycles passent instantanément
- 20M cycles de spin-up = quelques ms réelles
- Le chargement finit très vite, VBlank réactivé

### 🎯 SOLUTION PROPOSÉE: Délais CD rapides pour UE5

Option 1: Réduire les délais en mode wall-clock
Option 2: Forcer VBlank à rester enabled (hack)
Option 3: Mode "turbo CD" configurable

---

### ✅ FIX #4 CONFIRMÉ: VBlank est désactivé INTENTIONNELLEMENT

Le jeu désactive VBlank dans I_MASK pendant le chargement CD.
Ce n'est PAS un bug de l'émulateur - c'est le comportement normal.

**Le vrai problème**: Les délais CD réalistes sont trop longs en wall-clock.

### ✅ FIX #5 APPLIQUÉ (v5): FAST CD TIMING

**Fichier modifié**: `src/cdrom/cdrom.cpp`

**Changements**:
Tous les délais CD divisés par 10x:

| Délai | Original | Fast (v5) |
|-------|----------|-----------|
| Spin-up | 20,321,280 (~600ms) | 2,032,128 (~60ms) |
| Seek min | 400,000 (~12ms) | 40,000 (~1.2ms) |
| Seek max | 2,000,000 (~60ms) | 200,000 (~6ms) |
| Seek factor | 135,000/log2 | 13,500/log2 |
| Rotation | 110,000/220,000 | 11,000/22,000 |
| Sector read | 110,000/220,000 | 11,000/22,000 |

**Résultat attendu**:
- Le chargement CD prend ~1 seconde au lieu de ~10+ secondes
- VBlank réactivé avant timeout
- Le jeu progresse normalement après le logo

---

### ✅ FIX #4 APPLIQUÉ: Logging I_MASK VBlank disable

Ajout de logs WARNING quand VBlank (bit 0) est retiré de I_MASK:
```cpp
if ((old_mask & 0x01) && !(i_mask_ & 0x01))
{
    emu::logf(emu::LogLevel::warn, "IRQ",
        "!!! VBlank DISABLED in I_MASK (0x%04X -> 0x%04X) !!!", ...);
}
```

**Fichier modifié**: `src/r3000/bus.cpp` (byte write + word write)

Après rebuild, chercher dans les logs: `VBlank DISABLED`

---

### ✅ FIX #3 APPLIQUÉ: shell_close_sent_ spurious INT5

**Problème CONFIRMÉ**: Le shell close INT5 était envoyé au boot même si le shell n'a jamais été ouvert.

**Symptôme dans les logs**:
```
CD set_irq(3): irq_en=0x18 ... last_cmd=0x1E line=0->0  ← ReadTOC IGNORÉ
CD set_irq(2): irq_en=0x18 ... last_cmd=0x1E line=0->0  ← ReadTOC IGNORÉ
CD set_irq(3): irq_en=0x18 ... last_cmd=0x1A line=0->0  ← GetID IGNORÉ
CD set_irq(2): irq_en=0x18 ... last_cmd=0x1A line=0->0  ← GetID IGNORÉ
[GPU] GP1 RESET                                         ← Jeu reset GPU!
DMA2 LL: nodes=704 words=0                              ← Pas de primitives
```

**Cause**:
- `shell_close_sent_=0` → INT5 (shell close) envoyé au premier GetStat
- Le BIOS reçoit INT5 et entre dans une boucle de vérification shell
- Il met `irq_en=0x18` (seulement INT4/INT5 enabled)
- ReadTOC/GetID envoient INT2/INT3 qui sont **ignorés** (irq_en masque INT1-INT3)
- Le jeu pense qu'il y a une erreur → GP1 RESET → clip=(0,0)-(0,0)

**PSX-SPX spécification**:
- INT5 shell close = "the shell was opened and is now closed"
- Au cold boot avec disc déjà présent, le shell n'a JAMAIS été ouvert
- Donc shell close INT5 ne devrait PAS être envoyé

**Solution appliquée**:
```cpp
// AVANT (BUG):
shell_close_sent_ = 0;  // → INT5 envoyé au boot

// APRÈS (FIX):
shell_close_sent_ = 1;  // → Pas d'INT5 au boot
```

**Fichier modifié**: `src/cdrom/cdrom.cpp` ligne 649

### ✅ FIX #2 APPLIQUÉ: irq_line() formule incorrecte

**Problème CONFIRMÉ**: La fonction `irq_line()` utilisait une formule incorrecte:
```cpp
// AVANT (FAUX):
return ((irq_flags_ & irq_enable_ & 0x1Fu) != 0) ? 1 : 0;
```

**Pourquoi c'est faux** (PSX-SPX specification):
- `irq_flags_` bits 0-2 contient une **VALEUR** 1-7 pour INT1-INT7 (pas un bitmask!)
- `irq_enable_` bits 0-4 sont des **BITS INDIVIDUELS** (bit 0=INT1, bit 1=INT2, etc.)

**Exemple concret du bug**:
- INT3 pending: `irq_flags_` = 0x03 (valeur 3)
- INT3 enabled: `irq_enable_` = 0x04 (bit 2)
- Code faux: `0x03 & 0x04 = 0x00` → ligne=0 → **IRQ JAMAIS DÉLIVRÉ!**
- Code correct: type=3, enable_bit = 1<<(3-1) = 0x04, `0x04 & 0x04 = 0x04` → ligne=1 ✅

**Solution appliquée**:
```cpp
int Cdrom::irq_line() const
{
    const uint8_t irq_type = irq_flags_ & 0x07u;  // IRQ type 1-7
    if (irq_type == 0) return 0;                  // No IRQ pending
    if (irq_type > 5) return 0;                   // INT6/INT7 undefined
    const uint8_t enable_bit = (uint8_t)(1u << (irq_type - 1));
    return (irq_enable_ & enable_bit) ? 1 : 0;
}
```

**Fichier modifié**: `src/cdrom/cdrom.cpp` ligne 909-923

### ✅ TEST CLI RÉUSSI (2026-02-10)

Avec le fix irq_line():
```
CD set_irq(3): ... line=0->1  ← INT3 déclenche maintenant!
CD set_irq(5): ... line=0->1  ← INT5 aussi!
DMA2 LL: nodes=1032 words=37  ← Jeu progresse!
```

### ✅ FIX #1 APPLIQUÉ: MINIMUM_INTERRUPT_DELAY (DuckStation)

**Problème identifié**: Les IRQs CDROM pouvaient être délivrés trop rapidement après acquittement,
causant des séquences IRQ qui confondaient le BIOS/jeu.

**Solution implémentée**:
- Ajout de `cycles_since_irq_ack_` dans cdrom.h
- Constante `kMinInterruptDelay = 1000` cycles (comme DuckStation)
- Après ACK d'un IRQ, le prochain ne peut être délivré qu'après 1000 cycles
- Implémenté dans `Cdrom::tick()` et lors de l'écriture IRQ_ACK

**Fichiers modifiés**:
- `src/cdrom/cdrom.h`: Ajout compteur `cycles_since_irq_ack_` et constante
- `src/cdrom/cdrom.cpp`: Incrément dans tick(), reset lors de ACK, vérification avant délivrance

### ✅ TEST CLI RÉUSSI (2026-02-10)

**Test Ridge Racer avec SCPH1001.BIN**:
```
DMA2 LL transitions observées:
- words=5 : PlayStation logo phase (OT presque vide)
- words=25 : Game starts rendering (primitives)
- words=37 : Full game rendering (more primitives)
```

**Progression confirmée**:
- VBlank #51 atteint (~1 sec)
- VBlank #101 atteint (~2 sec)
- VBlank #151 atteint (~3 sec)
- Jeu passe de logo → game rendering

**Le core émulateur fonctionne correctement !**

### ⏳ PROCHAINE ÉTAPE: REBUILD UE5 (URGENT)

**TROIS FIXES APPLIQUÉS** - Tous doivent être inclus dans le rebuild:

1. **FIX #1**: `MINIMUM_INTERRUPT_DELAY` - timing IRQ
2. **FIX #2**: `irq_line()` - formule de calcul corrigée
3. **FIX #3**: `shell_close_sent_ = 1` - pas d'INT5 spurious au boot ← **NOUVEAU**

Le plugin UE5 doit être recompilé pour inclure TOUS les fixes:

**IMPORTANT**: Live Coding NE RECOMPILE PAS les fichiers inclus via `#include`!
Le plugin utilise `#include "../../src/cdrom/cdrom.cpp"` donc:
- **Rebuild All** ou **fermer/rouvrir UE5 Editor** sont OBLIGATOIRES
- Live Coding ne suffit PAS

Étapes:
1. **Fermer UE5 Editor complètement**
2. Rouvrir le projet PSXVR
3. Le plugin sera recompilé automatiquement
4. OU: Build → Rebuild All (force recompilation)
5. Vérifier que `shell_sent=1` N'APPARAÎT PAS dans les logs au boot

**Architecture plugin**: Le plugin inclut directement le source:
```cpp
// R3000Core_CDROM.cpp
#include "../../src/cdrom/cdrom.cpp"  // ← Sera recompilé!
```

**DLL à surveiller**: `integrations/ue5/R3000Emu/Binaries/Win64/UnrealEditor-R3000EmuRuntime.dll`
doit avoir un timestamp plus récent après rebuild.

---

## 🔧 SYSTÈME DE VERSIONS (IMPORTANT)

**À chaque modification des sources, incrémenter la version !**

Les fichiers suivants ont des marqueurs de version au démarrage:

| Fichier | Log au démarrage | Version actuelle |
|---------|------------------|------------------|
| `src/emu/core.cpp` | `[CORE] R3000-Emu core vX` | v6 |
| `src/gte/gte.cpp` | `[GTE] GTE source vX` | **v9** |
| `src/r3000/cpu.cpp` | `[CPU] CPU source vX` | v6 |
| `src/r3000/bus.cpp` | `[BUS] BUS source vX` | **v12** |
| `src/gpu/gpu.cpp` | `[GPU] GPU source vX` | v6 |
| `src/cdrom/cdrom.cpp` | `[CD] CDROM source vX` | v6 |

### Historique des versions bus.cpp:
- **v5**: Fast CD timing (délais réduits 10x)
- **v6**: VSync stuck detection (dump état quand bloqué)
- **v7**: VSync rescue (deliver_events_for_class pour VBlank)
- **v8**: Force ALL events ready (scan table, force BUSY→READY)
- **v9**: Log rescued events (log class/spec pour identifier le bon événement)
- **v10**: Fix bounds check
- **v11**: SIO0 CTRL reset/acknowledge, STAT IRQ bit fix (bit 8→9)
- **v12**: SIO0 RXRDY/IRQ flag separation (ROOT CAUSE fix for stuck after logo)
- **v13-v20**: SIO0 pad input debugging (global g_pad_buttons, ACK timing, IRQ tracing)
- **v21**: Force I_MASK bit 7 for SIO0 IRQ (caused spurious exceptions)
- **v22**: SIO0 event delivery on transfer complete + clear I_STAT bit 7 (pad input fix)
- **v23**: Remove I_MASK bit 7 force + remove I_STAT from ACK (v22 caused BIOS stuck during StartPAD)
- **v24**: DuckStation-style delayed SIO0 transfers (3-state machine, BAUD timing, ACK 450 ticks, TXINTEN/RXINTEN/ACKINTEN)

**Quand modifier la version**:
1. Après chaque fix appliqué aux sources
2. Incrémenter le numéro (v3 → v4 → v5...)
3. Optionnel: ajouter un tag descriptif (ex: `v4 (timing_fix)`)

**Comment vérifier que le rebuild a fonctionné**:
1. Chercher dans les logs UE5 : `R3000-Emu core vX`
2. Si le numéro de version correspond, le code est à jour
3. Si ancien numéro, le rebuild n'a pas fonctionné

---

## 📌 SESSION PRÉCÉDENTE (2026-02-09) - CLI vs UE5

### 🔍 DÉCOUVERTE MAJEURE (Session 2):

**CLI et UE5 prennent des CHEMINS DE CODE DIFFÉRENTS après le logo !**

#### Comparaison détaillée après logo PlayStation:

| Métrique | CLI | UE5 |
|----------|-----|-----|
| OT addresses | 0x121DD4/0x1209B4 | 0x131184/0x153D78 |
| DMA2 nodes | **1** | **704** |
| DMA2 words | **6** (primitives!) | **0** (vide!) |
| Clip region | Valid (600x400+) | **(0,0)-(0,0)** = RIEN |
| GP1 RESET | Non observé | **Frame 399** |
| Rendu | ✅ Fonctionne | ❌ Bloqué |

### ❌ CAUSE RACINE IDENTIFIÉE:

**1. UE5 reçoit GP1 RESET à frame 399 (ligne 17150)**
```
[GPU] GP1 RESET              ← GPU state effacé!
[GPU] GP1 DISPLAY OFF
[GPU] FRAME #399: clip=(0,0)-(0,0)  ← Clip invalide!
```

**2. Après GP1 RESET, le clip reste (0,0)-(0,0)**
- Dernière CLIP_BR valide (639,479) à ligne 16120
- Après: toutes les CLIP_BR sont (0,0)
- Le jeu ne réinitialise JAMAIS le clip correctement

**3. CDROM IRQs manquants autour de la transition**
```
17139: CD set_irq(3) last_cmd=0x1E line=0->0  ← IRQ NOT RAISED!
17141: CD set_irq(3) last_cmd=0x1A line=0->0  ← IRQ NOT RAISED!
17143: i_mask=0x0000000C                       ← VBlank désactivé
```
Les commandes ReadTOC (0x1E) et GetID (0x1A) ne lèvent pas l'IRQ line.

**4. CLI ne fait PAS de GP1 RESET après le logo**
- CLI continue avec les mêmes adresses OT (0x121DD4)
- CLI a des primitives (words=6) → rendu visible
- CLI progresse: nodes=1025 → 1028 → 1032 avec words croissants

### 🔑 POURQUOI CLI ET UE5 DIVERGENT?

Le jeu détecte quelque chose de différent et prend un autre chemin:

1. **Timing CDROM**: Les IRQs ReadTOC/GetID qui ne lèvent pas `line=0->1`
   pourraient faire que le jeu pense que le CD n'est pas prêt

2. **I_MASK différent**: Au moment critique, UE5 a I_MASK=0x0000000C
   (VBlank désactivé) tandis que CLI a I_MASK=0x007D (tous activés)

3. **Le jeu fait un GP1 RESET** en UE5 (ligne 17150) mais PAS en CLI
   → Suggère que le jeu est dans un état d'erreur/réinitialisation en UE5

### ✅ PREUVE: CLI FONCTIONNE CORRECTEMENT

Test 60 secondes CLI:
```bash
./build/Debug/r3000_emu.exe --bios="SCPH-7502.bin" --cd="Ridge Racer (U).cue"
```
- VBlank #1 → #251 atteint
- Progression: nodes=1025 (logo) → nodes=1032 (jeu avec primitives)
- **words=37** = 37 mots de primitives GPU par frame = RENDU ACTIF!

### 🔬 SESSION 2 - ANALYSE APPROFONDIE (2026-02-09)

#### Les deux modes ont GP1 RESET à VBlank #400!

**Test CLI 3 minutes:**
```bash
./build/Debug/r3000_emu.exe --bios="SCPH-7502.bin" --cd="Ridge Racer.cue"
```

| Événement | CLI | UE5 |
|-----------|-----|-----|
| GP1 RESET #1 | Boot | Boot |
| GP1 RESET #2 | ~VBlank #400 | ~VBlank #400 |
| Après reset | **Continue!** VBlank #801 | **Bloqué!** |
| clip=(0,0)-(0,0) | **ZÉRO** | **BEAUCOUP** |

#### UE5: Le jeu SET EXPLICITEMENT clip=(0,0)-(0,0)

```
17204→[GPU] GP0 ENV CLIP_TL (0,0)
17205→[GPU] GP0 ENV CLIP_BR (0,0)    ← Le jeu fait ça exprès!
17206→[GPU] GP0 ENV DRAW_OFFSET (0,0)
```

Ce n'est **PAS** un bug d'émulation - le jeu envoie ces commandes!

#### Différence clé: Adresse LBA lue après GP1 RESET

| Mode | LBA après GP1 RESET | Résultat |
|------|---------------------|----------|
| CLI | **LBA=16** | ✅ Jeu continue |
| UE5 | LBA autre (?) | ❌ clip=(0,0)-(0,0) |

CLI lit LBA=16 après reset:
```
[CD] SetLoc: MSF=00:02:16 -> LBA=16
```

UE5 ne montre PAS de SetLoc LBA=16 dans les logs!

#### Hypothèse finale:

Le jeu prend un **chemin de code différent** basé sur:
1. L'état mémoire qui diffère entre CLI et UE5
2. Une variable ou flag qui n'est pas correctement initialisé
3. Un timing subtil qui cause une condition de course

Le jeu pense être dans un état d'erreur/réinitialisation en UE5 et:
- Configure clip=(0,0)-(0,0)
- Ne charge pas les bons secteurs CD
- Reste bloqué dans une boucle d'attente

### Prochaines étapes:
1. [ ] Comparer les secteurs CD lus après GP1 RESET (CLI vs UE5)
2. [ ] Tracer quelle variable d'état cause le clip=(0,0)
3. [ ] Vérifier si un flag mémoire diffère (0x80040018 = game code)
4. [ ] Tester avec DuckStation pour avoir une référence

---

**⚠️ NE PAS TOUCHER À `deliver_events_for_class` POUR VBLANK/CDROM !**

### ❌ RÉGRESSION CAUSÉE PAR CES FIXES (REVERTÉS):

J'ai essayé d'ajouter `deliver_events_for_class()` pour VBlank et CDROM:
```cpp
// FIX #1 (REVERT): VBlank - CASSAIT LE LOGO PLAYSTATION
deliver_events_for_class(ram_, ram_size_, 0xF000'0001u);

// FIX #2 (REVERT): CDROM classe 0xF0000003 - CASSAIT AUSSI
deliver_events_for_class(bus->ram_, bus->ram_size_, 0xF000'0003u);
```

**RÉSULTAT:** Régression ! On perdait le logo PlayStation (280 triangles).
- AVANT les fixes: Sony ✅ + PlayStation ✅ (280 tris)
- APRÈS les fixes: Sony ✅ + PlayStation ❌ (plus affiché!)

### ✅ REVERT APPLIQUÉ:
Les fixes ont été retirés. Retour à l'état précédent:
- Sony logo: ✅
- PlayStation logo (280 tris): ✅
- Après PlayStation logo: ❌ (bloqué, 0 primitives)

### 🔑 LEÇON APPRISE:
**Le BIOS exception handler (0x80000080) gère DÉJÀ la délivrance des événements!**

En appelant `deliver_events_for_class()` nous-mêmes, on DOUBLE-DÉLIVRE les événements,
ce qui corrompt l'état du système d'événements BIOS et casse le jeu.

La classe `0x28` pour CDROM est correcte et suffisante - c'est ce que le BIOS utilise.
Ne PAS ajouter `0xF0000003` qui est utilisé seulement en mode HLE.

### Ce qui fonctionne actuellement:
- ✅ Boot BIOS complet
- ✅ Logo Sony (son + image)
- ✅ Logo PlayStation License (280 triangles)
- ✅ CDROM lecture (données chargées)

### Ce qui ne fonctionne PAS:
- ❌ Après logo PlayStation: 0 primitives, clip=(0,0)-(0,0)
- ❌ Galaga (mini-jeu loading) jamais affiché
- ❌ Le jeu reste bloqué en mode "loading"

### Analyse des logs (2026-02-09):

**Séquence observée dans system.log:**
1. CDROM lit LBA 4-477 → OK (940KB chargé)
2. CMD Pause → IRQ3 Complete → OK
3. DMA4 SPU → audio chargé → OK
4. Frame #488+: **0 primitives, clip=(0,0)-(0,0)**
5. CPU alterne entre:
   - `0x8005699x` = game code (boucle VSync wait)
   - `0x00001Exx` = BIOS exception handler
6. VBlank continue: #501 → #551 → ... → #851
7. `i_stat=0x00000001` (VBlank) apparaît parfois, puis est cleared
8. **Le jeu ne sort JAMAIS de sa boucle VSync pour rendre Galaga**

**Conclusion:**
- Les IRQs VBlank ARRIVENT (i_stat=1 visible)
- Le BIOS exception handler TOURNE (PC=0x00001Exx)
- MAIS le callback VBlank du jeu ne fait pas ce qu'il devrait
- Le jeu reste coincé dans sa boucle d'attente VSync

### Questions ouvertes:
1. **Quelle BIOS?** Certains BIOS ont des comportements différents
2. **Est-ce que HLE mode fonctionne?** Si oui, le problème est dans l'interaction BIOS/hardware
3. **Comparer avec DuckStation** pour voir où ça diverge

---

## Historique: UE5 se bloquait après les logos Sony/PlayStation

**RELIRE CE FICHIER À CHAQUE FOIS AVANT DE CONTINUER LE DEBUG**

---

## Symptômes observés

1. **Logo Sony** : S'affiche correctement avec son ✅
2. **Logo PlayStation License** : S'affiche correctement ✅
3. **Son** : Fonctionne pendant les logos, puis COUPE ❌
4. **Après les logos** : Rien ne s'affiche, VRAM vide, le jeu semble bloqué ❌
5. **CLI** : Fonctionne parfaitement, le jeu démarre ✅
6. **UE5** : Se bloque après les logos ❌

---

## Chemins des logs (CLAUDE.md)

- UE5 logs : `E:\Projects\github\Live\PSXVR\logs\`
  - `system.log` - logs système/core
  - `gpu.log` - logs GPU
  - `cdrom.log` - logs CD-ROM
  - `io.log` - logs I/O

---

## Différences CLI vs UE5

### CLI (main.cpp lignes 455-513)
```cpp
for (;;)
{
    const auto res = core.step();
    // Simple loop, pas de timing, exécute aussi vite que possible
    ++steps;
}
```

### UE5 Worker Thread (R3000EmuComponent.cpp lignes 87-260)
```cpp
// Calcul des cycles cibles basé sur wall-clock
const double Now = FPlatformTime::Seconds();
const double Elapsed = Now - StartTime;
const uint64 TargetTotalCycles = static_cast<uint64>(Elapsed * kPS1CpuClock);

// Exécution en batches de 1024
for (uint32 i = 0; i < Batch; ++i)
{
    const auto Res = Core->step();
}
```

---

## Questions à investiguer

1. **Le CPU continue-t-il à exécuter après les logos ?**
   - Vérifier les PC samples dans system.log
   - Le PC devrait avancer, pas rester bloqué

2. **Les IRQs sont-elles délivrées correctement ?**
   - VBlank IRQ (bit 0) - nécessaire pour le jeu
   - CDROM IRQ (bit 2) - nécessaire pour charger le jeu

3. **Le CDROM répond-il ?**
   - Le jeu charge l'EXE depuis le CD après le BIOS
   - Si pas de réponse CDROM, le jeu reste bloqué

4. **Y a-t-il une différence de timing ?**
   - CLI : pas de limite, exécute à fond
   - UE5 : limité à wall-clock (33.8688 MHz simulé)

5. **Le son coupe = SPU s'arrête ou buffer vide ?**
   - Si SPU s'arrête : le CPU ne tick plus le SPU
   - Si buffer vide : le CPU est trop lent

---

## Hypothèses actuelles

### Hypothèse 1 : IRQ VBlank manquante en UE5
- Le jeu attend VBlank pour continuer
- Si VBlank IRQ n'arrive pas, le jeu boucle infiniment

### Hypothèse 2 : CDROM bloqué
- Le jeu demande des données CD
- CDROM ne répond pas (IRQ INT2 manquante?)
- Le jeu attend indéfiniment

### Hypothèse 3 : Timing différent
- En CLI, l'émulateur va plus vite que le temps réel
- En UE5, limité au temps réel, peut-être trop lent pour certaines conditions de course

---

## Ce qu'il faut vérifier dans les logs UE5

1. **PC samples** : Le PC change-t-il après les logos ?
   ```
   Worker PC sample steps=XXX pc=0xXXXXXXXX
   ```

2. **VBlank count** : Les VBlanks continuent-elles ?
   ```
   VBlank #XXX
   ```

3. **CDROM activity** : Le CD est-il lu ?
   ```
   [CDROM] ...
   ```

4. **DMA2 (GPU)** : Y a-t-il des primitives après les logos ?
   ```
   DMA2 LL: start=... nodes=... words=...
   ```

---

## Actions à faire

1. [ ] Lire les logs UE5 après un test (system.log, cdrom.log)
2. [ ] Comparer les dernières lignes avant le blocage
3. [ ] Chercher où le PC se stabilise (boucle infinie?)
4. [ ] Vérifier si VBlank continue après les logos
5. [ ] Vérifier si CDROM reçoit/répond aux commandes

---

## Propositions de l'utilisateur

- Mettre un marqueur/breakpoint quelque part pour identifier le point exact de blocage
- Comparer CLI vs UE5 sur le même nombre de steps

---

## Notes techniques

- PS1 CPU : 33.8688 MHz
- VBlank PAL : ~50 Hz (680,688 cycles par frame)
- CDROM : IRQ2 (bit 2 de I_STAT)
- Le jeu Ridge Racer charge depuis le CD après le BIOS

---

## Timing DuckStation (référence)

Ces timings sont pour un boot normal avec BIOS réel (pas fast-boot) :

| Milestone | DuckStation (approx) | Description |
|-----------|---------------------|-------------|
| BIOS Start | 0 ms | Reset vector 0xBFC00000 |
| BIOS → Shell | ~1800 ms | PC passe de 0xBFCxxxxx à 0x800xxxxx |
| Sony Logo (SCE) | ~2000 ms | Premier affichage GPU |
| PlayStation License | ~4500 ms | Texte "Licensed by..." |
| License End | ~6000 ms | Fin de l'écran de license |
| Game Start | ~8000-10000 ms | Le jeu commence vraiment |

### Ce qui se passe à chaque étape :
1. **BIOS** : Initialise hardware, teste RAM, cherche CD
2. **Shell load** : BIOS charge le "PlayStation shell" depuis le CD (premier secteur)
3. **Sony Logo** : Le shell affiche le logo SCE avec son
4. **License** : Le shell affiche "Licensed by Sony..."
5. **Game EXE load** : Le shell lit SYSTEM.CNF, charge l'EXE du jeu
6. **Game start** : Jump vers l'entry point du jeu

### Code ajouté pour tracker les milestones :
Dans `core.cpp` step(), on log maintenant :
- `=== BOOT START ===`
- `=== BIOS → SHELL/GAME ===` (avec timing)
- `=== FIRST GPU PRIMITIVES ===` (avec timing)
- `=== LICENSE END ===` (frame 200, avec timing)

---

## Observations récentes des logs UE5

Dernière lecture des logs (frame 488-493) :
- **GPU FRAME #488-493** : `0 tri, 0 quad, 0 rect` = AUCUN PRIMITIF !
- **DRAWENV** : `clip=(0,0)-(0,0)` = CLIP INVALIDE !
- **DMA2 LL** : `nodes=704 words=0` = OT vide, pas de primitives
- **CDROM** : Continue à lire (LBA avance) = CD fonctionne
- **DMA3** : Continue = données CD chargées en RAM
- **DMA4 SPU** : `words=122768` = son chargé

### PROBLÈME IDENTIFIÉ :

**Séquence du problème (ligne 17154 du system.log UE5) :**
```
[GPU] GP1 RESET                    ← Le shell appelle GPU reset avant de lancer le jeu
[GPU] GP1 DISPLAY OFF
[GPU] FRAME #404: clip=(0,0)-(0,0) ← Clip invalide après reset
[CORE] PC=0xBFC09158               ← Retour dans le BIOS (fonction de reset GPU?)
[CORE] PC=0x8004C0E4               ← Le jeu tourne (game code)
[CORE] PC=0x8004E858               ← Le jeu continue...
```

**Après le GP1 RESET:**
- Le clip reste à (0,0)-(0,0) = invalide
- Le jeu NE REMET JAMAIS le clip correctement
- Donc aucune primitive ne peut être dessinée

**Question clé:**
Pourquoi le jeu ne réinitialise-t-il pas le clip après le reset ?
- Problème de timing ? Le jeu attend quelque chose ?
- Problème d'IRQ ? Le jeu est bloqué dans une boucle ?
- Problème CDROM ? Le jeu attend des données ?

**À vérifier:**
1. Le CLI fait-il le même GP1 RESET ?
2. Après le reset en CLI, le clip revient-il à une valeur correcte ?
3. Combien de temps entre GP1 RESET et le premier primitive en CLI ?

---

## !! DÉCOUVERTE MAJEURE !!

**CORRECTION: LE CLI FONCTIONNE !** (testé le 2026-02-08)

Le crash précédent était dû à un **mauvais chemin CD** qui n'existait pas.
Avec le bon CD (`ridgeracer.cue`), le jeu CLI affiche:
- Frame 264-283: **278 triangles, 1 quad** (le jeu tourne correctement !)
- Frame 285-293: 8 rectangles (loading screen)

**Donc le problème est SPÉCIFIQUE à UE5, pas au core de l'émulateur.**

---

## ANCIENNE ANALYSE (crash sans CD - ignorez si CD chargé) :

**LE CLI CRASH AUSSI (SANS CD) !** (testé le 2024-02-08)

```
[ERROR] [CPU] IFETCH fault kind=1 vaddr=0xFFFFFFFF paddr=0xFFFFFFFF — raising ADEL
[ERROR] [CPU] *** CRASH *** ADEL EPC=0xFFFFFFFF BadVAddr=0xFFFFFFFF SP=0x801FFD00 RA=0xFFFFFFFF
[INFO] [CPU] DIAG: branch to 0xFFFFFFFF from PC=0x80065DC8 RA=0xFFFFFFFF
```

**Analyse du crash:**
- PC = 0x80065DC8 : Le jeu essaie de faire `JR $ra` (retour de fonction)
- RA = 0xFFFFFFFF : L'adresse de retour est corrompue !
- L'instruction 0x03E00008 = JR $ra

**Séquence avant crash (CLI):**
```
FRAME #230: 4 tri, 4 quad, 4 rect  ← Dernière frame avec primitives
GP1 CLEAR_FIFO
FRAME #231: 0 tri, 0 quad, 0 rect  ← Plus rien
CD CMD 0x19 (Test)                 ← Le jeu teste le CD
CD CMD 0x01 (GetStat)
CRASH → JR $ra avec RA=0xFFFFFFFF
```

**Ceci est un BUG D'ÉMULATION, pas un problème UE5-spécifique !**

**CONFIRMATION: UE5 aussi est bloqué dans le BIOS !**
PC samples UE5 à la fin :
```
PC=0x000005EC, 0x00001ED0, 0x00001EEC  ← Boucle BIOS (exception handler)
```

**CLI vs UE5:**
- CLI: IFETCH fault à 0xFFFFFFFF → exception ADEL → boucle dans handler BIOS
- UE5: Même chose, mais pas de log IFETCH visible

Les deux finissent bloqués dans la boucle d'exception du BIOS.

**CAUSE RACINE:**
Le RA est corrompu à 0xFFFFFFFF quelque part avant l'appel à 0x80065DC8.

Possibilités:
1. Bug dans la gestion de la stack (mauvais LW/SW?)
2. Bug dans les syscalls HLE qui corrompent les registres
3. Bug dans la gestion des exceptions qui restaure mal les registres
4. Le jeu utilise une fonctionnalité non implémentée
5. **Open bus read** : lecture d'une adresse non mappée retourne 0xFFFFFFFF (bus.cpp:541)
   → Si le jeu charge RA depuis une adresse invalide, RA = 0xFFFFFFFF

**PROCHAINE ÉTAPE:**
Tracer les LW $ra, XXX($sp) avant le crash pour voir d'où vient 0xFFFFFFFF

---

---

## SESSION 2026-02-08 : Nettoyage et état actuel

### Code nettoyé :
- Retiré `dump_debug_state()` fonction de debug
- Retiré détection RA CORRUPTION dans LW
- Retiré détection JR to 0xFFFFFFFF
- Simplifié callback CDROM garbage SetLoc

### Observations CLI récentes (50M steps) :
- Frames 36-73 : 0-2 tri, 2 quads (logos Sony/PlayStation)
- Frame 70 : Premier triangles détectés (2 tri, 3 quad)
- VBlank #51 atteint à 50M steps (~1 seconde simulée)
- **50M steps = ~1 seconde, donc pour atteindre le jeu (~6-8 sec) il faut ~300-400M steps**

### CDROM fonctionne (UE5 logs récents) :
- LBA 454-477 lus avec succès
- Pause envoyée correctement
- IRQ1/IRQ2 délivrées normalement

---

## !! CAUSE RACINE TROUVÉE !! (2026-02-08)

### Différence CLI vs UE5 :
- **CLI** : Utilise `--hle` → `hle_vectors=1` → exceptions interceptées à 0x80000080
- **UE5** : `bHleVectors=false` par défaut → `hle_vectors=0` → BIOS réel gère les exceptions

### Symptôme :
Quand `hle_vectors=0`, le CPU boucle infiniment dans le kernel exception handler :
```
PC samples: 0x00001EDC, 0x00001F08, 0x000005E8, 0x000005FC
i_stat=0x00000000, i_mask=0x0000000D
```
Le handler dispatche les callbacks VBlank mais le jeu reste dans une boucle d'attente VSync.

### Analyse DMA :
Les logs montrent que DMA3/DMA4 ne génèrent PAS d'IRQ :
```
DMA3 finish: DICR=0x4C000000 flags=0x4C en=0x00 master_en=0 force=0 flag_set=0 irq_fired=0
```
Le jeu a `i_mask` bit 3 (DMA) activé mais `master_en=0` dans DICR → pas d'IRQ DMA.

---

## ✅ FIX APPLIQUÉ (2026-02-08)

### Fix HLE (ACTIF) :
Fichiers modifiés :
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000EmuComponent.cpp`
- `integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Public/R3000EmuComponent.h`

```cpp
// R3000EmuComponent.cpp - InitEmulator()
emu::Core::InitOptions Opt{};
// BIOS boot requires HLE vectors - our hardware emulation isn't accurate enough
// for the real BIOS exception handler to work correctly without HLE interception.
Opt.hle_vectors = 1;
```

```cpp
// R3000EmuComponent.h
// [DEPRECATED] HLE vectors are now always enabled for BIOS boot.
// Our hardware emulation isn't accurate enough for the real BIOS exception
// handler to work correctly without HLE interception. This setting is ignored.
UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "R3000Emu", meta = (DeprecatedProperty))
bool bHleVectors{true};
```

### Statut :
- ✅ CLI : Fonctionne (testé avec `--hle --max-steps=5000000`)
- ⏳ UE5 : En attente de recompilation du plugin par l'utilisateur

### Pour tester en UE5 :
1. Ouvrir le projet UE5 dans l'éditeur
2. Recompiler le plugin R3000Emu (automatique ou Build → Rebuild)
3. Relancer le jeu → devrait fonctionner avec HLE forcé

---

## 📋 FIX NON-HLE (Futur)

Pour que le BIOS réel fonctionne sans HLE, il faudrait :
1. Désassembler le code BIOS aux adresses 0x00001EDC etc.
2. Comprendre ce que le handler attend exactement
3. Corriger notre émulation I_STAT/I_MASK/DICR/timers
4. Implémenter les IRQs edge-triggered comme DuckStation (SetLineState)

Le BIOS exception handler fait :
1. Vérifier I_STAT & I_MASK
2. Dispatcher aux handlers via SysEnqIntRP chains (RAM[0x100+prio*4])
3. Les handlers du jeu ne s'exécutent pas correctement ou ne mettent pas à jour les compteurs VSync

---

---

## ✅ SESSION 2026-02-09 : NON-HLE FONCTIONNE !

### CORRECTION IMPORTANTE :
**Le mode non-HLE fonctionne maintenant !** L'utilisateur NE VEUT PAS de HLE.

### Modifications apportées cette session :

#### 1. Thread-safety VRAM (gpu.h)
```cpp
// Ajouté: copie thread-safe de VRAM pour UE5
void copy_vram(uint16_t* out, uint32_t& out_seq) const
{
    std::lock_guard<std::mutex> lock(draw_list_mutex_);
    std::memcpy(out, vram_.get(), kVramPixels * sizeof(uint16_t));
    out_seq = vram_write_seq_;
}

uint32_t vram_write_seq_locked() const
{
    std::lock_guard<std::mutex> lock(draw_list_mutex_);
    return vram_write_seq_;
}
```

#### 2. Thread-safety PutcharCB (R3000EmuComponent)
```cpp
// PutcharCB queue les lignes au lieu de broadcast direct
FScopeLock Lock(&Self->PutcharLock_);
Self->PutcharPendingLines_.Add(Self->PutcharLineBuf_);

// TickComponent broadcast sur le game thread
TArray<FString> LinesToBroadcast;
{
    FScopeLock Lock(&PutcharLock_);
    LinesToBroadcast = MoveTemp(PutcharPendingLines_);
}
for (const FString& Line : LinesToBroadcast)
    OnBiosPrint.Broadcast(Line);
```

#### 3. Respect de bHleVectors (IMPORTANT!)
```cpp
// AVANT (bug): Opt.hle_vectors = 1; // Forçait HLE secrètement!
// APRÈS (fix): Opt.hle_vectors = bHleVectors ? 1 : 0;
```

### Résultats du test non-HLE (Ridge Racer US) :

| Élément | Résultat |
|---------|----------|
| Boot BIOS | ✅ PC=0xBFC00000 → game code |
| CDROM boot | ✅ 940KB chargé (LBA 4-477) |
| GPU intro 3D | ✅ 278 triangles (frames 275-284) |
| "Press Start" | ✅ 8 rectangles (frames 285-313) |
| VBlank IRQ | ✅ #1 → #951 (continu) |
| Mode attract | ✅ Charge LBA 478, 238 |
| Worker exit | ✅ Normal (pas de crash) |

### Séquence observée :
1. **Frames 36-73** : Logos Sony/PlayStation (fade in/out)
2. **Frames 75-284** : Intro 3D Ridge Racer (278 tri, 1 quad)
3. **Frames 285-313** : "Press Start" (8 rect, fade effect)
4. **Frame 314+** : Mode attract loading (VBlank masqué, CD streaming)
5. **Frames 437-488** : Écran vide (0 primitives, attente données)

### Comportement attendu :
- Sans appuyer sur START, le jeu entre en mode démo après ~5 sec
- Pendant le chargement démo, I_MASK=0x0C (VBlank bit 0 désactivé)
- C'est **normal** - le jeu masque VBlank pendant le streaming CD

### Question ouverte :
**Est-ce que l'affichage UE5 montre les triangles/rectangles ?**
- Si OUI → émulation OK, jeu attend input
- Si NON → problème côté rendu UE5 (copie VRAM → texture)

---

## Configuration recommandée (UE5 Blueprint) :

| Property | Valeur | Raison |
|----------|--------|--------|
| bHleVectors | **false** | Non-HLE préféré par l'utilisateur |
| bThreadedMode | true | Timing précis via waitable timer |
| BusTickBatch | 1 | Cycle-accurate |
| CycleMultiplier | 1 | Timing normal |
| bFastBoot | false | Boot BIOS complet |

---

## Chemins importants :

| Fichier | Chemin |
|---------|--------|
| UE5 Logs | `E:\Projects\github\Live\PSXVR\logs\` |
| CLI Build | `E:\Projects\github\Live\R3000-Emu\build\Release\r3000emu.exe` |
| BIOS | Configuré dans Blueprint `BiosPath` |
| CD Image | `E:\Projects\PSX\roms\Ridge Racer (U).cue` |

---

## RAPPEL : TOUJOURS RELIRE CE FICHIER AVANT DE CONTINUER LE DEBUG !

---

## SESSION 2026-03-22 - État actuel exact (Tekken Europe UE5)

### Correctifs réellement appliqués dans cette phase

1. **IRQ CD matérielle**
   - push `Cdrom -> Bus`
   - latch direct de `I_STAT bit 2`
   - traces ajoutées :
     - `CDROM IRQ push`
     - `BUS IRQ2 latch`
     - `I_STAT ACK CD via SW`

2. **Fiabilité des traces IRQ CPU**
   - suppression du biais des quotas `static` persistants entre runs PIE
   - les compteurs de logs IRQ critiques sont maintenant réarmés par instance core

3. **Trace BIOS MMIO CD**
   - ajout de `CDMMIO BIOS RD/WR`
   - fenêtre tracée :
     - `0xBFC04A00 .. 0xBFC05580`

4. **Fix structurel du registre `1F801803` index 1/3**
   - avant :
     - le code renvoyait `irq_flags | cmd_ready | 0xE0`
     - donc `F1/F2/F3`
   - après :
     - il renvoie `(irq_flags & 0x1F) | 0xE0`
     - donc `E1/E2/E3`
   - bannière :
     - `CDROM source v18 (irq_flag_reg_fix)`

### Ce qui est maintenant prouvé par logs

- Les bons secteurs sont lus :
  - `SYSTEM.CNF` (`LBA 60642`)
  - demande du premier secteur `PS-X EXE` (`LBA 60643`)
- DMA3 fonctionne.
- Le BIOS voit l'IRQ CD et l'acquitte.
- Le fix `v18` est bien chargé en UE5.

### Ce qui échoue encore

Le run retombe toujours sur :
- `FAULT code=5 (ADES)`
- `PC=0xBFC03D1C`
- `BadVAddr=0x3D20544E`

`0x3D20544E` correspond à du texte `SYSTEM.CNF` (`NT =` en little-endian).

### Diagnostic courant le plus probable

Le verrou principal n'est plus :
- ni un secteur faux
- ni une IRQ CD perdue
- ni un DMA3 cassé
- ni le vieux bug du registre `IRQ Flag`

Le suspect principal est maintenant :
- **l'arbitrage des commandes CD pendant un `ReadN` encore actif**

Observation clé :
- pour `LBA 60642`, la séquence est cohérente :
  - `ReadN`
  - `INT1`
  - `DMA3`
  - `Pause`
- pour `LBA 60643`, on voit :
  - `ReadN/S START: LBA=60643`
  - puis avant une complétion normale du secteur :
    - `ReadTOC`
    - `GetID`

Donc le contrôleur accepte encore des commandes BIOS alors qu'une lecture `ReadN` est logiquement vivante.

### Prochaine étape

Durcir le **command gating** dans `src/cdrom/cdrom.cpp` :
- empêcher `ReadTOC` / `GetID` / autres commandes de s'exécuter au milieu d'un `ReadN` encore actif
- les mettre en queue proprement ou les rejeter selon le vrai comportement matériel

---

## Update 2026-03-22 (late) - Deferred CD command execution

- `1F801803` index `1/3` bug fixed first:
  - controller now returns `E1/E2/E3` instead of `F1/F2/F3`
- this did not unlock the boot by itself
- stronger divergence vs DuckStation identified:
  - commands were executed immediately on `CMD_WRITE`
  - response FIFO bytes became visible immediately
  - only the matching IRQ was delayed
- current structural change:
  - `CDROM source v19 (deferred_command_exec)`
  - command writes now latch a pending command
  - command execution itself happens later from `Cdrom::tick()`
  - queued commands restart through the same deferred pipeline
- goal:
  - align the controller with DuckStation's command-event model
  - remove BIOS-visible impossible states where response bytes appear before the matching command event timing
## 2026-03-22 late update - UE5 vs CLI divergence after PVD

### Proven with current build markers
- UE5 current logs show:
  - `CDROM source v19 (deferred_command_exec)`
  - `BUS source v28 (session_2026_03_22)`
- BIOS DMA dump in UE5 is correct for the ISO9660 PVD:
  - `pc=0xBFC06618`
  - `madr=0xA000B070`
  - ASCII `.CD001..PLAYSTAT`
- Therefore:
  - PVD payload is correct
  - DMA3 to BIOS RAM is correct
  - the blocker is not raw ISO payload corruption

### New comparison: UE5 vs CLI
A fresh CLI run with the same BIOS/disc and the same current core was executed from:
- `build/Debug/r3000_emu.exe`
- BIOS: `SCPH-7502 EU`
- CD: `Tekken (Europe).cue`

#### CLI behavior (current core)
From `logs/cdrom.log` after the fresh CLI run:
- `LBA=16` is read successfully
- then the flow continues with:
  - `Async IRQ1 delivered (resp=0x22)`
  - `FIFO LBA=16 [01434430 30310100]`
  - `CMD 0x09 (Pause)`
  - then new reads:
    - `LBA=18`
    - `LBA=22`
    - `LBA=60642` (`SYSTEM.CNF`)
    - `LBA=60643` (`PS-X EXE`)
- So the current core can still progress past the PVD under CLI.

#### UE5 behavior (current core)
From `PSXVR/logs/cdrom.log` and `system.log`:
- `LBA=16` is read successfully
- BIOS DMA dump confirms correct PVD bytes in RAM
- immediately after that, UE5 shows:
  - `QUEUE CMD 0x09 (Pause): irq=0x00 busy=0 cmd_exec=0 pend_type=1 read_pend=0 async=0`
  - `Cancelled pending continuous ReadN INT1 advance`
- and then no follow-up CD requests for:
  - `LBA=18`
  - `LBA=22`
  - `LBA=60642`
  - `LBA=60643`

### Current best diagnosis
- The core is now proven capable of progressing past `LBA=16` in CLI.
- The UE5-specific blocker is therefore a runtime divergence after the PVD read, not a generic CD payload bug.
- The first useful divergence is:
  - CLI reaches `Async IRQ1 -> FIFO consume -> Pause -> next SetLoc/ReadN`
  - UE5 stops right after `LBA16 DMA3 done` and never reaches the later BIOS ISO path.

### Most likely active suspect now
- UE5 timing/thread/runtime interaction around the post-`INT1` CD path:
  - `ReadN(LBA16)`
  - `INT1`
  - DMA3 completion
  - `Pause`
  - delivery/consumption of the next BIOS-visible response/state
- This is no longer a pure CD content issue.
- It is also no longer a generic CLI/core issue.
- It is now specifically an UE5-vs-CLI divergence.

## 2026-03-22 correction - UE5 does consume PVD setup after all

The latest UE5 run with elevated `info` tracing for `LBA=16` shows that UE5 does execute the expected BIOS-side MMIO sequence after `INT1`:
- clears IRQ1
- sets Want Data via `WR 0x3 idx=0 val=0x80`
- fills FIFO:
  - `FIFO LBA=16 [01434430 30310100]`
- performs DMA3 from the FIFO to `0xA000B070`
- then writes `CMD 0x09 (Pause)`

So the earlier conclusion "UE5 stops before consuming the PVD" was too strong.
What is actually proven now is narrower:
- UE5 consumes the PVD sector setup correctly up to FIFO+DMA+Pause
- but unlike CLI, UE5 stops immediately after this `Pause`
- CLI continues from the same stage toward later BIOS reads (`LBA=18`, `22`, `60642`, `60643`)

Current best diagnosis after this correction:
- the divergence is not on PVD bytes and not on WantData/FIFO/DMA for `LBA=16`
- the divergence is specifically on what happens immediately after the BIOS issues `Pause` after the PVD read
- the active suspect remains UE5 runtime/timing/order interaction around post-`INT1` `Pause` handling

## 2026-03-22 latest update - no longer a pure stall, now a reproducible BIOS crash

The latest UE5 run with the queued-`Pause` restart fix no longer stops only at the old `LBA16` point.
It now progresses into later BIOS parsing and then crashes deterministically:

- `FAULT code=5(ADES)`
- `PC=0xBFC03D1C`
- `BadVAddr=0x3D20544E`
- `ra=0xBFC0D5F0`

### What is now proven

- The queued `Pause` command was one real issue:
  - after fixing queued-command restart in `Cdrom::tick()`, UE5 advances further than the previous `LBA16 -> Pause` stop.
- The BIOS RAM scratch buffer is now observed directly during the failing path.
- New `CDRAM` traces show the BIOS reading RAM words immediately before the crash:
  - `pc=0xBFC03CF0 addr=0x0000B88C -> 0x0D303120`
  - `pc=0xBFC03D04 addr=0x0000B888 -> 0x3D20544E`
- `0x3D20544E` is little-endian ASCII for `NT =`
  - this is text from `SYSTEM.CNF`
- Earlier `CDRAM` byte reads in the same run show the BIOS parsing `BOOT = cdrom:SCES_000.05;1` from RAM.

### Practical reading

- This is no longer just "UE5 stalls after `Pause`".
- UE5 now reaches the BIOS `SYSTEM.CNF` parsing path.
- The current failure is:
  - the BIOS later dereferences / uses a RAM word containing `SYSTEM.CNF` text as if it were structured data or an address
  - this leads to `BadVAddr=0x3D20544E`

### Current best diagnosis

- The active bug is no longer:
  - raw IRQ2 loss
  - raw DMA3 failure
  - bad PVD payload
  - bad `WantData`
- The active bug is now most likely:
  - wrong post-`Pause` sector/state sequencing in the BIOS scratch buffer area
  - i.e. a text sector (`SYSTEM.CNF`) is present in RAM in a state/context where the BIOS path at `0xBFC03CF0..0xBFC03D1C` expects something else

### Current target

- Focus on the exact LBA/sector ordering that populates the BIOS scratch region around:
  - `0xA000B070..0xA000B88F`
- The next useful correction is in CD state/sequencing after `Pause`, not in raw DMA plumbing.
