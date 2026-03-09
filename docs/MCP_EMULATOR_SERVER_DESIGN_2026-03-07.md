# Emulator MCP Server Design

Date: 2026-03-07

## Goal

Add a reusable MCP server layer to the emulator now, first wired into the CLI, but designed so the same server/backend split can be reused from UE5 later.

The point is not to make the CLI "own" the whole analysis workflow.

The point is to expose the emulator as a controllable analysis endpoint that can work together with:

- a future UE5 MCP endpoint,
- Ghidra MCP,
- and an LLM orchestrating both sides.

## Why this matters

The project is moving toward an offline-first authoring workflow:

- static analysis in Ghidra,
- runtime analysis in the emulator,
- optional visual validation in UE5,
- synthesis into `.psx3dprof`.

For that workflow to be robust, the emulator needs a structured machine interface.

CLI is the right first integration point because:

- it is easy to automate,
- it already exposes the core runtime,
- it is the right place for long captures, hooks, and trace-heavy analysis.

## Architectural choice

The server was not added directly as "CLI glue".

Instead, the implementation is split in two layers:

### 1. `emu::IMcpBackend`

This is the frontend-agnostic control surface.

It defines what a host can do:

- query emulator status,
- query CPU state,
- step execution,
- manage CPU PC breakpoints,
- run until a breakpoint hit,
- configure deferred GTE trace windows,
- read RAM,
- switch PSX3D mode,
- request analysis refresh.

This is the compatibility seam that allows a later UE5 integration without rewriting the MCP protocol layer.

### 2. `emu::McpServer`

This is the protocol/runtime layer:

- stdio transport
- JSON-RPC / MCP request handling
- tool discovery
- tool execution dispatch

It does not know whether it is running in CLI or UE5.

It only depends on `IMcpBackend`.

## Files added

Core reusable server:

- `src/emu/mcp_server.h`
- `src/emu/mcp_server.cpp`

CLI integration:

- `cli/main.cpp`

Supporting accessors:

- `src/emu/core.h`
- `src/r3000/cpu.h`

Build integration:

- `CMakeLists.txt`

## Transport

The initial implementation uses stdio with `Content-Length` framing.

That choice was made deliberately:

- it is compatible with standard MCP/LSP-style clients,
- it avoids inventing a custom transport,
- it keeps the first version simple and local.

Important constraint:

- in MCP mode, stdout is reserved for protocol messages,
- so CLI logs must stay off stdout.

That is why the CLI logger is redirected away from stdout when `--mcp-stdio` is active.

## Current CLI flag

The first entry point is:

- `--mcp-stdio`

Example:

```powershell
.\lib\Debug\r3000_emu.exe --bios=bios/ps1_bios.bin --mcp-stdio
```

In practice the flag can be combined with other runtime setup arguments:

- `--cd=...`
- `--load=...`
- `--psx3d-analysis=1`
- `--psx3d-mode=analysis`
- etc.

So the host can choose the boot/setup path, then switch into MCP control.

## Current tool surface

The first implementation intentionally stays small.

Exposed tools:

### `emu.ping`

Checks that the endpoint is alive.

### `emu.get_status`

Returns high-level runtime state:

- core/cpu availability
- PSX3D analysis enabled
- PSX3D mode
- profile path / profile game id
- latest shadow GPU counters

### `emu.get_cpu_state`

Returns a CPU snapshot:

- `pc`
- `hi`
- `lo`
- `gpr[32]`

### `emu.step`

Executes one or more CPU steps.

This is enough to start doing scriptable breakpoint-style exploration from an external controller.

### `emu.list_breakpoints`

Lists the CPU PC breakpoints managed by the MCP backend.

### `emu.set_breakpoint_pc`

Adds a CPU PC breakpoint in the emulator backend.

### `emu.clear_breakpoint_pc`

Removes a single CPU PC breakpoint.

### `emu.clear_all_breakpoints`

Clears all CPU PC breakpoints.

### `emu.run_until_breakpoint`

Runs the CPU until:

