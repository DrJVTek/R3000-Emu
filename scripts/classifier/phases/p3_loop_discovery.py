"""Phase 3: correlate static findings with runtime render-loop evidence."""

from __future__ import annotations

from typing import Any

from .. import log as log_mod
from ..mcp.ghidra_http import GhidraHttpClient, GhidraHttpError


def _safe_tool_json(emu: Any, tool: str, arguments: dict[str, Any] | None = None) -> Any:
    try:
        return emu.call_tool_json(tool, arguments)
    except Exception as exc:
        log_mod.log("p3_loop", "tool_failed", tool=tool, error=str(exc))
        return {}


def _candidate_pc_values(payload: Any) -> list[str]:
    pcs: list[str] = []
    if isinstance(payload, dict):
        for key in ("source_pc", "pc", "address"):
            value = payload.get(key)
            if value not in (None, ""):
                pcs.append(str(value))
        for value in payload.values():
            if isinstance(value, list):
                for item in value:
                    pcs.extend(_candidate_pc_values(item))
            elif isinstance(value, dict):
                pcs.extend(_candidate_pc_values(value))
    elif isinstance(payload, list):
        for item in payload:
            pcs.extend(_candidate_pc_values(item))
    return pcs


def _as_int(value: Any) -> int:
    try:
        if isinstance(value, str):
            try:
                return int(value, 0)
            except ValueError:
                return int(value, 16)
        return int(value)
    except Exception:
        return 0


def _scene_readiness(runtime: dict[str, Any]) -> dict[str, Any]:
    score = 0.0
    reasons: list[str] = []

    gte_trace = runtime.get("gte_trace", {}) if isinstance(runtime.get("gte_trace"), dict) else {}
    # C++ emits top_ops (list); older/alt schemas may use opcode_counts (dict)
    top_ops = gte_trace.get("top_ops") or gte_trace.get("opcode_counts") or []
    top_gte = []
    for key in ("top_pcs", "hotspots", "pcs", "producers"):
        value = gte_trace.get(key)
        if isinstance(value, list):
            top_gte = value
            break
    if (isinstance(top_ops, list) and top_ops) or (isinstance(top_ops, dict) and top_ops):
        score += 2.0
        reasons.append("gte_opcode_counts")
    if top_gte:
        score += 1.0
        reasons.append("gte_hotspots")

    draw_list = runtime.get("draw_list", {}) if isinstance(runtime.get("draw_list"), dict) else {}
    # C++ nests counts: draw_list["origin_counts"]["origin_3d"], draw_list["packet_flags"]["textured"]
    origin_counts = draw_list.get("origin_counts") or {}
    pkt_flags = draw_list.get("packet_flags") or {}
    origin_3d = _as_int(
        origin_counts.get("origin_3d") or draw_list.get("origin_3d") or draw_list.get("origin3d") or draw_list.get("tri_3d")
    )
    textured = _as_int(pkt_flags.get("textured") or draw_list.get("textured"))
    if origin_3d > 0:
        score += 2.0
        reasons.append("draw_list_3d")
    if textured > 0:
        score += 0.5
        reasons.append("textured_prims")

    dma2 = runtime.get("dma2", {}) if isinstance(runtime.get("dma2"), dict) else {}
    # C++ nests top_pcs under dma2["last"]["top_pcs"]
    dma_last = dma2.get("last") or {}
    dma_top_pcs = dma_last.get("top_pcs") or dma2.get("top_pcs")
    if isinstance(dma_top_pcs, list) and dma_top_pcs:
        score += 1.5
        reasons.append("dma2_gpu_producers")

    scene = runtime.get("scene", {}) if isinstance(runtime.get("scene"), dict) else {}
    groups = scene.get("groups")
    if isinstance(groups, list) and groups:
        score += 1.0
        reasons.append("scene_groups")

    salience = runtime.get("salience", {}) if isinstance(runtime.get("salience"), dict) else {}
    targets = salience.get("targets")
    if isinstance(targets, list) and targets:
        score += 0.5
        reasons.append("salient_targets")

    focus = runtime.get("focus", {}) if isinstance(runtime.get("focus"), dict) else {}
    if bool(focus.get("has_focus")):
        score += 0.5
        reasons.append("focus_candidate")

    fingerprint = runtime.get("fingerprint", {}) if isinstance(runtime.get("fingerprint"), dict) else {}
    candidates = fingerprint.get("candidates")
    if isinstance(candidates, list) and candidates:
        score += 1.0
        reasons.append("render_fingerprint")

    return {
        "score": score,
        "reasons": reasons,
        "ready": score >= 4.0,
        "origin_3d": origin_3d,
    }


