"""Phase 3b — GTE trap: widen GTE trace window and inject pad inputs to trigger 3D.

When the seek loop exhausts its pass budget with readiness still low (e.g. long 2D
intros), this phase activates passive GTE PC tracing over full PSX RAM and advances
the game in frame chunks, cycling through button presses.  The goal is to discover
which PCs execute GTE opcodes before the main 3D scene becomes visible, then
correlate them with Ghidra function names for the LLM context.
"""

from __future__ import annotations

from typing import Any

from .. import log as log_mod
from ..mcp.ghidra_http import GhidraHttpClient, GhidraHttpError
from .p1_boot import advance_runtime


PSX_RAM_START = 0x80000000
PSX_RAM_END   = 0x801FFFFF


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
    """Return PC integers from get_gte_trace_summary, sorted by hit count desc."""
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
    if not ghidra or not pcs:
        return []
    try:
        functions = ghidra.list_functions_parsed()
    except GhidraHttpError:
        return []
    if not functions:
        return []

    # Build sorted (addr_int, name, address_str) list for nearest-function lookup.
    # Only include PSX RAM range — Ghidra may also list BIOS/stub addresses.
    parsed: list[tuple[int, str, str]] = []
    for fn in functions:
        addr_int = _as_int(fn.get("address", ""))
        if PSX_RAM_START <= addr_int <= PSX_RAM_END:
            parsed.append((addr_int, fn.get("name", ""), fn.get("address", "")))
    parsed.sort(key=lambda x: x[0])

    results: list[dict[str, str]] = []
    seen: set[int] = set()
    for pc in pcs:
        # Find the latest function whose start ≤ pc (containing function).
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


def run(ctx: dict) -> dict:
    """
    Widen GTE trace window to full PSX RAM, advance in frame chunks with pad
    input injection, and collect the PCs that executed GTE opcodes.

    Returns dict with keys: discovered_pcs, top_ops, frames_done,
    ghidra_functions, triggered.
    """
    cfg = ctx["config"]
    emu = ctx["emu"]
    w = cfg.workflow
    ghidra: GhidraHttpClient | None = ctx.get("ghidra")

    buttons = [b.strip() for b in (w.gte_trap_buttons or "").split(",") if b.strip()]
    max_frames = max(1, int(w.gte_trap_max_frames))
    frame_chunk = max(1, int(w.gte_trap_frame_chunk))
    max_pcs = max(1, int(w.gte_trap_max_pcs))

    # 1. Open wide GTE trace window over all PSX RAM.
    try:
        emu.call_tool_json("emu.set_gte_trace_window", {
            "pc_start": PSX_RAM_START,
            "pc_end": PSX_RAM_END,
            "start_frame": 0,
            "end_frame": 0,
            "enabled": True,
        })
    except Exception as exc:
        log_mod.log("p3b_gte_trap", "set_window_failed", error=str(exc))
        return {
            "discovered_pcs": [],
            "top_ops": [],
            "frames_done": 0,
            "ghidra_functions": [],
            "triggered": False,
            "error": str(exc),
        }

    discovered_pcs: list[int] = []
    frames_done = 0
    summary: dict[str, Any] = {}

    # 2. Advance in frame chunks, cycling through buttons, sampling after each chunk.
    while frames_done < max_frames:
        chunk_idx = frames_done // frame_chunk
        button = buttons[chunk_idx % len(buttons)] if buttons else ""

        # Press button with minimal observe steps — effect registered, not blocking.
        if button:
            try:
                emu.call_tool_json("emu.step_with_pad_observation", {
                    "names_csv": button,
                    "hold_steps": 1,
                    "observe_steps": 100,
                    "max_groups": 12,
                    "max_targets": 6,
                })
            except Exception as exc:
                log_mod.log("p3b_gte_trap", "button_failed", button=button, error=str(exc))

        # Advance frame_chunk full frames.
        try:
            advance_runtime(
                emu=emu,
                max_steps=2_000_000 * frame_chunk,
                max_frames=frame_chunk,
            )
        except Exception as exc:
            log_mod.log("p3b_gte_trap", "advance_failed", frames_done=frames_done, error=str(exc))
            break

        frames_done += frame_chunk

        # 3. Sample GTE trace — top_pcs are decimal integers (uint32 PSX addresses).
        try:
            summary = emu.call_tool_json("emu.get_gte_trace_summary") or {}
        except Exception as exc:
            log_mod.log("p3b_gte_trap", "summary_failed", frames_done=frames_done, error=str(exc))
            continue

        top_pcs = _extract_top_pcs(summary)
        psx_pcs = [p for p in top_pcs if PSX_RAM_START <= p <= PSX_RAM_END]

        log_mod.log(
            "p3b_gte_trap",
            "chunk_done",
            frames_done=frames_done,
            button=button,
            psx_pcs_found=len(psx_pcs),
            top_pcs_raw=len(top_pcs),
        )

        if psx_pcs:
            discovered_pcs = psx_pcs[:max_pcs]
            break

    ghidra_functions = _correlate_ghidra(ghidra, discovered_pcs)

    result: dict[str, Any] = {
        "discovered_pcs": [f"0x{p:08X}" for p in discovered_pcs],
        "top_ops": summary.get("top_ops", []),
        "frames_done": frames_done,
        "ghidra_functions": ghidra_functions,
        "triggered": len(discovered_pcs) > 0,
    }
    log_mod.log(
        "p3b_gte_trap",
        "done",
        triggered=result["triggered"],
        discovered_pcs=result["discovered_pcs"],
        frames_done=frames_done,
        ghidra_functions_count=len(ghidra_functions),
    )
    return result
