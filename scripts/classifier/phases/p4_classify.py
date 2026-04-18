"""Phase 4: ask the LLM to classify the discovered render loop(s)."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any

from .. import log as log_mod
from ..mcp.llm_litellm import LlmClient, LlmError
from ..notes import top_notes
from ..taxonomy import normalize_link_rule, normalize_mode_kind


def _read_text(path: str | Path) -> str:
    return Path(path).read_text(encoding="utf-8")


def _sanitize_rules(raw_rules: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], list[str]]:
    rules: list[dict[str, Any]] = []
    notes: list[str] = []
    for idx, raw in enumerate(raw_rules, start=1):
        mode_kind, exact = normalize_mode_kind(str(raw.get("mode_kind", "")))
        link_rule = normalize_link_rule(raw.get("link_rule"))
        if mode_kind == "unknown":
            notes.append(f"rule_{idx}: unknown mode_kind {raw.get('mode_kind')!r}")
            continue
        if not exact:
            notes.append(f"rule_{idx}: normalized mode_kind -> {mode_kind}")
        rules.append({
            "mode_kind": mode_kind,
            "link_rule": link_rule,
            "confidence": float(raw.get("confidence", 0.0)),
            "reason": str(raw.get("reason", "")),
            "producer_pc_ranges": list(raw.get("producer_pc_ranges", []) or []),
            "gte_pc_ranges": list(raw.get("gte_pc_ranges", []) or []),
            "ot_fill_pc_ranges": list(raw.get("ot_fill_pc_ranges", []) or []),
        })
    rules.sort(key=lambda r: r.get("confidence", 0.0), reverse=True)
    return rules, notes


def _compact_playbook(text: str, max_chars: int = 1200) -> str:
    stripped = text.strip()
    if len(stripped) <= max_chars:
        return stripped
    lines = stripped.splitlines()
    kept: list[str] = []
    for line in lines:
        s = line.strip()
        if not s:
            if kept and kept[-1] != "":
                kept.append("")
            continue
        if s.startswith("#") or s.startswith("- ") or s.startswith("* ") or s[:2].isdigit():
            kept.append(line)
        elif len(kept) < 24:
            kept.append(line)
        if sum(len(x) + 1 for x in kept) >= max_chars:
            break
    compact = "\n".join(kept).strip()
    return compact[:max_chars].rstrip()


def _summarize_boot_info(boot: dict[str, Any]) -> dict[str, Any]:
    boot_exe = boot.get("boot_exe_info", {}) if isinstance(boot, dict) else {}
    history = boot.get("boot_exe_history", {}) if isinstance(boot, dict) else {}
    events = history.get("events", []) if isinstance(history, dict) else []
    return {
        "launch_mode": boot.get("launch_mode"),
        "pc": boot.get("pc"),
        "paused": boot.get("paused"),
        "boot_exe": {
            "source": boot_exe.get("source"),
            "boot_path": boot_exe.get("boot_path"),
            "entry_pc": boot_exe.get("entry_pc"),
            "loaded_to_ram": boot_exe.get("loaded_to_ram"),
            "reached_entry_pc": boot_exe.get("reached_entry_pc"),
        } if boot_exe else {},
        "boot_history": [
            {
                "step_index": ev.get("step_index"),
                "stage": ev.get("stage"),
                "pc": ev.get("pc"),
                "entry_pc": ev.get("entry_pc"),
            }
            for ev in events[:8]
        ],
    }


def _summarize_static_discovery(static_discovery: dict[str, Any]) -> dict[str, Any]:
    draw_calls = static_discovery.get("draw_calls", []) if isinstance(static_discovery, dict) else []
    gte_functions = static_discovery.get("gte_functions", []) if isinstance(static_discovery, dict) else []
    runtime_gte = static_discovery.get("runtime_gte", {}) if isinstance(static_discovery, dict) else {}
    return {
        "ghidra_available": static_discovery.get("ghidra_available"),
        "recommendations": list(static_discovery.get("recommendations", []) or [])[:8],
        "draw_calls": [
            {
                "query": item.get("query"),
                "name": item.get("name"),
                "address": item.get("address"),
            }
            for item in draw_calls[:8]
            if isinstance(item, dict)
        ],
        "gte_functions": [
            {
                "address": item.get("address"),
                "source": item.get("source"),
                "function_name": (item.get("function", {}) or {}).get("name") if isinstance(item.get("function"), dict) else None,
            }
            for item in gte_functions[:8]
            if isinstance(item, dict)
        ],
        "runtime_gte": {
            "top_pcs": list((runtime_gte.get("top_pcs", []) or []))[:8] if isinstance(runtime_gte, dict) else [],
            "opcode_counts": dict(list(((runtime_gte.get("opcode_counts", {}) or {}).items()))[:12]) if isinstance(runtime_gte, dict) else {},
        },
    }


def _summarize_dynamic_discovery(dynamic_discovery: dict[str, Any]) -> dict[str, Any]:
    runtime = dynamic_discovery.get("runtime", {}) if isinstance(dynamic_discovery, dict) else {}
    loop_candidates = dynamic_discovery.get("loop_candidates", []) if isinstance(dynamic_discovery, dict) else []
    runtime_modules = runtime.get("runtime_modules", {}) if isinstance(runtime, dict) else {}
    scene = runtime.get("scene", {}) if isinstance(runtime, dict) else {}
    salience = runtime.get("salience", {}) if isinstance(runtime, dict) else {}
    focus = runtime.get("focus", {}) if isinstance(runtime, dict) else {}
    fingerprint = runtime.get("fingerprint", {}) if isinstance(runtime, dict) else {}
    dma2 = runtime.get("dma2", {}) if isinstance(runtime, dict) else {}
    draw_list = runtime.get("draw_list", {}) if isinstance(runtime, dict) else {}
    transforms = runtime.get("transforms", {}) if isinstance(runtime, dict) else {}
    group_links = runtime.get("group_links", {}) if isinstance(runtime, dict) else {}
    mesh_candidates = runtime.get("mesh_candidates", {}) if isinstance(runtime, dict) else {}
    return {
        "candidate_pcs": list(dynamic_discovery.get("candidate_pcs", []) or [])[:16],
        "loop_candidates": [
            {
                "pc": item.get("pc"),
                "function_name": (item.get("function", {}) or {}).get("name") if isinstance(item.get("function"), dict) else None,
            }
            for item in loop_candidates[:8]
            if isinstance(item, dict)
        ],
        "runtime_modules": [
            {
                "step_index": ev.get("step_index"),
                "kind": ev.get("kind"),
                "label": ev.get("label"),
                "pc": ev.get("pc"),
                "base": ev.get("base"),
            }
            for ev in list(runtime_modules.get("events", []) or [])[:12]
            if isinstance(ev, dict)
        ],
        "scene_summary": scene.get("scene_summary", {}) if isinstance(scene, dict) else {},
        "scene_groups": [
            {
                "group_id": g.get("group_id"),
                "kind": g.get("kind"),
                "source_pc": g.get("source_pc"),
                "root_addr": g.get("root_addr"),
                "stable_score": g.get("stable_score"),
                "screen_bbox": g.get("screen_bbox"),
            }
            for g in list(scene.get("groups", []) or [])[:3]
            if isinstance(g, dict)
        ] if isinstance(scene, dict) else [],
        "salience": {
            "summary": salience.get("summary"),
            "targets": [
                {
                    "group_id": t.get("group_id"),
                    "kind": t.get("kind"),
                    "score": t.get("score"),
                    "reason": t.get("reason"),
                    "source_pc": t.get("source_pc"),
                }
                for t in list(salience.get("targets", []) or [])[:4]
                if isinstance(t, dict)
            ],
        } if isinstance(salience, dict) else {},
        "focus": {
            "has_focus": focus.get("has_focus"),
            "summary": focus.get("summary"),
            "group_id": focus.get("group_id"),
            "source_pc": focus.get("source_pc"),
            "root_addr": focus.get("root_addr"),
        } if isinstance(focus, dict) else {},
        "fingerprint": {
            "summary": fingerprint.get("summary"),
            "candidate_count": fingerprint.get("candidate_count"),
            "top_candidate": (list(fingerprint.get("candidates", []) or [])[:1] or [None])[0],
        } if isinstance(fingerprint, dict) else {},
        "dma2": {
            "summary": dma2.get("summary"),
            "top_pcs": list(dma2.get("top_pcs", []) or [])[:6],
        } if isinstance(dma2, dict) else {},
        "draw_list": {
            "summary": draw_list.get("summary"),
            "origin_3d": draw_list.get("origin_3d"),
            "origin_2d": draw_list.get("origin_2d"),
            "textured": draw_list.get("textured"),
        } if isinstance(draw_list, dict) else {},
        "transforms": {
            "summary": transforms.get("summary"),
            "roots": list(transforms.get("roots", []) or [])[:4],
        } if isinstance(transforms, dict) else {},
        "group_links": {
            "summary": group_links.get("summary"),
            "links": list(group_links.get("links", []) or [])[:4],
        } if isinstance(group_links, dict) else {},
        "mesh_candidates": {
            "summary": mesh_candidates.get("summary"),
            "candidates": list(mesh_candidates.get("candidates", []) or [])[:4],
        } if isinstance(mesh_candidates, dict) else {},
    }


def _summarize_notes(ctx: dict[str, Any]) -> list[dict[str, Any]]:
    compact: list[dict[str, Any]] = []
    for note in top_notes(ctx, limit=16):
        compact.append({
            "stage": note.get("stage"),
            "kind": note.get("kind"),
            "summary": note.get("summary"),
            "details": note.get("details", {}),
        })
    return compact


def run(ctx: dict[str, Any]) -> dict[str, Any]:
    cfg = ctx["config"]
    llm: LlmClient = ctx["llm"]
    static_discovery = ctx["static_discovery"]
    dynamic_discovery = ctx["dynamic_discovery"]

    if not llm.enabled:
        raise RuntimeError("LLM classification is required by project policy; disable-llm runs are not supported")

    system_template = _read_text(Path(__file__).resolve().parents[1] / "prompts" / "system_playbook.md")
    user_template = _read_text(Path(__file__).resolve().parents[1] / "prompts" / "classify_loop.tmpl")
    playbook_text = _read_text(cfg.playbook.path)

    system_prompt = system_template.format(playbook_text=_compact_playbook(playbook_text))
    boot_summary = _summarize_boot_info(ctx["boot_info"])
    static_summary = _summarize_static_discovery(static_discovery)
    dynamic_summary = _summarize_dynamic_discovery(dynamic_discovery)
    notes_summary = _summarize_notes(ctx)
    user_prompt = user_template.format(
        game_id=ctx["game_id"],
        boot_info=json.dumps(boot_summary, indent=2, ensure_ascii=False),
        static_discovery=json.dumps(static_summary, indent=2, ensure_ascii=False),
        dynamic_discovery=json.dumps(dynamic_summary, indent=2, ensure_ascii=False),
        analysis_notes=json.dumps(notes_summary, indent=2, ensure_ascii=False),
    )

    try:
        raw_json, raw_text = llm.complete_json(system_prompt, user_prompt)
    except LlmError as exc:
        log_mod.log("p4_classify", "llm_failed", error=str(exc))
        raise RuntimeError(f"LLM classification failed: {exc}") from exc

    raw_rules = raw_json.get("rules", []) if isinstance(raw_json, dict) else []
    rules, notes = _sanitize_rules(raw_rules if isinstance(raw_rules, list) else [])
    result = {
        "summary": raw_json.get("summary", "") if isinstance(raw_json, dict) else "",
        "rules": rules,
        "notes": notes + list(raw_json.get("notes", []) if isinstance(raw_json, dict) else []),
        "raw_response": raw_text,
    }
    log_mod.log("p4_classify", "done", fn_count=len(rules), confidence=rules[0]["confidence"] if rules else 0.0)
    return result