def _analysis_hints(disasm: Any) -> list[str]:
    text = str(disasm or "").lower()
    hints: list[str] = []
    if "cop2" in text or "rtpt" in text or "rtps" in text:
        hints.append("uses_gte")
    if "drawot" in text or "drawotag" in text:
        hints.append("calls_drawot")
    if "otag" in text or "addprim" in text or "clearot" in text:
        hints.append("touches_ot")
    if "poly" in text or "prim" in text or "packet" in text:
        hints.append("packet_or_poly_logic")
    if "swl" in text or "swr" in text:
        hints.append("custom_partial_store_builder")
    return hints


def _count_xrefs(payload: Any) -> int:
    if isinstance(payload, list):
        return len(payload)
    if isinstance(payload, dict):
        for key in ("items", "results", "data", "xrefs"):
            value = payload.get(key)
            if isinstance(value, list):
                return len(value)
    return 0


def _relation_hypotheses(runtime: dict[str, Any], static_discovery: dict[str, Any], loop_candidates: list[dict[str, Any]]) -> list[dict[str, Any]]:
    hypotheses: list[dict[str, Any]] = []
    static_links = list(static_discovery.get("link_hypotheses", []) or []) if isinstance(static_discovery, dict) else []
    for item in static_links[:8]:
        if isinstance(item, dict):
            hypotheses.append(dict(item))

    gte_opcodes = list(static_discovery.get("gte_opcodes_found", []) or []) if isinstance(static_discovery, dict) else []
    draw_list = runtime.get("draw_list", {}) if isinstance(runtime.get("draw_list"), dict) else {}
    _origin_counts = draw_list.get("origin_counts") or {}
    origin_3d = _as_int(_origin_counts.get("origin_3d") or draw_list.get("origin_3d") or draw_list.get("origin3d") or draw_list.get("tri_3d"))
    if gte_opcodes and origin_3d > 0:
        hypotheses.append({
            "kind": "runtime_and_static_support_3d_pipeline",
            "reason": "Static GTE opcode inventory exists and runtime sees 3D-origin draw activity.",
        })
    if not gte_opcodes and origin_3d == 0:
        hypotheses.append({
            "kind": "insufficient_render_signal",
            "reason": "No static GTE opcode inventory and no runtime 3D draw activity were observed yet.",
        })

    for item in loop_candidates[:8]:
        if not isinstance(item, dict):
            continue
        hints = list(item.get("analysis_hints", []) or [])
        if "uses_gte" in hints and "packet_or_poly_logic" in hints:
            hypotheses.append({
                "address": item.get("pc"),
                "kind": "possible_direct_vertex_or_poly_fill",
                "reason": "Loop candidate combines GTE and packet/poly logic in the same branch.",
            })
        if "touches_ot" in hints and "calls_drawot" in hints:
            hypotheses.append({
                "address": item.get("pc"),
                "kind": "possible_ot_submit_chain",
                "reason": "Loop candidate touches OT state and appears close to DrawOT submission.",
            })
    return hypotheses[:16]


def _focus_gte_trace(emu: Any, static_discovery: dict[str, Any]) -> None:
    # Optional: focus GTE trace window on statically-known function addresses.
    # Failure is legitimate (feature may be unavailable) — log but continue.
    gte_functions = static_discovery.get("gte_functions", []) or []
    explored = static_discovery.get("explored_functions", []) or []
    all_fns = list(gte_functions) + list(explored)
    addrs: list[int] = []
    for fn in all_fns:
        if not isinstance(fn, dict):
            continue
        raw = fn.get("address")
        if raw is None:
            continue
        v = _as_int(raw)
        # Only PSX mapped RAM/ROM range — filter out Ghidra file offsets and GTE lib stubs
        if 0x80000000 <= v <= 0x801FFFFF:
            addrs.append(v)
    if not addrs:
        log_mod.log("p3_loop", "gte_trace_window_skipped", reason="no static addresses available")
        return
    pc_start = min(addrs)
    pc_end = max(addrs) + 0x400
    try:
        emu.call_tool_json("emu.set_gte_trace_window", {
            "pc_start": pc_start,
            "pc_end": pc_end,
            "start_frame": 0,
            "end_frame": 9999,
            "enabled": True,
        })
        log_mod.log("p3_loop", "gte_trace_window_set",
                    pc_start=f"0x{pc_start:08X}", pc_end=f"0x{pc_end:08X}",
                    fn_count=len(addrs))
    except Exception as exc:
        # set_gte_trace_window is optional — older builds may not expose it.
        log_mod.log("p3_loop", "gte_trace_window_unavailable", error=str(exc))


