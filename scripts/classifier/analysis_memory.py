"""Persistent low-context memory snapshots for classifier sessions."""

from __future__ import annotations

import json
from pathlib import Path
from typing import Any


def _compact_static(static_discovery: dict[str, Any]) -> dict[str, Any]:
    out: dict[str, Any] = {
        "ghidra_available": static_discovery.get("ghidra_available"),
        "recommendations": list(static_discovery.get("recommendations", []) or [])[:12],
        "gte_opcodes_found": list(static_discovery.get("gte_opcodes_found", []) or [])[:24],
        "runtime_gte_inventory": static_discovery.get("runtime_gte_inventory", {}),
        "link_hypotheses": list(static_discovery.get("link_hypotheses", []) or [])[:12],
    }
    out["draw_calls"] = [
        {
            "query": item.get("query"),
            "name": item.get("name"),
            "address": item.get("address"),
        }
        for item in list(static_discovery.get("draw_calls", []) or [])[:16]
        if isinstance(item, dict)
    ]
    out["gte_functions"] = [
        {
            "address": item.get("address"),
            "source": item.get("source"),
            "gte_opcodes": list(item.get("gte_opcodes", []) or [])[:12],
            "function_name": (item.get("function", {}) or {}).get("name") if isinstance(item.get("function"), dict) else None,
        }
        for item in list(static_discovery.get("gte_functions", []) or [])[:16]
        if isinstance(item, dict)
    ]
    out["explored_functions"] = [
        {
            "address": item.get("address"),
            "function_name": item.get("function_name"),
            "source": item.get("source"),
            "gte_opcodes": list(item.get("gte_opcodes", []) or [])[:12],
            "xrefs_to_count": item.get("xrefs_to_count"),
            "xrefs_from_count": item.get("xrefs_from_count"),
            "analysis_hints": list(item.get("analysis_hints", []) or [])[:12],
        }
        for item in list(static_discovery.get("explored_functions", []) or [])[:24]
        if isinstance(item, dict)
    ]
    return out


def _compact_dynamic(dynamic_discovery: dict[str, Any]) -> dict[str, Any]:
    runtime = dynamic_discovery.get("runtime", {}) if isinstance(dynamic_discovery, dict) else {}
    out: dict[str, Any] = {
        "candidate_pcs": list(dynamic_discovery.get("candidate_pcs", []) or [])[:24],
        "readiness": dynamic_discovery.get("readiness", {}),
        "relation_hypotheses": list(dynamic_discovery.get("relation_hypotheses", []) or [])[:12],
        "runtime": {
            "status": runtime.get("status", {}),
            "runtime_modules": runtime.get("runtime_modules", {}),
            "gte_trace": runtime.get("gte_trace", {}),
            "dma2": runtime.get("dma2", {}),
            "draw_list": runtime.get("draw_list", {}),
            "scene": runtime.get("scene", {}),
            "salience": runtime.get("salience", {}),
            "focus": runtime.get("focus", {}),
            "fingerprint": runtime.get("fingerprint", {}),
        },
    }
    out["loop_candidates"] = [
        {
            "pc": item.get("pc"),
            "function_name": (item.get("function", {}) or {}).get("name") if isinstance(item.get("function"), dict) else None,
            "analysis_hints": list(item.get("analysis_hints", []) or [])[:12],
            "xrefs_to_count": item.get("xrefs_to_count"),
            "xrefs_from_count": item.get("xrefs_from_count"),
        }
        for item in list(dynamic_discovery.get("loop_candidates", []) or [])[:24]
        if isinstance(item, dict)
    ]
    return out


def _compact_classification(classification: dict[str, Any]) -> dict[str, Any]:
    return {
        "summary": classification.get("summary"),
        "notes": list(classification.get("notes", []) or [])[:24],
        "rules": [
            {
                "mode_kind": item.get("mode_kind"),
                "link_rule": item.get("link_rule"),
                "confidence": item.get("confidence"),
                "reason": item.get("reason"),
                "producer_pc_ranges": list(item.get("producer_pc_ranges", []) or [])[:12],
                "gte_pc_ranges": list(item.get("gte_pc_ranges", []) or [])[:12],
                "ot_fill_pc_ranges": list(item.get("ot_fill_pc_ranges", []) or [])[:12],
            }
            for item in list(classification.get("rules", []) or [])[:8]
            if isinstance(item, dict)
        ],
    }


def build_snapshot(stage: str, ctx: dict[str, Any]) -> dict[str, Any]:
    return {
        "stage": stage,
        "game_id": ctx.get("game_id"),
        "started_at": ctx.get("started_at"),
        "boot_info": ctx.get("boot_info", {}),
        "static_discovery": _compact_static(ctx.get("static_discovery", {}) or {}),
        "dynamic_discovery": _compact_dynamic(ctx.get("dynamic_discovery", {}) or {}),
        "classification": _compact_classification(ctx.get("classification", {}) or {}),
        "analysis_notes": list(ctx.get("analysis_notes", []) or [])[:64],
        "signal_seek_passes": list(ctx.get("signal_seek_passes", []) or [])[:32],
        "analysis_passes": list(ctx.get("analysis_passes", []) or [])[:32],
        "profile_path": ctx.get("profile_path"),
        "report_path": ctx.get("report_path"),
        "session_log": ctx.get("session_log"),
    }


def write_snapshot(memory_dir: str | Path, game_id: str, stage: str, ctx: dict[str, Any]) -> str:
    out_dir = Path(memory_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    safe_game = "".join(ch if ch.isalnum() or ch in "._-" else "_" for ch in game_id) or "unknown"
    filename = f"{safe_game}-{stage}.state.json"
    path = out_dir / filename
    payload = build_snapshot(stage, ctx)
    path.write_text(json.dumps(payload, indent=2, ensure_ascii=False), encoding="utf-8")
    return str(path)
