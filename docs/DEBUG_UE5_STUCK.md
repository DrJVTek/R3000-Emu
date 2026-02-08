# DEBUG: UE5 se bloque après les logos Sony/PlayStation

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

### À tester :
1. Rebuild UE5 plugin avec code nettoyé
2. Tester UE5 en comparant avec CLI au même nombre de steps
3. Vérifier si les 278 triangles apparaissent en UE5 à ~frame 264

---

## RAPPEL : TOUJOURS RELIRE CE FICHIER AVANT DE CONTINUER LE DEBUG !
