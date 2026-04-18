"""Phase 3c — BIOS GTE trap: trace GTE caller PCs inside the BIOS ROM.

The PlayStation BIOS renders the Sony/PlayStation logo using GTE (RTPS/NCLIP).
This phase activates the GTE trace window over the BIOS ROM range and advances
the emulator with steps only (max_frames=0, no VBlank wait) so it works cleanly
at cold BIOS start before the GPU is fully initialized.

When GTE-caller PCs are found at BIOS addresses (0xBFC00000-0xBFC80000), they
can be correlated with a BIOS binary loaded in Ghidra to identify the rendering
functions.

Trigger condition (orchestrator): PC is in BIOS range after p1_boot.
"""

from __future__ import annotations

from typing import Any

from .. import log as log_mod
from ..mcp.ghidra_http import GhidraHttpClient, GhidraHttpError


BIOS_START = 0xBFC00000
BIOS_END   = 0xBFC80000


def _as_int(value: Any) -> int:
    try:
        if isinstance(value, int):
            return value
        s = str(value).strip()
        try:
            return int(s, 0)
        except ValueError:
            return int(s, 16)
    except Exception:
        return 0


def _extract_top_pcs(summary: dict[str, Any]) -> list[int]:
    """Return PC integers from get_gte_trace_summary top_pcs, sorted by count desc."""
    top_pcs = summary.get("top_pcs") or []
    if not isinstance(top_pcs, list):
        return []
    pairs: list[tuple[int, int]] = []
    for item in top_pcs:
        if isinstance(item, dict):
            pc = _as_int(item.get("pc") or item.get("address") or 0)
            count = int(item.get("count", 0))
            if pc:
                pairs.append((pc, count))
        elif isinstance(item, (int, str)):
            pc = _as_int(item)
            if pc:
                pairs.append((pc, 1))
    pairs.sort(key=lambda x: -x[1])
    return [p for p, _ in pairs]


def _correlate_ghidra(
    ghidra: GhidraHttpClient | None,
    pcs: list[int],
) -> list[dict[str, str]]:
    """Map discovered BIOS PCs to Ghidra function names (BIOS must be loaded)."""
    if not ghidra or not pcs:
        return []
    try:
        functions = ghidra.list_functions_parsed()
    except GhidraHttpError:
        return []
    if not functions:
        return []

    # Include both BIOS ROM range and any mirrored/RAM range functions.
    parsed: list[tuple[int, str, str]] = []
    for fn in functions:
        addr_int = _as_int(fn.get("address", ""))
        if addr_int:
            parsed.append((addr_int, fn.get("name", ""), fn.get("address", "")))
    parsed.sort(key=lambda x: x[0])

    results: list[dict[str, str]] = []
    seen: set[int] = set()
    for pc in pcs:
        best: tuple[int, str, str] | None = None
        for entry in reversed(parsed):
            if entry[0] <= pc:
                best = entry
                break
        if best and best[0] not in seen:
            seen.add(best[0])
            results.append({
                "pc": f"0x{pc:08X}",
                "function": best[1],
                "function_addr": best[2],
            })
    return results


def _advance_steps_only(emu: Any, steps: int) -> dict[str, Any]:
    """Advance using steps only — max_frames=0 means no VBlank limit."""
    try:
        return emu.call_tool_json("emu.resume", {
            "max_steps": steps,
            "max_frames": 0,
            "stop_on_breakpoint": True,
        }) or {}
    except Exception as exc:
        log_mod.log("p3c_bios_gte_trap", "resume_failed", error=str(exc))
        return {"error": str(exc)}


def run(ctx: dict) -> dict:
    """
    Trace GTE caller PCs inside the BIOS ROM by advancing with steps only.

    Must be called while the PC is still in BIOS range (0xBFC00000-0xBFC80000).
    Uses max_frames=0 so there is no dependency on GPU VBlank initialization.

    Returns dict with: discovered_pcs, top_ops, steps_done, ghidra_functions,
    triggered.
    """
    cfg = ctx["config"]
    emu = ctx["emu"]
    w = cfg.workflow
    ghidra: GhidraHttpClient | None = ctx.get("ghidra")

    max_steps = max(1, int(w.bios_gte_trap_max_steps))
    step_chunk = max(1, int(w.bios_gte_trap_step_chunk))
    max_pcs = max(1, int(w.bios_gte_trap_max_pcs))

    # 1. Open GTE trace window over the entire BIOS ROM.
    try:
        emu.call_tool_json("emu.set_gte_trace_window", {
            "pc_start": BIOS_START,
            "pc_end": BIOS_END,
            "start_frame": 0,
            "end_frame": 0,
            "enabled": True,
        })
    except Exception as exc:
        log_mod.log("p3c_bios_gte_trap", "set_window_failed", error=str(exc))
        return {
            "discovered_pcs": [],
            "top_ops": [],
            "steps_done": 0,
            "ghidra_functions": [],
            "triggered": False,
            "error": str(exc),
        }

    discovered_pcs: list[int] = []
    steps_done = 0
    summary: dict[str, Any] = {}

    # 2. Advance in step chunks (no VBlank wait), sampling GTE trace after each.
    while steps_done < max_steps:
        chunk = min(step_chunk, max_steps - steps_done)
        result = _advance_steps_only(emu, chunk)
        steps_done += chunk

        if result.get("error"):
            break

        try:
            summary = emu.call_tool_json("emu.get_gte_trace_summary") or {}
        except Exception as exc:
            log_mod.log("p3c_bios_gte_trap", "summary_failed", steps_done=steps_done, error=str(exc))
            continue

        top_pcs = _extract_top_pcs(summary)
        bios_pcs = [p for p in top_pcs if BIOS_START <= p <= BIOS_END]

        log_mod.log(
            "p3c_bios_gte_trap",
            "chunk_done",
            steps_done=steps_done,
            bios_pcs_found=len(bios_pcs),
            top_pcs_total=len(top_pcs),
        )

        if bios_pcs:
            discovered_pcs = bios_pcs[:max_pcs]
            break

    ghidra_functions = _correlate_ghidra(ghidra, discovered_pcs)

    result_out: dict[str, Any] = {
        "discovered_pcs": [f"0x{p:08X}" for p in discovered_pcs],
        "top_ops": summary.get("top_ops", []),
        "steps_done": steps_done,
        "ghidra_functions": ghidra_functions,
        "triggered": len(discovered_pcs) > 0,
    }
    log_mod.log(
        "p3c_bios_gte_trap",
        "done",
        triggered=result_out["triggered"],
        discovered_pcs=result_out["discovered_pcs"],
        steps_done=steps_done,
        ghidra_functions_count=len(ghidra_functions),
    )
    return result_out
