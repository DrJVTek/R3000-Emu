"""Phase 1 — Boot/resume control on a single native stdio MCP session.

Launch modes:
- ``paused`` / ``pause_after_exe_load``: run until BIOS hands off to game code,
  then stop so static analysis can start from the loaded EXE/menu context.
- ``pause_immediate``: do not advance; stop right at session start.
- ``run_to_ram``: same runtime handoff detection, but mark the session as
  already resumed for the later phases.
"""

from __future__ import annotations

import time
from typing import Any, Optional

from .. import log as log_mod
from ..mcp.emu_stdio import EmuMcp, EmuMcpError


BIOS_PC_LOW = 0xBFC00000
BIOS_PC_HIGH = 0xBFC80000


class BootError(RuntimeError):
    """Raised when Phase 1 cannot reach a usable emu state."""


def _read_pc(emu: Any) -> int:
    state = emu.call_tool_json("emu.get_cpu_state")
    pc_raw = state.get("pc", "0x0")
    try:
        return int(pc_raw, 16) if isinstance(pc_raw, str) else int(pc_raw)
    except ValueError:
        return 0


def _read_boot_exe_info(emu: Any) -> dict[str, Any]:
    try:
        info = emu.call_tool_json("emu.get_boot_exe_info")
        return info if isinstance(info, dict) else {}
    except Exception:
        return {}


def _read_boot_exe_history(emu: Any) -> dict[str, Any]:
    try:
        info = emu.call_tool_json("emu.get_boot_exe_history")
        return info if isinstance(info, dict) else {}
    except Exception:
        return {}


def run(
    emu: EmuMcp,
    boot_timeout_s: int = 60,
    launch_mode: str = "run_to_ram",
    pause_pc: int = BIOS_PC_LOW,
    step_chunk: int = 50000,
) -> dict:
    """Execute phase 1 on an already-started native MCP emulator session."""
    launch_mode = (launch_mode or "run_to_ram").strip().lower()
    pause_immediate = launch_mode in ("pause_immediate", "pause-now", "pause_now", "boot-pause")
    pause_after_exe_load = launch_mode in ("paused", "pause", "static-first", "pause_after_exe_load", "pause-after-exe-load")
    log_mod.log("boot", "start", launch_mode=launch_mode)
    t_start = time.monotonic()
    final_pc = _read_pc(emu)
    if pause_immediate:
        boot_time_s = round(time.monotonic() - t_start, 2)
        log_mod.log(
            "boot",
            "paused_immediate",
            pc=f"0x{final_pc:08X}",
            boot_time_s=boot_time_s,
            pause_pc=f"0x{pause_pc:08X}",
        )
        return {
            "pid": None,
            "pc": f"0x{final_pc:08X}",
            "boot_time_s": boot_time_s,
            "reused": False,
            "paused": True,
            "launch_mode": "pause_immediate",
        }

    boot_exe_info = _read_boot_exe_info(emu)
    if pause_after_exe_load and boot_exe_info.get("valid"):
        resume_info = resume_until_boot_exe(
            emu=emu,
            timeout_s=boot_timeout_s,
            step_chunk=step_chunk,
        )
    else:
        resume_info = resume_until_runtime(
            emu=emu,
            timeout_s=boot_timeout_s,
            step_chunk=step_chunk,
            clear_pause_pc=None,
        )

    if pause_after_exe_load:
        resume_info["paused"] = True
        resume_info["launch_mode"] = "pause_after_exe_load"
        if boot_exe_info:
            resume_info["boot_exe_info"] = boot_exe_info
            resume_info["boot_exe_history"] = _read_boot_exe_history(emu)
    else:
        resume_info["paused"] = False
        resume_info["launch_mode"] = launch_mode
    return resume_info


