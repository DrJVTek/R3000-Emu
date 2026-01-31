# R3000-Emu

Émulateur **R3000 (MIPS I / PS1)** en **ligne de commande**, orienté **pédagogie live** (logs, trace, code commenté).

## BMAD / b-mad (pour le live)

Le repo embarque aussi l’outillage **BMAD** (dossiers `/_bmad/`, `/.cursor/commands/`, `/.claude/commands/`) afin de montrer en live
comment on s’organise / exécute des workflows.

## Build (Windows / Visual Studio)

```powershell
cmake -S . -B build
cmake --build build -j
```

## Run

### Booter le BIOS PS1 (expérimental)

Par défaut, l’émulateur cherche un BIOS ici: `bios/ps1_bios.bin` (ou `bios/bios.bin`).  
Le BIOS est mappé en ROM à `0x1FC00000` (alias `0xBFC00000` au reset).  
Le bus implémente un sous-ensemble **minimal mais correct** des registres MMIO requis par le BIOS (pas de “stub 0” silencieux).  
Le hardware PS1 est encore incomplet (GPU/SPU/CDROM/DMA/IRQ partiels), donc selon le BIOS/jeu on peut encore voir des boucles (polling) tant que
certains registres/IRQ ne sont pas couverts.

```powershell
.\build\Debug\r3000_emu.exe --pretty --max-steps=200000
```

### Debug BIOS (preset)

Un preset pratique pour suivre le boot BIOS sans retaper plein d’options :

- active `--pretty`
- met `--log-level=debug`
- met `--log-cats=exec,exc,mem`
- active `--stop-on-bios-to-ram-nop`

Note: `--debug-bios` est **diagnostic** (logs/trace/stops) et ne doit pas modifier le comportement d’émulation.

```powershell
.\build\Debug\r3000_emu.exe --debug-bios --cd="E:\Projects\PSX\roms\Ridge Racer (U).cue" --max-steps=200000
```

### Charger un binaire externe (ELF / PS-X EXE)

```powershell
.\build\Debug\r3000_emu.exe --pretty --load=path\to\hello.elf --format=elf
```

- `--pretty` : trace lisible (1 ligne par instruction).
- `--log-level=trace|debug|info|warn|error`
- `--log-cats=fetch,decode,exec,mem,exc,all`
- `--max-steps=N` : stoppe après N instructions (utile pour BIOS/loops).
- `--bios=...` : boote le BIOS PS1 (reset PC=0xBFC00000).
- `--cd=...` : insère une image CD.
  - Support **2048 bytes/secteur**: `.iso` (data)
  - Support **2352 bytes/secteur**: `.bin/.img` RAW (**requis** pour CD-XA / Mode2)
  - Support `.cue` (BIN/CUE multi-tracks) : parse minimal `FILE/TRACK/INDEX 01`
  - Limites actuelles: pas d’audio/subchannel (tracks audio ignorées côté “lecture data”)
- `--gpu-dump=...` : dump binaire des writes GPU (GP0/GP1) pour intégration future (ex: Unreal).
  - Format: répétition de paires `[u32 port][u32 value]` avec `port=0` (GP0) ou `port=1` (GP1).
- `--load=...` : charge un binaire dans la RAM et exécute.
- `--format=auto|elf|psxexe`
- `--compare-duckstation` : écrit une trace parseable aux PCs des boucles debug dans `logs/compare_r3000.txt` (comparaison avec DuckStation).

### Comparaison avec DuckStation (debug)

Pour comparer l’état CPU/MMIO de R3000-Emu avec DuckStation aux mêmes PCs :

1. Lancer R3000-Emu avec le même BIOS + CD (ou sans CD) et `--compare-duckstation --max-steps=...` : génère `logs/compare_r3000.txt`.
2. Lancer DuckStation avec le **même** BIOS (ex. SCPH-7502 EU) et le même CD. Ouvrir le debugger, mettre des breakpoints aux PCs : `0x8005EE80` `0x8005EF30` `0x8005DE24` `0x8005E520` `0x8006797C` `0x80067938`.
3. À chaque break, recopier registres (pc, v0, a0–a3, sp, ra, gp, status, cause, etc.) et MMIO (I_STAT, I_MASK, DPCR, DICR, GPUSTAT, DMA, TMR selon le bloc) dans `logs/compare_duckstation_ref.txt`, même format que `compare_r3000.txt` (blocs `[PC=0x...]`, lignes `key=0xXXXXXXXX`). Voir `scripts/compare_duckstation_ref.template.txt`.
4. Lancer le script de diff :

```powershell
.\scripts\compare-duckstation.ps1
```

Par défaut : `logs/compare_r3000.txt` vs `logs/compare_duckstation_ref.txt`. Options : `-R3000Path <path>` et `-RefPath <path>`.

## Structure

- `src/r3000/` : CPU R3000 + bus mémoire (CLI/trace).
- `scripts/` : `compare-duckstation.ps1` pour diff R3000 trace vs DuckStation reference.
- `src/gte/` : module GTE (COP2) séparé du CPU (work in progress).
- `src/log/` : logger “C+” (pas de streams).
- `src/loader/` : loaders (ELF32 MIPS, PS-X EXE minimal).
- `integrations/ue5/` : intégration Unreal Engine 5 (plugin) + docs.

## Toolchain R3000 / PS1 (Windows)

Sur ce PC, `mipsel-none-elf-gcc` est déjà présent (toolchain MIPS little-endian).

### Installer `mipsel-none-elf-gcc` (si pas déjà installé)

#### Option A (recommandée) : MSYS2

- Installer MSYS2 : `https://www.msys2.org/`
- Ouvrir **“MSYS2 UCRT64”** puis exécuter :

```bash
pacman -Syu --noconfirm
pacman -S --noconfirm mingw-w64-ucrt-x86_64-mips-elf-gcc
```

- Vérifier :

```bash
mipsel-none-elf-gcc --version
```

- Si la commande n’est pas trouvée dans PowerShell/Windows Terminal, ajouter au `PATH` :
  - `C:\msys64\ucrt64\bin`

#### Option B : toolchain “standalone” (ZIP)

- Télécharger une toolchain précompilée qui fournit `mipsel-none-elf-gcc.exe`
- Dézipper, puis ajouter au `PATH` le dossier `bin` (celui qui contient `mipsel-none-elf-gcc.exe`)
- Vérifier :

```powershell
mipsel-none-elf-gcc --version
```

- **Vérifier**:

```powershell
mipsel-none-elf-gcc --version
```

- **Compiler un hello (ELF)**:

```powershell
cd examples\hello
.\build.ps1
```

- **Exécuter dans l’émulateur**:

```powershell
cd ..\..
.\build\Debug\r3000_emu.exe --pretty --load=examples\hello\build\hello.elf --format=elf
```

Le `hello` utilise les **host syscalls** (SYSCALL) pour imprimer dans la console:
`print_u32 / putc / print_cstr`.


## Slides (PowerPoint)

Le PowerPoint de la partie 1 (spec) du live **PSXVR** est dans le repo :
- [psxvr 1.pptx](psxvr%201.pptx)

