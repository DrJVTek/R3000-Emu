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

- `--pretty` : trace lisible (1 ligne par instruction).
- `--log-level=trace|debug|info|warn|error`
- `--log-cats=fetch,decode,exec,mem,exc,all`

## Structure

- `src/r3000/` : CPU R3000 + bus mémoire (CLI/trace).
- `src/gte/` : module GTE (COP2) séparé du CPU (work in progress).
- `src/log/` : logger “C+” (pas de streams).