- a managed breakpoint is hit,
- or a step budget is exhausted.

This is the first practical replacement for ad-hoc CLI-only stop logic when driving the emulator from MCP.

### `emu.set_gte_trace_window`

Configures GTE trace capture by:

- CPU PC range,
- optional start frame,
- optional end frame,
- enable/disable state.

This is the practical tool needed for cases like Ridge Racer where:

- the interesting geometry is produced much later than boot,
- tracing everything is wasteful,
- and the goal is to isolate the exact runtime GTE path for a known visual window.

### `emu.read_ram_u32`

Reads a physical 32-bit RAM value.

Useful for:

- validating camera roots,
- checking object structures,
- correlating static addresses found in Ghidra with runtime values.

### `emu.set_psx3d_mode`

Switches between:

- `game`
- `analysis`

### `emu.request_psx3d_refresh`

Queues an explicit PSX3D analysis refresh:

- `reason`
- `scope`

This is an important bridge between a future orchestration layer and the profile-learning system already present in the emulator.

## What this server is and is not

### It is

- a controllable emulator endpoint,
- the runtime-side half of an authoring pipeline,
- a reusable service layer that can later be hosted by UE5 too.

### It is not

- a direct Ghidra integration,
- a replacement for Ghidra MCP,
- a full remote-debugger yet,
- a final production protocol surface.

The intended usage is:

- `Ghidra MCP` for static code understanding
- `Emulator MCP` for runtime execution/state
- `UE5 MCP` later for visual acquisition/validation
- `LLM` to correlate all of that into better profiles and tooling decisions

## Why the backend split matters for UE5

The same protocol layer can later be reused from UE5 if UE5 provides another `IMcpBackend` implementation.

That means:

- same tool names,
- same request shapes,
- same orchestration logic,
- different host/runtime constraints.

This keeps the analysis ecosystem coherent instead of creating:

- one ad-hoc CLI API,
- another ad-hoc UE5 API,
- and no stable way to coordinate them.

## Immediate value for Ghidra-driven analysis

Even in this first version, the MCP server is already useful alongside Ghidra:

1. Find candidate function or address in Ghidra
2. Query emulator state / RAM / PC through MCP
3. Step execution or switch PSX3D mode
4. Request targeted analysis refresh
5. Compare runtime evidence with static structure

That is exactly the workflow the project needs.

## Known limits of the first version

This is a bootstrap implementation.

Current limits:

- minimal JSON extraction instead of a full JSON library
- no symbol or disassembly tools
- no direct Ghidra annotation bridge
- no session persistence layer
- no UE5 backend yet

These are acceptable limits for version 1 because the goal is to establish:

- protocol shape
- host abstraction
- control flow
- orchestration feasibility

## Recommended next additions

Priority additions for the next iterations:

### Runtime control

- `emu.run_until_pc`
- `emu.run_until_vblank`
- `emu.get_last_dma2_nohint_summary`

### Inspection

- `emu.read_ram_bytes`
- `emu.read_cop0`
- `emu.get_gpu3d_frame_stats`
- `emu.get_camera_candidates`

### Profiling / PSX3D

- `emu.get_psx3d_profile_status`
- `emu.save_psx3d_profile`
- `emu.load_psx3d_profile`
- `emu.get_hotspots`

### Cross-tool workflow support

- `emu.add_note`
- `emu.export_analysis_snapshot`

The last category is especially useful if the LLM is going to orchestrate emulator + Ghidra together.

## Recommended Ghidra-side pairing

This emulator MCP should be paired with a Ghidra workflow that can:

- rename functions,
- add labels,
- add comments,
- add bookmarks,
- and preserve findings durably.

That way:

- runtime discoveries are not ephemeral,
- static knowledge improves over time,
- and the profile pipeline becomes cumulative.

## Practical conclusion

The first MCP step should not try to solve everything.

It should establish a clean axis:

- emulator as reusable MCP service,
- frontend-specific backends,
- CLI first,
- UE5 later,
- Ghidra alongside,
- profile generation as the actual endgame.

That axis is now in place.
