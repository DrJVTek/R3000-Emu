# IRQ Threads Architecture

## Motivation

L'émulateur actuel tick tous les périphériques séquentiellement dans `Bus::tick()` — soit à chaque instruction CPU (full path), soit batché via `tick_peripherals()`. Ce modèle cause des divergences de timing entre CLI et UE5 car le batching change le moment exact où les IRQs fire.

Sur vrai hardware PS1, chaque périphérique a son propre oscillateur/horloge et fire des IRQs de manière **asynchrone** au CPU. Le CPU se fait interrompre quand ça arrive.

## Architecture proposée

### Principe
- **Thread principal (CPU)** : exécute les instructions MIPS, le seul à accéder la RAM
- **Threads IRQ** (un par source hardware) : timing réel → set un bit dans `I_STAT` (atomique)
- Le CPU vérifie `I_STAT` après chaque instruction → si pending, sauve PC dans EPC, jump 0x80000080

### Threads IRQ

| Thread | Source | Timing | I_STAT bit |
|--------|--------|--------|------------|
| VBlank | GPU scanline counter | ~16.67ms (PAL: ~20ms) | bit 0 |
| CDROM | Drive sector delivery | ~6.67ms (2x speed) | bit 2 |
| DMA | Transfer complete | Variable | bit 3 |
| Timer 0 | Dotclock / sysclk | Target/overflow | bit 4 |
| Timer 1 | HBlank / sysclk | Target/overflow | bit 5 |
| Timer 2 | Sysclk/8 / sysclk | Target/overflow | bit 6 |
| SIO0 | Transfer complete | BAUD * 8 ticks | bit 7 |

### Synchronisation

- `I_STAT` = `std::atomic<uint32_t>` — chaque thread set son propre bit via `fetch_or`
- Pas de locks nécessaires — chaque thread écrit un bit unique
- Le CPU thread est le SEUL à :
  - Exécuter des instructions
  - Lire/écrire la RAM
  - Lire les registres MMIO
  - Clear les bits I_STAT (via write AND)

### Timers : calcul à la lecture

Les timers n'ont pas besoin de threads. Au lieu de compter cycle par cycle, on **calcule la valeur au moment de la lecture** :

```cpp
uint16_t timer_read_count(int ch) {
    uint64_t elapsed = now_cycles - timer_start_cycles[ch];
    if (use_external_clock) elapsed /= prescaler;
    return (uint16_t)(elapsed % (target + 1));
}
```

L'IRQ timer fire quand le compteur atteint target ou overflow. On pré-calcule le cycle exact où ça arrivera et on programme un timer OS (ou un check dans le CPU loop) :

```cpp
uint64_t next_irq_cycle = timer_start + target * prescaler;
// Thread timer: sleep until next_irq_cycle, then set I_STAT bit
```

### CDROM : thread sector delivery

Le thread CDROM :
1. Attend le délai secteur (basé sur la vitesse 1x/2x)
2. Lit le secteur depuis l'image disque
3. Place les données dans le sector buffer (le CPU les lira via DMA3)
4. Set `I_STAT bit 2` (atomique)

Le CPU thread gère le DMA3 quand le game le programme (write CHCR).

### VBlank : thread GPU

Le thread VBlank :
1. Timer basé sur le refresh rate (50Hz PAL / 60Hz NTSC)
2. Set `I_STAT bit 0` (atomique)
3. Signal le swap de draw list (pour UE5 rendering)

### CPU loop simplifié

```cpp
for (;;) {
    execute_instruction();
    
    // Check IRQ (atomique, pas de lock)
    uint32_t pending = i_stat.load(relaxed) & i_mask;
    if (pending && interrupts_enabled()) {
        cop0[EPC] = pc;
        push_status();
        pc = 0x80000080;
    }
}
```

Plus de `tick()`, plus de `tick_peripherals()`, plus de fast path. Le CPU ne fait QUE exécuter des instructions et vérifier I_STAT.

## IRQs simultanées : comment le vrai hardware gère ça

Sur vrai PS1, les IRQ lines sont des **fils électriques séparés**. Deux périphériques peuvent passer HIGH au même instant (ex: VBlank + CDROM sector ready). Il n'y a pas de "lock" hardware entre eux.

Ce qui se passe :

1. **Les deux bits sont set dans I_STAT simultanément** — c'est du hardware, deux signaux indépendants qui latent chacun leur bit
2. **Le CPU finit son instruction courante** — il ne s'arrête jamais en plein milieu
3. **Il vérifie `(I_STAT & I_MASK) != 0`** → voit les deux bits set
4. **Il entre dans l'exception handler UNE SEULE FOIS** — jump à 0x80000080
5. **Le handler BIOS boucle sur tous les bits set** dans `(I_STAT & I_MASK)` : traite VBlank (clear bit 0), puis CDROM (clear bit 2), etc.
6. **`ReturnFromException`** — restaure PC et Status

