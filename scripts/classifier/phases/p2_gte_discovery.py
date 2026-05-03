"""Phase 2: static-first discovery around GTE and display entry points."""

from __future__ import annotations

import re
from typing import Any

from .. import log as log_mod
from ..mcp.ghidra_http import GhidraHttpClient, GhidraHttpError


def _coerce_list(data: Any) -> list[Any]:
    if isinstance(data, list):
        return data
    if isinstance(data, dict):
        for key in ("functions", "items", "results", "data"):
            value = data.get(key)
            if isinstance(value, list):
                return value
    return []


def _extract_address(entry: Any) -> str | None:
    if isinstance(entry, dict):
        for key in ("address", "entry", "addr", "function_address"):
            if key in entry:
                return str(entry[key])
    return None


def _extract_name(entry: Any) -> str:
    if isinstance(entry, dict):
        for key in ("name", "function_name", "label"):
            if key in entry:
                return str(entry[key])
    return str(entry)


def _extract_gte_opcodes(disasm: Any) -> list[str]:
    text = str(disasm or "")
    if not text:
        return []
    found: set[str] = set()
    for opcode in (
        "RTPS", "RTPT", "NCLIP", "OP", "DPCS", "INTPL", "MVMVA",
        "NCDS", "CDP", "NCDT", "NCCS", "CC", "NCS", "NCT",
        "SQR", "DCPL", "DPCT", "AVSZ3", "AVSZ4", "GPF", "GPL",
        "NCCT",
    ):
        if re.search(rf"\b{opcode}\b", text, re.IGNORECASE):
            found.add(opcode)
    return sorted(found)


def _count_xrefs(payload: Any) -> int:
    if isinstance(payload, list):
        return len(payload)
    if isinstance(payload, dict):
        for key in ("items", "results", "data", "xrefs"):
            value = payload.get(key)
            if isinstance(value, list):
                return len(value)
    if isinstance(payload, str):
        return len(re.findall(r"\b(?:From|To)\s+(?:0x)?[0-9A-Fa-f]{6,8}\b", payload))
    return 0


def _normalize_address(value: Any) -> str | None:
    if value in (None, ""):
        return None
    text = str(value).strip()
    if not text:
        return None
    if re.fullmatch(r"(?:0x)?[0-9A-Fa-f]{6,8}", text):
        return f"0x{int(text.removeprefix('0x').removeprefix('0X'), 16):08X}"
    try:
        return f"0x{int(text, 0):08X}"
    except Exception:
        return text


def _address_to_int(value: Any) -> int | None:
    normalized = _normalize_address(value)
    if not normalized or not normalized.startswith("0x"):
        return None
    try:
        return int(normalized, 16)
    except ValueError:
        return None


def _is_runtime_psx_address(value: Any) -> bool:
    addr = _address_to_int(value)
    if addr is None:
        return False
    return (
        0x00010000 <= addr <= 0x001FFFFF
        or 0x80000000 <= addr <= 0x807FFFFF
        or 0xA0000000 <= addr <= 0xA07FFFFF
    )


def _extract_xref_addresses(payload: Any) -> list[str]:
    items = _coerce_list(payload)
    out: list[str] = []
    for item in items:
        if isinstance(item, dict):
            for key in ("address", "from_address", "to_address", "from", "to", "target", "pc"):
                normalized = _normalize_address(item.get(key))
                if normalized:
                    out.append(normalized)
    if isinstance(payload, str):
        for match in re.finditer(r"\b(?:From|To)\s+((?:0x)?[0-9A-Fa-f]{6,8})\b", payload):
            normalized = _normalize_address(match.group(1))
            if normalized:
                out.append(normalized)
    deduped: list[str] = []
    seen: set[str] = set()
    for addr in out:
        if addr in seen:
            continue
        seen.add(addr)
        deduped.append(addr)
    return deduped


