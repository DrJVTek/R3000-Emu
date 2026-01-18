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

### Démo (mini-ROM intégrée)

```powershell
.\build\Debug\r3000_emu.exe --pretty
```

- **Binaire externe (ELF / PS-X EXE)**:

```powershell
.\build\Debug\r3000_emu.exe --pretty --load=path\to\hello.elf --format=elf
```

- `--pretty` : trace lisible (1 ligne par instruction).
- `--log-level=trace|debug|info|warn|error`
- `--log-cats=fetch,decode,exec,mem,exc,all`
- `--load=...` : charge un binaire externe dans la RAM et exécute.
- `--format=auto|elf|psxexe`

## Structure

- `src/r3000/` : CPU R3000 + bus mémoire (CLI/trace).
- `src/gte/` : module GTE (COP2) séparé du CPU (work in progress).
- `src/log/` : logger “C+” (pas de streams).
- `src/loader/` : loaders (ELF32 MIPS, PS-X EXE minimal).

## Toolchain R3000 / PS1 (Windows)

Sur ce PC, `mipsel-none-elf-gcc` est déjà présent (toolchain MIPS little-endian).

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

