# DevPSX Test Programs

This folder contains small native PlayStation test programs used by R3000-Emu.
They are built as standalone `ps-exe` files and can be loaded directly by the
CLI emulator with `--load`.

## SDK / Toolchain Location

The build setup intentionally reuses the existing local PSX SDK tree:

```text
E:/Projects/PSX/nolibgs_hello_worlds
```

`common.mk` pulls from that tree:

- `thirdparty/nugget/common.mk` for the MIPS compiler/linker rules.
- `thirdparty/nugget/common/crt0/crt0.s` for startup.
- `thirdparty/nugget/common/syscalls/printf.s` for printf syscall support.
- `psyq/include` for PsyQ headers.
- `psyq/lib` for PsyQ libraries.

The compiler expected by Nugget is `mipsel-none-elf-gcc`, available from the
same PSX development environment already used by the existing demos.

## Project Layout

Each test program lives in its own subfolder:

```text
devpsx/
  common.mk
  physics_ball/
  perf_bench/
  blend_test/
  doodle_jump/
```

Minimal `Makefile` for a new program:

```make
TARGET = my_test

SRCS = my_test.c \

include ../common.mk
```

Build from the program folder:

```bat
make
```

Generated files are ignored by `devpsx/.gitignore`:

- `*.o`
- `*.elf`
- `*.map`
- `*.dep`
- `*.ps-exe`

Commit source files, `Makefile`, and docs only unless an artifact is explicitly
needed for a release/debug handoff.

## Running In R3000-Emu

From the repository root:

```bat
lib\Debug\r3000_emu.exe --load="E:\Projects\github\Live\R3000-Emu\devpsx\doodle_jump\doodle_jump.ps-exe" --emu-log-level=warn
```

For bounded smoke tests, prefer adding either a timeout or a max-step option if
the current emulator build supports it:

```bat
lib\Debug\r3000_emu.exe --load="E:\Projects\github\Live\R3000-Emu\devpsx\doodle_jump\doodle_jump.ps-exe" --timeout-ms=3000 --emu-log-level=warn
```

Note: some emulator builds keep running past the requested timeout while the
game itself is alive. If the command prints `R3000 run start` and reaches VBlank
logs, the PS-EXE has at least booted.

## Doodle Jump MCP Demo

`devpsx/doodle_jump` is a small game specifically designed for LLM control via
the emulator MCP vector tools.

Design constraints:

- No textures required.
- Player is a cyan `POLY_F4` quad.
- Platforms are green `POLY_F4` quads.
- Bottom danger line is a red `POLY_F4` quad.
- Decorative stars are tiny dark-blue quads and should be ignored by agents.
- HUD text is for humans; the LLM should reason from primitive geometry.

Controls:

- D-pad left/right steers the player.
- Cross restarts after falling.
- Start restarts anytime.

Suggested LLM/MCP loop:

1. Start the emulator paused or step-controlled with `doodle_jump.ps-exe`.
2. Request a 2D/vector scene snapshot with HUD primitives included if needed.
3. Identify the cyan player quad.
4. Identify the nearest reachable green platform below or slightly above the
   player.
5. Hold left/right for several frames to align player center with platform
   center.
6. Observe again after a short frame window and update the decision.

For this demo, continuous control is more important than tap control. The MCP
should provide either:

- `hold_pad_named_buttons` plus `release_pad`, or
- a `step_with_pad_observation` mode that keeps the button held during the
  observation window.

This makes Doodle Jump a better stress test than Flappy Bird: the LLM must read
geometry, predict horizontal motion, and correct over multiple frames.

## Notes For Future Agents

- Keep programs simple and deterministic when they are meant for emulator/MCP
  validation.
- Prefer flat quads with stable colors for LLM-visible objects.
- Avoid relying on raster screenshots; these demos are for vector observation.
- If a new demo is intended for autonomous play, document the object-color
  contract next to the source.
- Do not hardcode external SDK paths in individual demos; update `common.mk`
  if the SDK location changes.