def _summarize_runtime_gte(runtime_gte: Any) -> dict[str, Any]:
    if not isinstance(runtime_gte, dict):
        return {}
    opcode_counts = runtime_gte.get("opcode_counts", {})
    if not isinstance(opcode_counts, dict):
        opcode_counts = {}
    top_opcode_counts = dict(list(opcode_counts.items())[:16])
    hotspot_pcs: list[Any] = []
    for key in ("hotspots", "pcs", "top_pcs", "producers"):
        value = runtime_gte.get(key)
        if isinstance(value, list):
            hotspot_pcs = value[:16]
            break
    return {
        "opcode_counts": top_opcode_counts,
        "hotspot_pcs": hotspot_pcs,
    }


def _analysis_hints(disasm: Any, decomp: Any) -> list[str]:
    text = f"{disasm}\n{decomp}".lower()
    hints: list[str] = []
    if "cop2" in text or "rtpt" in text or "rtps" in text:
        hints.append("uses_gte")
    if "drawot" in text or "drawotag" in text:
        hints.append("calls_drawot")
    if "addprim" in text or "clearot" in text or "otag" in text:
        hints.append("touches_ot")
    if "mtc2" in text or "ctc2" in text or "matrix" in text:
        hints.append("touches_matrix_setup")
    if "gte_stsz" in text or "avsz" in text or "nclip" in text:
        hints.append("depth_or_clip_logic")
    if "poly" in text or "prim" in text or "packet" in text:
        hints.append("packet_or_poly_logic")
    return hints


def _summarize_function(
    ghidra: GhidraHttpClient,
    address: str,
    source: str,
    depth: int,
    edge_kind: str | None = None,
    parent_address: str | None = None,
) -> dict[str, Any] | None:
    try:
        function = ghidra.get_function_by_address(address)
        disasm = ghidra.disassemble_function(address)
        decomp = ghidra.decompile_function(address)
        xrefs_to = ghidra.get_xrefs_to(address)
        xrefs_from = ghidra.get_xrefs_from(address)
    except GhidraHttpError as exc:
        log_mod.log("p2_gte", "ghidra_explore_failed", address=address, error=str(exc))
        return None

    gte_opcodes = _extract_gte_opcodes(disasm)
    hints = _analysis_hints(disasm, decomp)
    return {
        "address": address,
        "source": source,
        "depth": depth,
        "edge_kind": edge_kind,
        "parent_address": parent_address,
        "function_name": _extract_name(function),
        "gte_opcodes": gte_opcodes,
        "analysis_hints": hints,
        "xrefs_to_count": _count_xrefs(xrefs_to),
        "xrefs_from_count": _count_xrefs(xrefs_from),
        "xrefs_to_addresses": _extract_xref_addresses(xrefs_to),
        "xrefs_from_addresses": _extract_xref_addresses(xrefs_from),
        "disasm": disasm,
        "decomp": decomp,
    }


