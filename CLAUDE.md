# R3000-Emu Project Notes

> **⚠️ CLAUDE: LIRE `docs/DEBUG_UE5_STUCK.md` AU DÉBUT DE CHAQUE SESSION !**
>
> Ce fichier contient l'historique complet du debug UE5 et les infos critiques.
> **Préférence utilisateur: NON-HLE (bHleVectors=false)**

---

## Log Paths

### UE5 Project (PSXVR)
- **Logs**: `E:\Projects\github\Live\PSXVR\logs\`
  - `system.log` - Core emulator logs
  - `gpu.log` - GPU primitives, DRAWENV, display config
  - `cdrom.log` - CDROM activity
  - `spu.log` - SPU/audio logs
  - `io.log` - I/O operations
- **Project Root**: `E:\Projects\github\Live\PSXVR\`

### CLI Emulator
- **Logs**: `E:\Projects\github\Live\R3000-Emu\logs\`

### Reference (DuckStation)
- **Reference logs**: `E:\Projects\github\Live\R3000-Emu\docs\`
  - Contains DuckStation boot traces for comparison

## UE5 Integration

### GPU Debug
- **EmuLogLevel** (R3000EmuComponent): set to `trace` for full primitive logs (vertices, offset) in gpu.log
- **bDebugMeshLog** (R3000GpuComponent): log transform params, first 3 tris, bounds to UE Output Log (Verbose)

### Build Notes
- UE5 plugin uses symlink: `integrations/ue5/R3000Emu/Source/src` -> `src/`
- To force rebuild after core changes: `touch integrations/ue5/R3000Emu/Source/R3000EmuRuntime/Private/R3000Core_*.cpp`

## Test ROMs (`E:\Projects\PSX\roms\`)
- Ridge Racer (U): `Ridge Racer (U).cue`
- Moto Racer (EU): `Moto Racer (Europe) (En,Fr,De,Es,It,Sv).cue`
- Moto Racer 2 (JP): `Moto Racer 2 (Japan).cue`
- Moto Racer World Tour (EU): `Moto Racer World Tour (Europe) (En,Fr,De,Es,It,Sv).cue`
- hello_strplay: `E:\Projects\PSX\nolibgs_hello_worlds\hello_strplay\hello_strplay.cue`

## CLI Emulator (r3000_emu.exe)

### Paramètres:
```
--bios="<path>"     BIOS file (SCPH-7502 EU recommandé)
--cd="<path>"       CD image (.cue, .bin, .iso) ⚠️ PAS --iso !
--load="<path>"     Load PS-EXE directly
--timeout-ms=N      Stop after N milliseconds
--fast-boot         Skip BIOS, load game EXE from CD
--trace-io          Enable I/O tracing
```

### BIOS recommandé:
`E:\Projects\PSX\duckstation\bios\Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin`

### Exemple complet:
```bash
./build/Debug/r3000_emu.exe \
  --bios="E:/Projects/PSX/duckstation/bios/Sony PlayStation SCPH-7502 BIOS v4.1 (1997-12-16)(Sony)(EU).bin" \
  --cd="E:/Projects/PSX/roms/Ridge Racer (U).cue" \
  --timeout-ms=60000
```

---

## UE5 Configuration (R3000EmuComponent)

### Propriétés importantes:

| Property | Recommandé | Description |
|----------|------------|-------------|
| `bHleVectors` | **false** | NON-HLE préféré (BIOS réel gère exceptions) |
| `bThreadedMode` | true | Worker thread avec waitable timer |
| `BusTickBatch` | 1 | Cycle-accurate (avec threaded mode) |
| `CycleMultiplier` | 1 | Timing PS1 normal |
| `bFastBoot` | false | Boot BIOS complet |
| `EmuLogLevel` | "debug" | Logs composants (GPU, CD, SPU) |

### Thread-safety (2026-02-09):
- `copy_vram()` dans gpu.h pour copie VRAM thread-safe
- `PutcharCB` queue les lignes, broadcast sur game thread
- `FCriticalSection GpuStateLock_` protège l'état GPU

---

## État actuel (2026-02-09)

### ⚠️ NE PAS AJOUTER `deliver_events_for_class` POUR VBLANK/CDROM !

**RÉGRESSION CAUSÉE:** J'ai essayé d'ajouter:
- `deliver_events_for_class(0xF0000001)` pour VBlank
- `deliver_events_for_class(0xF0000003)` pour CDROM

**RÉSULTAT:** Logo PlayStation (280 tris) disparu! REVERT effectué.

**LEÇON:** Le BIOS exception handler (0x80000080) délivre DÉJÀ les événements.
Appeler `deliver_events_for_class` nous-mêmes = double-délivrance = corruption.

### Ce qui marche:
- ✅ Sony logo + PlayStation logo (280 tris)
- ✅ CDROM lecture fonctionne

### Ce qui bloque:
- ❌ Après PlayStation logo: 0 primitives, jeu bloqué en loading
- ❌ Galaga (mini-jeu) jamais affiché

### Analyse logs (après logo PlayStation):
```
CPU alterne entre:
- 0x8005699x = game code (VSync wait loop)
- 0x00001Exx = BIOS exception handler
VBlank #501 → #851 (continue)
i_stat=0x00000001 (VBlank) parfois visible puis cleared
```

**Symptôme:** VBlank IRQ arrive, BIOS handler tourne, MAIS le jeu ne sort jamais de sa boucle VSync.

### Questions à poser:
1. Quelle BIOS? (SCPH-????)
2. HLE mode fonctionne-t-il? (bHleVectors=true)