Le "lock" naturel : **les interrupts sont automatiquement désactivées** quand le CPU entre dans l'exception (COP0 Status `IEc` passe à 0). Pendant que le handler tourne, aucune nouvelle exception ne peut fire. Si un nouveau bit I_STAT est set pendant le handler (par un thread périphérique), il sera vu et traité soit dans la boucle `while(I_STAT & I_MASK)` du handler actuel, soit au prochain check après `ReturnFromException`.

### Implication pour nos threads

Exactement le même principe :
- Deux threads font `i_stat.fetch_or(bit)` au même instant → les deux bits sont set
- Le CPU thread voit les deux bits, entre dans le handler une fois, traite tout
- Pendant le handler, les interrupts sont off (IEc=0) → pas de ré-entrance
- Si un thread set un nouveau bit pendant le handler → traité au prochain tour

**Pas de lock, pas de mutex, pas de condition variable.** Juste des atomic `fetch_or` sur I_STAT. C'est exactement comme le hardware : des fils qui passent HIGH indépendamment.

## Avantages

1. **Fidélité** : les IRQs arrivent au bon moment réel, indépendamment du CPU
2. **Pas de divergence CLI/UE5** : même code, mêmes threads, même timing
3. **Simplicité** : plus de tick batching, plus de fast path vs full path
4. **Performance** : le CPU loop est minimal, les périphériques tournent en parallèle
5. **Extensibilité** : ajouter un nouveau périphérique = ajouter un thread

## Risques / Points d'attention

1. **MMIO reads** : quand le CPU lit un registre timer/CDROM/GPU, il faut retourner la valeur correcte au moment exact. Pour les timers c'est un calcul. Pour CDROM status c'est un atomic read.
2. **DMA** : les transferts DMA accèdent à la RAM. Si le CDROM thread prépare les données et le CPU thread fait le DMA, il faut que les données soient prêtes (memory barrier).
3. **Granularité OS** : les timers OS (sleep, waitable timer) ont une granularité de ~1ms. Pour des events à <1ms (SIO0 transfer = ~30µs), il faut du busy-wait ou un high-resolution timer.
4. **Ordre des IRQs** : si deux IRQs fire "en même temps", l'ordre peut varier. Le BIOS exception handler gère ça (boucle sur tous les bits I_STAT).

## Comparaison avec les autres émulateurs

### DuckStation (et la plupart des émulateurs PS1/N64/etc.)
- **Event scheduler** : tous les périphériques programment des events à des cycles futurs
- Un seul thread exécute tout séquentiellement : CPU → check events → fire callbacks
- Avantage : simple, déterministe, pas de synchronisation
- Inconvénient : tout est séquentiel, les events ne fire pas au "vrai" moment mais quand le scheduler les check

### Notre approche : IRQ threads
- **Chaque source IRQ tourne sur son propre thread** avec son propre timing réel
- Le CPU thread ne fait QUE exécuter des instructions et vérifier I_STAT
- Les IRQ arrivent de manière **réellement asynchrone** comme sur vrai hardware
- `I_STAT` est le point de rendez-vous — atomic fetch_or, pas de locks

### Pourquoi c'est mieux

| Aspect | Event scheduler | IRQ threads |
|--------|----------------|-------------|
| Fidélité temporelle | Events fire au prochain check CPU | Events fire au bon moment réel |
| Parallélisme | Séquentiel (1 thread) | Vrai parallélisme hardware |
| Synchronisation | Aucune (single thread) | Atomic I_STAT (zero-lock) |
| Complexité code | Scheduler central + priority queue | Un thread loop simple par source |
| UE5 intégration | Besoin de tick_peripherals batching | Threads natifs, pas de batching |
| Déterminisme | Parfait (même seed = même résultat) | Dépend du scheduling OS |

Le déterminisme n'est pas un problème pour nous : le vrai PS1 n'est pas déterministe non plus (le timing exact dépend de la mécanique du drive CD, de la température du cristal, etc.). Notre modèle est plus fidèle au hardware réel que le scheduler déterministe.

## Migration

Phase 1 : VBlank thread (déjà partiellement fait via `fire_vblank_external`)
Phase 2 : CDROM sector delivery thread
Phase 3 : Timer IRQ (calcul à la lecture + programmation IRQ)
Phase 4 : SIO0 thread
Phase 5 : Supprimer tick()/tick_peripherals()