def resume_until_runtime(
    emu: EmuMcp,
    timeout_s: int = 60,
    step_chunk: int = 50000,
    clear_pause_pc: Optional[int] = None,
) -> dict:
    """Resume a paused emu cooperatively until the PC leaves BIOS."""
    t_start = time.monotonic()
    deadline = t_start + timeout_s
    final_pc = 0
    steps = 0

    if clear_pause_pc is not None:
        try:
            emu.call_tool_json("emu.clear_breakpoint_pc", {"pc": int(clear_pause_pc)})
        except EmuMcpError:
            log_mod.log("boot", "clear_pause_breakpoint_failed", pc=f"0x{clear_pause_pc:08X}")

    while time.monotonic() < deadline:
        final_pc = _read_pc(emu)
        if not (BIOS_PC_LOW <= final_pc < BIOS_PC_HIGH):
            boot_time_s = round(time.monotonic() - t_start, 2)
            log_mod.log("boot", "resumed_runtime", pc=f"0x{final_pc:08X}", boot_time_s=boot_time_s, steps=steps)
            return {
                "pc": f"0x{final_pc:08X}",
                "boot_time_s": boot_time_s,
                "steps": steps,
                "paused": False,
                "pid": None,
                "reused": False,
            }

        resume_result = emu.call_tool_json("emu.resume", {
            "max_steps": int(step_chunk),
            "stop_on_breakpoint": True,
        })
        steps += int(resume_result.get("steps_done", step_chunk))

    raise BootError(
        f"paused emu never reached runtime within {timeout_s}s "
        f"(last PC=0x{final_pc:08X}, steps={steps})"
    )


def resume_until_boot_exe(
    emu: EmuMcp,
    timeout_s: int = 60,
    step_chunk: int = 50000,
) -> dict:
    """Resume until the detected boot EXE entry point is reached."""
    t_start = time.monotonic()
    deadline = t_start + timeout_s
    final_pc = _read_pc(emu)
    steps = 0
    boot_exe_info = _read_boot_exe_info(emu)
    entry_pc = int(boot_exe_info.get("entry_pc", 0) or 0)

    while time.monotonic() < deadline:
        boot_exe_info = _read_boot_exe_info(emu)
        reached = bool(boot_exe_info.get("reached_entry_pc")) or bool(boot_exe_info.get("loaded_to_ram") and entry_pc != 0 and final_pc == entry_pc)
        final_pc = _read_pc(emu)
        if reached or (entry_pc != 0 and final_pc == entry_pc):
            boot_time_s = round(time.monotonic() - t_start, 2)
            log_mod.log("boot", "boot_exe_ready", pc=f"0x{final_pc:08X}", entry_pc=f"0x{entry_pc:08X}", boot_time_s=boot_time_s, steps=steps)
            return {
                "pc": f"0x{final_pc:08X}",
                "boot_time_s": boot_time_s,
                "steps": steps,
                "paused": True,
                "pid": None,
                "reused": False,
                "boot_exe_info": boot_exe_info,
                "boot_exe_history": _read_boot_exe_history(emu),
            }

        result = emu.call_tool_json("emu.resume_until_boot_exe", {"max_steps": int(step_chunk)})
        steps += int(result.get("steps_done", 0))
        final_pc_raw = result.get("pc", 0)
        try:
            final_pc = int(final_pc_raw)
        except Exception:
            final_pc = _read_pc(emu)
        if bool(result.get("hit")):
            boot_exe_info = _read_boot_exe_info(emu)
            boot_time_s = round(time.monotonic() - t_start, 2)
            log_mod.log("boot", "boot_exe_ready", pc=f"0x{final_pc:08X}", entry_pc=f"0x{entry_pc:08X}", boot_time_s=boot_time_s, steps=steps)
            return {
                "pc": f"0x{final_pc:08X}",
                "boot_time_s": boot_time_s,
                "steps": steps,
                "paused": True,
                "pid": None,
                "reused": False,
                "boot_exe_info": boot_exe_info,
                "boot_exe_history": _read_boot_exe_history(emu),
            }

    raise BootError(
        f"boot EXE entry not reached within {timeout_s}s "
        f"(last PC=0x{final_pc:08X}, steps={steps}, entry=0x{entry_pc:08X})"
    )


def advance_runtime(
    emu: EmuMcp,
    max_steps: int = 20000,
    max_frames: int = 1,
) -> dict:
    """Advance an already-loaded runtime/menu a little for dynamic observation."""
    before_pc = _read_pc(emu)
    result = emu.call_tool_json("emu.resume", {
        "max_steps": int(max_steps),
        "max_frames": int(max_frames),
        "stop_on_breakpoint": True,
    })
    after_pc = _read_pc(emu)
    out = {
        "pc_before": f"0x{before_pc:08X}",
        "pc_after": f"0x{after_pc:08X}",
        "pc": f"0x{after_pc:08X}",
        "steps": int(result.get("steps_done", 0)),
        "frames": int(result.get("frames_done", 0)),
        "hit_breakpoint": bool(result.get("hit_breakpoint", False)),
        "hit_pc": result.get("hit_pc", 0),
        "paused": True,
    }
    log_mod.log("boot", "advanced_runtime", **out)
    return out
