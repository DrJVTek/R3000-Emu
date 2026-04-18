"""Phase 3c — BIOS GTE trap: trace GTE caller PCs inside the BIOS ROM.

The PlayStation BIOS renders the Sony/PlayStation logo using GTE (RTPS/NCLIP).
This phase activates the GTE trace with no PC filter (pc_start=0, pc_end=0 =
global capture) so it catches BIOS code regardless of whether it executes from
KSEG1 (0xBFC00000, uncached) or KSEG0 (0x9FC00000, cached). Python then filters
top_pcs for both BIOS mirrors.

Trigger condition (orchestrator): PC is in BIOS range after p1_boot.
"""

from __future__ import annotations

from typing import Any

from .. import log as log_mod


BIOS_KSEG0_START = 0x9FC00000
BIOS_KSEG0_END   = 0x9FC80000
BIOS_KSEG1_START = 0xBFC00000
BIOS_KSEG1_END   = 0xBFC80000


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


def _is_bios_pc(pc: int) -> bool:
    return (BIOS_KSEG0_START <= pc <= BIOS_KSEG0_END or
            BIOS_KSEG1_START <= pc <= BIOS_KSEG1_END)


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


def run(ctx: dict) -> dict:
    """
    Trace GTE caller PCs inside the BIOS ROM.

    3 MCP calls:
      1. set_gte_trace_window  — arm global GTE trace (pc_start=0, pc_end=0)
      2. emu.resume            — run the full step budget in one shot (no VBlank wait)
      3. get_gte_trace_summary — read top_pcs accumulated during the run

    Must be called while the PC is still in BIOS range (0xBFC00000-0xBFC80000).
    max_frames=0 means the resume stops only on max_steps (no VBlank dependency).
    pc_start=0/pc_end=0 means capture ALL GTE ops; Python filters for BIOS addresses.
    """
    cfg = ctx["config"]
    emu = ctx["emu"]
    w = cfg.workflow

    max_steps = max(1, int(w.bios_gte_trap_max_steps))
    max_pcs = max(1, int(w.bios_gte_trap_max_pcs))

    # 1. Arm global GTE trace (no PC range filter — catches both KSEG0 and KSEG1).
    try:
        emu.call_tool_json("emu.set_gte_trace_window", {
            "pc_start": 0,
            "pc_end": 0,
            "start_frame": 0,
            "end_frame": 0,
            "enabled": True,
        })
    except Exception as exc:
        log_mod.log("p3c_bios_gte_trap", "set_window_failed", error=str(exc))
        return {
            "discovered_pcs": [], "top_ops": [], "steps_done": 0,
            "triggered": False, "error": str(exc),
        }

    # 2. Single resume — GTE trace accumulates during execution.
    # The --stop-on-pc=0xBFC00000 breakpoint fires on the very first step when
    # pause_immediate mode is used (stopped_on_pc_ starts at 0).  The emu
    # returns a "step stopped kind=1" error for that one step, then clears the
    # internal flag so subsequent resumes proceed normally.  Retry once.
    resume_result: dict[str, Any] = {}
    try:
        resume_result = emu.call_tool_json("emu.resume", {
            "max_steps": max_steps,
            "max_frames": 0,
        }) or {}
    except Exception as exc:
        exc_str = str(exc)
        if "step stopped" in exc_str or "kind=1" in exc_str or "halted" in exc_str.lower():
            log_mod.log("p3c_bios_gte_trap", "stop_on_pc_cleared", note="retrying resume")
            try:
                resume_result = emu.call_tool_json("emu.resume", {
                    "max_steps": max_steps,
                    "max_frames": 0,
                }) or {}
            except Exception as exc2:
                log_mod.log("p3c_bios_gte_trap", "resume_failed", error=str(exc2))
        else:
            log_mod.log("p3c_bios_gte_trap", "resume_failed", error=exc_str)

    steps_done = int(resume_result.get("steps_done", 0) or 0)
    log_mod.log("p3c_bios_gte_trap", "resume_done", steps_done=steps_done)

    # 3. Read the GTE trace — top_pcs populated from the run above.
    summary: dict[str, Any] = {}
    try:
        summary = emu.call_tool_json("emu.get_gte_trace_summary") or {}
    except Exception as exc:
        log_mod.log("p3c_bios_gte_trap", "summary_failed", error=str(exc))

    top_pcs = _extract_top_pcs(summary)
    bios_pcs = [p for p in top_pcs if _is_bios_pc(p)]
    discovered_pcs = bios_pcs[:max_pcs]

    result_out: dict[str, Any] = {
        "discovered_pcs": [f"0x{p:08X}" for p in discovered_pcs],
        "top_ops": summary.get("top_ops", []),
        "steps_done": steps_done,
        "triggered": len(discovered_pcs) > 0,
    }
    log_mod.log(
        "p3c_bios_gte_trap", "done",
        triggered=result_out["triggered"],
        discovered_pcs=result_out["discovered_pcs"],
        steps_done=steps_done,
        all_top_pcs=[f"0x{p:08X}" for p in top_pcs],
    )
    return result_out