def _advance_runtime(emu: Any, cfg: Any) -> None:
    # Advance the emulator a few frames so the game enters its 3D render loop
    # before we collect runtime observations. Failure here is a real error —
    # without advancing, runtime data will be meaningless (readiness=0).
    workflow = getattr(cfg, "workflow", None)
    advance_frames = max(1, int(getattr(workflow, "advance_frames_before_snapshot", 2) or 2))
    step_chunk = max(1000, int(getattr(getattr(cfg, "emu", None), "step_chunk", 50000) or 50000))
    # max_steps must be generous: PSX ≈ 1.1M instructions/frame; let max_frames drive the stop
    generous_steps = max(2_000_000, step_chunk * advance_frames * 40)
    result = emu.call_tool_json("emu.resume", {
        "max_steps": generous_steps,
        "max_frames": advance_frames,
        "stop_on_breakpoint": False,
    })
    frames_done = int(result.get("frames_done", 0)) if isinstance(result, dict) else 0
    steps_done = int(result.get("steps_done", 0)) if isinstance(result, dict) else 0
    log_mod.log("p3_loop", "advanced_runtime",
                frames_requested=advance_frames, frames_done=frames_done, steps_done=steps_done)


def run(ctx: dict[str, Any], advance: bool = True) -> dict[str, Any]:
    emu = ctx["emu"]
    ghidra: GhidraHttpClient | None = ctx.get("ghidra")
    static_discovery = ctx.get("static_discovery", {})
    cfg = ctx.get("config")

    _focus_gte_trace(emu, static_discovery)
    if advance:
        _advance_runtime(emu, cfg)

    runtime = {
        "status": _safe_tool_json(emu, "emu.get_status"),
        "cpu": _safe_tool_json(emu, "emu.get_cpu_state"),
        "runtime_modules": _safe_tool_json(emu, "emu.get_runtime_module_history"),
        "gte_trace": _safe_tool_json(emu, "emu.get_gte_trace_summary"),
        "dma2": _safe_tool_json(emu, "emu.get_dma2_nohint_summary"),
        "draw_list": _safe_tool_json(emu, "emu.get_draw_list_summary"),
        "scene": _safe_tool_json(emu, "emu.get_scene_vector_snapshot", {"max_groups": 12, "max_roots": 8}),
        "salience": _safe_tool_json(emu, "emu.get_scene_salience_summary", {"max_targets": 6}),
        "focus": _safe_tool_json(emu, "emu.get_focus_candidate"),
        "transforms": _safe_tool_json(emu, "emu.get_transform_roots"),
        "group_links": _safe_tool_json(emu, "emu.get_group_transform_links"),
        "mesh_candidates": _safe_tool_json(emu, "emu.get_mesh_cache_candidates"),
        "fingerprint": _safe_tool_json(emu, "emu.match_render_pattern", {"max_candidates": 4}),
    }

    candidate_pcs = []
    candidate_pcs.extend(_candidate_pc_values(runtime["scene"]))
    candidate_pcs.extend(_candidate_pc_values(runtime["focus"]))
    candidate_pcs.extend(_candidate_pc_values(runtime["mesh_candidates"]))
    candidate_pcs.extend(_candidate_pc_values(runtime["runtime_modules"]))
    for fn in static_discovery.get("gte_functions", []):
        address = fn.get("address")
        if address:
            candidate_pcs.append(str(address))
    for fn in static_discovery.get("explored_functions", []):
        address = fn.get("address")
        if address:
            candidate_pcs.append(str(address))

    unique_pcs: list[str] = []
    seen: set[str] = set()
    for pc in candidate_pcs:
        if pc in seen:
            continue
        seen.add(pc)
        unique_pcs.append(pc)

    loop_candidates = []
    ghidra_available = bool(ghidra and static_discovery.get("ghidra_available"))
    if ghidra_available and ghidra is not None:
        for pc in unique_pcs[:10]:
            try:
                fn = ghidra.get_function_by_address(pc)
                xrefs = ghidra.get_xrefs_to(pc)
                xrefs_from = ghidra.get_xrefs_from(pc)
                disasm = ghidra.disassemble_function(pc)
                try:
                    decomp = ghidra.decompile_function(pc)
                except GhidraHttpError:
                    decomp = None
            except GhidraHttpError as exc:
                log_mod.log("p3_loop", "ghidra_lookup_failed", pc=pc, error=str(exc))
                continue
            loop_candidates.append({
                "pc": pc,
                "function": fn,
                "xrefs_to": xrefs,
                "disasm": disasm,
                "decomp": decomp,
                "xrefs_to_count": _count_xrefs(xrefs),
                "xrefs_from_count": _count_xrefs(xrefs_from),
                "analysis_hints": _analysis_hints(disasm),
            })

    result = {
        "runtime": runtime,
        "loop_candidates": loop_candidates,
        "candidate_pcs": unique_pcs[:16],
        "ghidra_available": ghidra_available,
    }
    readiness = _scene_readiness(runtime)
    result["readiness"] = readiness
    result["relation_hypotheses"] = _relation_hypotheses(runtime, static_discovery, loop_candidates)
    result["static_branch_edges"] = list(static_discovery.get("exploration_edges", []) or [])[:32]
    log_mod.log(
        "p3_loop",
        "done",
        loop_entry=unique_pcs[0] if unique_pcs else None,
        fn_count=len(loop_candidates),
        readiness_score=readiness.get("score", 0.0),
    )
    return result
