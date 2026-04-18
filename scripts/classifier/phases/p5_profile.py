"""Phase 5: write the generated `.psx3dprof` file."""

from __future__ import annotations

from pathlib import Path
from typing import Any

from .. import log as log_mod


def _sanitize_game_id(game_id: str) -> str:
    out = []
    for ch in game_id:
        if ch.isalnum() or ch in "._-":
            out.append(ch)
        else:
            out.append("_")
    return "".join(out) or "unknown"


def _emit_ranges(prefix: str, ranges: list[Any]) -> list[str]:
    values: list[str] = []
    for item in ranges:
        if isinstance(item, str):
            text = item.strip()
            if "-" in text:
                a, b = text.split("-", 1)
                try:
                    values.append(f"0x{int(a, 0):08X}-0x{int(b, 0):08X}")
                except ValueError:
                    continue
            else:
                try:
                    v = int(text, 0)
                    values.append(f"0x{v:08X}")
                except ValueError:
                    continue
        elif isinstance(item, dict):
            start = item.get("start")
            end = item.get("end")
            if start is not None and end is not None:
                values.append(f"0x{int(start):08X}-0x{int(end):08X}")
    if not values:
        return []
    return [f"{prefix} " + " ".join(values)]


def run(ctx: dict[str, Any]) -> dict[str, Any]:
    cfg = ctx["config"]
    classification = ctx["classification"]
    rules = classification.get("rules", [])

    game_id = _sanitize_game_id(ctx["game_id"])
    output_dir = Path(cfg.output.profiles_dir)
    output_dir.mkdir(parents=True, exist_ok=True)
    profile_path = output_dir / f"{game_id}.generated.psx3dprof"

    lines = ["PSX3D_PROFILE_V2", f"GAME {game_id}"]
    for priority, rule in enumerate(rules, start=1):
        lines.append(f"MODE {rule['mode_kind']} {rule['link_rule']} {priority}")
        lines.extend(_emit_ranges("MODE_PRODUCER_RANGES", rule.get("producer_pc_ranges", [])))
        lines.extend(_emit_ranges("MODE_GTE_RANGES", rule.get("gte_pc_ranges", [])))
        lines.extend(_emit_ranges("MODE_OT_RANGES", rule.get("ot_fill_pc_ranges", [])))

    profile_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    log_mod.log("p5_profile", "done", profile_path=str(profile_path), fn_count=len(rules))
    return {"profile_path": str(profile_path), "rule_count": len(rules)}