def run(ctx: dict[str, Any]) -> dict[str, Any]:
    ghidra: GhidraHttpClient | None = ctx.get("ghidra")
    emu = ctx.get("emu")
    cfg = ctx.get("config")
    workflow_cfg = getattr(cfg, "workflow", None)
    branch_depth = max(0, int(getattr(workflow_cfg, "ghidra_branch_depth", 2) or 0))
    branch_fanout = max(1, int(getattr(workflow_cfg, "ghidra_branch_fanout", 6) or 1))
    seed_limit = max(1, int(getattr(workflow_cfg, "ghidra_static_seed_limit", 16) or 1))

    runtime_gte = {}
    if emu is not None:
        try:
            runtime_gte = emu.call_tool_json("emu.get_gte_trace_summary")
        except Exception as exc:
            log_mod.log("p2_gte", "runtime_gte_unavailable", error=str(exc))

    result: dict[str, Any] = {
        "ghidra_available": False,
        "runtime_gte": runtime_gte,
        "runtime_gte_inventory": _summarize_runtime_gte(runtime_gte),
        "draw_calls": [],
        "gte_functions": [],
        "gte_opcodes_found": [],
        "explored_functions": [],
        "exploration_edges": [],
        "link_hypotheses": [],
        "recommendations": [],
    }

    if ghidra is None:
        log_mod.log("p2_gte", "ghidra_missing")
        result["recommendations"].append("use_runtime_only")
        return result

    if not ghidra.ping():
        log_mod.log("p2_gte", "ghidra_unreachable", base_url=ghidra.base_url)
        result["recommendations"].append("ghidra_unavailable")
        return result

    result["ghidra_available"] = True

    draw_queries = (
        "DrawOTag", "DrawOT", "AddPrim", "addPrim", "ClearOTag", "ClearOTagR",
        "RotTransPers", "RotAverageNclip", "RotAverage4", "RotAverage3",
        "RotColorDpq", "RotColorMatDpq", "RotColorMatDpq3", "RotColorMatDpq4",
        "SetRotMatrix", "SetTransMatrix", "MulMatrix", "TransMatrix",
        "ApplyMatrix", "CompMatrix", "RCpoly", "DivPloy", "DivPoly",
    )
    for query in draw_queries:
        try:
            matches = _coerce_list(ghidra.search_functions(query, limit=12))
        except GhidraHttpError:
            continue
        for match in matches:
            result["draw_calls"].append({
                "query": query,
                "name": _extract_name(match),
                "address": _extract_address(match),
            })

    hotspot_entries = []
    if isinstance(runtime_gte, dict):
        for key in ("hotspots", "pcs", "top_pcs", "producers"):
            value = runtime_gte.get(key)
            if isinstance(value, list):
                hotspot_entries = value
                break

    seen_addresses: set[str] = set()
    for entry in hotspot_entries[:12]:
        address = None
        if isinstance(entry, dict):
            pc = entry.get("pc") or entry.get("address")
            if pc is not None:
                address = str(pc)
        elif isinstance(entry, (str, int)):
            address = str(entry)
        if not address or address in seen_addresses:
            continue
        seen_addresses.add(address)
        try:
            function = ghidra.get_function_by_address(address)
            disasm = ghidra.disassemble_function(address)
        except GhidraHttpError as exc:
            log_mod.log("p2_gte", "ghidra_lookup_failed", address=address, error=str(exc))
            continue
        result["gte_functions"].append({
            "address": address,
            "function": function,
            "disasm": disasm,
            "gte_opcodes": _extract_gte_opcodes(disasm),
            "source": "runtime_hotspot",
        })

    if not result["gte_functions"]:
        for query in (
            "gte", "Rot", "Matrix", "Sort", "Draw", "Clip",
            "Pers", "Nclip", "OTag", "Prim", "Trans", "Color", "Dpq",
        ):
            try:
                matches = _coerce_list(ghidra.search_functions(query, limit=8))
            except GhidraHttpError:
                continue
            for match in matches:
                address = _extract_address(match)
                if not address or address in seen_addresses:
                    continue
                seen_addresses.add(address)
                try:
                    disasm = ghidra.disassemble_function(address)
                except GhidraHttpError:
                    continue
                disasm_text = str(disasm).lower()
                if "cop2" not in disasm_text and "rtpt" not in disasm_text and "rtps" not in disasm_text:
                    continue
                result["gte_functions"].append({
                    "address": address,
                    "function": match,
                    "disasm": disasm,
                    "gte_opcodes": _extract_gte_opcodes(disasm),
                    "source": f"static_search:{query}",
                })

    opcode_set: set[str] = set()
    for fn in result["gte_functions"]:
        for opcode in list(fn.get("gte_opcodes", []) or []):
            opcode_set.add(str(opcode))
    result["gte_opcodes_found"] = sorted(opcode_set)

    exploration_candidates: list[dict[str, Any]] = []
    for item in list(result["draw_calls"])[:seed_limit]:
        if isinstance(item, dict) and item.get("address"):
            address = _normalize_address(item.get("address"))
            if not _is_runtime_psx_address(address):
                continue
            exploration_candidates.append({
                "address": address,
                "source": f"draw_call:{item.get('query')}",
                "name": item.get("name"),
                "depth": 0,
                "edge_kind": "seed",
                "parent_address": None,
            })
    for item in list(result["gte_functions"])[:seed_limit]:
        if isinstance(item, dict) and item.get("address"):
            address = _normalize_address(item.get("address"))
            if not _is_runtime_psx_address(address):
                continue
            exploration_candidates.append({
                "address": address,
                "source": item.get("source"),
                "name": _extract_name(item.get("function", {})),
                "depth": 0,
                "edge_kind": "seed",
                "parent_address": None,
            })

    explored_seen: set[str] = set()
    queue = list(exploration_candidates)
    while queue:
        candidate = queue.pop(0)
        address = _normalize_address(candidate.get("address"))
        if not address or address in explored_seen or not _is_runtime_psx_address(address):
            continue
        explored_seen.add(address)
        explored = _summarize_function(
            ghidra=ghidra,
            address=address,
            source=str(candidate.get("source") or "exploration"),
            depth=int(candidate.get("depth", 0) or 0),
            edge_kind=str(candidate.get("edge_kind") or "seed"),
            parent_address=_normalize_address(candidate.get("parent_address")),
        )
        if explored is None:
            continue
        result["explored_functions"].append(explored)
        if explored.get("parent_address"):
            result["exploration_edges"].append({
                "from": explored.get("parent_address"),
                "to": explored.get("address"),
                "kind": explored.get("edge_kind"),
                "depth": explored.get("depth"),
            })
        hints = list(explored.get("analysis_hints", []) or [])
        if "uses_gte" in hints and "touches_ot" in hints:
            result["link_hypotheses"].append({
                "address": address,
                "kind": "possible_direct_gte_to_ot_or_packet",
                "reason": "Same function shows GTE usage and OT/packet hints.",
            })
        elif "uses_gte" in hints and "packet_or_poly_logic" in hints:
            result["link_hypotheses"].append({
                "address": address,
                "kind": "possible_gte_to_poly_builder",
                "reason": "Same function shows GTE usage and polygon/packet logic.",
            })
        elif "touches_ot" in hints and "packet_or_poly_logic" in hints:
            result["link_hypotheses"].append({
                "address": address,
                "kind": "possible_ot_submit_or_builder",
                "reason": "Function appears to manage OT and polygon/packet building.",
            })
        next_depth = int(explored.get("depth", 0) or 0) + 1
        if next_depth > branch_depth:
            continue
        for neighbor in list(explored.get("xrefs_to_addresses", []) or [])[:branch_fanout]:
            if neighbor not in explored_seen and _is_runtime_psx_address(neighbor):
                queue.append({
                    "address": neighbor,
                    "source": f"branch_from:{address}",
                    "depth": next_depth,
                    "edge_kind": "xref_to",
                    "parent_address": address,
                })
        for neighbor in list(explored.get("xrefs_from_addresses", []) or [])[:branch_fanout]:
            if neighbor not in explored_seen and _is_runtime_psx_address(neighbor):
                queue.append({
                    "address": neighbor,
                    "source": f"branch_to:{address}",
                    "depth": next_depth,
                    "edge_kind": "xref_from",
                    "parent_address": address,
                })

    if result["gte_functions"]:
        result["recommendations"].append("cross_reference_runtime_and_ghidra")
        if result["gte_opcodes_found"]:
            result["recommendations"].append("use_static_gte_opcode_inventory")
        if result["explored_functions"]:
            result["recommendations"].append("follow_branch_hierarchy_from_explored_functions")
        if result["exploration_edges"]:
            result["recommendations"].append("review_branch_edges_for_pipeline_relationships")
    else:
        result["recommendations"].append("use_scene_runtime_tools")

    log_mod.log(
        "p2_gte",
        "done",
        ghidra_available=result["ghidra_available"],
        fn_count=len(result["gte_functions"]),
        draw_calls=len(result["draw_calls"]),
    )
    return result
