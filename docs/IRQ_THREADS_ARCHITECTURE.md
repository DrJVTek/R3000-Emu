# IRQ Threads Architecture

## Motivation

L'émulateur actuel tick tous les périphériques séquentiellement dans `Bus::tick()` — soit à chaque instruction CPU (full path), soit batché via `tick_peripherals()`. Ce modèle cause des divergences de timing entre CLI et UE5 car le batching change le moment exact où les IRQs fire.

Sur vrai hardware PS1, chaque périphérique a son propre oscillateur/horloge et fire des IRQs de manière **asynchrone** au CPU. Le CPU se fait interrompre quand ça arrive.

## Architecture

### Principe
- **Thread principal (CPU)** : exécute les instructions MIPS, le seul à exécuter du code game
- **Threads hardware** : chaque composant hardware tourne sur son propre thread avec son propre timing
- `I_STAT` (atomic) est le point de rendez-vous : chaque thread set son bit via `fetch_or`
- Le CPU vérifie `I_STAT` après chaque instruction → exception si pending

### Threads hardware

| Thread | Source | Timing | I_STAT bit | Status |
|--------|--------|--------|------------|--------|
| GPU scanline | Cristal GPU | ~63.5µs/scanline | bit 0 (VBlank), HBlank signal | ✅ Implémenté |
| CDROM sector | Moteur drive | ~6.67ms (2x) / ~13.3ms (1x) | bit 2 | ✅ Implémenté |
| Timer 0 | Reprogrammable (sysclk ou dotclock) | Dépend de la config game | bit 4 | 🔲 À faire |
| Timer 1 | Reprogrammable (sysclk ou HBlank) | Dépend de la config game | bit 5 | ✅ HBlank dans GPU thread |
| Timer 2 | Reprogrammable (sysclk ou sysclk/8) | Dépend de la config game | bit 6 | 🔲 À faire |
| SIO0 | Transfer complete | BAUD * 8 ticks | bit 7 | 🔲 À faire |
| DMA mémoire | Bus transfer | Variable | bit 3 | 🔲 À faire |

### Timers user (Timer 0/1/2) — reprogrammables

Les timers PS1 sont des compteurs hardware **indépendants** du CPU. Le game peut configurer à tout moment :
- Source clock (sysclk, dotclock, sysclk/8, HBlank)
- Target value (0-0xFFFF)
- Reset on target, IRQ on target, IRQ on overflow
- Mode pulse/toggle, one-shot/repeat
- Sync enable + sync mode (gate)

**Important** : le BIOS PS1 a un scheduler multi-thread (TCB — Thread Control Block). Un thread game peut modifier la config d'un timer pendant qu'un autre thread tourne. Les timers doivent donc être **thread-safe** (tous les champs atomiques).

Chaque timer a son propre thread :
1. Quand le game écrit Timer mode/target → le thread recalcule son prochain fire time
2. Le thread dort jusqu'au prochain event (target hit ou overflow)
3. Au réveil → `fire_irq_external(4+ch)`
4. Le thread recalcule le prochain fire time et se rendort

Pour la lecture du count (MMIO read) : **calcul à la lecture** basé sur le temps écoulé depuis le dernier reset, pas de tick per-cycle.

```cpp
uint16_t timer_read_count(int ch) {
    auto elapsed = steady_clock::now() - timer_start_time[ch];
    uint64_t ticks = elapsed / tick_period[ch]; // period dépend de la clock source
    return (uint16_t)(ticks % (target + 1));
}
```

### DMA — les canaux indépendants

Sur vrai PS1, le DMA controller a 7 canaux qui opèrent sur le bus **indépendamment** du CPU. Le CPU programme un canal (MADR, BCR, CHCR) et le DMA fait le transfert tout seul. Le CPU est même stallé pendant les transfers DMA (bus steal).

#### DMA mémoire (threads OK)
- **DMA6 OTC** : ordering table clear (RAM→RAM) → thread simple
- **DMA3 CDROM→RAM** : lit le FIFO CDROM, écrit en RAM → thread (FIFO atomic)
- **DMA0 MDEC IN** : RAM→MDEC decoder → thread possible

#### DMA avec état partagé UE5 (attention)
- **DMA2 GPU** : linked-list GPU commands → le GPU state est lu par UE5 render thread. **Ring buffer** entre le DMA thread et le render thread UE5.
- **DMA4 SPU** : RAM→SPU RAM → le SPU state est lu par le audio thread. Même approche ring buffer.
- **DMA1 MDEC OUT** : MDEC→RAM (decoded video) → écrit en RAM, lu par le VRAM display. Thread OK si le write est atomique.

#### Principe de sécurité
- Un DMA thread ne touche QUE la RAM (reads/writes atomiques sur des blocs)
- Pour les DMA vers GPU/SPU : le thread DMA écrit dans un **ring buffer intermédiaire**, le consumer UE5 (render/audio thread) lit quand il est prêt
- Pas de lock : producteur-consommateur lock-free

### SIO0 — transfer timing

Quand le game écrit JOY_DATA, le SIO0 controller commence un transfer qui prend `BAUD * 8` cycles CPU. Sur vrai hardware c'est le controller SIO qui fait le timing, pas le CPU.

Thread SIO0 :
1. Game écrit JOY_DATA → signal au thread SIO0
2. Thread dort pour la durée du transfer (BAUD * 8 / 33.87MHz)
3. Au réveil → met la réponse dans RX buffer, `fire_irq_external(7)`

### CPU loop simplifié

```cpp
for (;;) {
    execute_instruction();
    
    // Consume external IRQ bits (atomic, zero-lock)
    uint32_t ext = irq_ext_pending.exchange(0, acquire);
    if (ext) i_stat |= (ext & ~i_stat); // edge-trigger
    
    // Check IRQ
    if ((i_stat & i_mask) && interrupts_enabled()) {
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

## Thread safety des registres périphériques

Les jeux PS1 peuvent reconfigurer les périphériques à tout moment via MMIO writes. Le BIOS a un scheduler multi-thread (TCB) — plusieurs threads game peuvent tourner en parallèle.

### Règle : tous les registres partagés entre threads sont atomiques

- **Timer mode/target/count** : `std::atomic` — le CPU thread écrit (MMIO), les threads timer/GPU lisent
- **CDROM status/flags** : `std::atomic` — le CDROM thread écrit, le CPU thread lit (MMIO read)
- **DMA channel config** : `std::atomic` — le CPU thread écrit (CHCR), le DMA thread lit
- **I_STAT / I_MASK** : `std::atomic<uint32_t>` — écrit par tous les threads IRQ, lu par CPU

Pas de mutex, pas de lock. Les atomics suffisent car chaque thread a un rôle unique (producteur ou consommateur pour chaque registre). C'est le modèle hardware : des registres câblés, pas des structures de données logicielles.

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

### Fait
- ✅ Phase 1 : GPU scanline thread (VBlank + HBlank + Timer 1 ext clock)
- ✅ Phase 2 : CDROM sector delivery thread
- ✅ Infrastructure : `irq_ext_pending_` atomic register, `fire_irq_external(bit)`

### À faire
- 🔲 Phase 3 : Timer 0/1/2 threads reprogrammables (sleep + recalcul sur write mode/target)
- 🔲 Phase 4 : SIO0 transfer thread
- 🔲 Phase 5 : DMA threads (mémoire d'abord, puis GPU/SPU avec ring buffers)
- 🔲 Phase 6 : Supprimer tick()/tick_peripherals() — le CPU loop ne fait plus que execute + check I_STAT
