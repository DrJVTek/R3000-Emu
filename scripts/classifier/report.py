"""Markdown report builder for classifier sessions."""

from __future__ import annotations

from pathlib import Path

from .notes import top_notes


def build_markdown(ctx: dict) -> str:
    boot = ctx.get("boot_info", {})
    static_discovery = ctx.get("static_discovery", {})
    dynamic_discovery = ctx.get("dynamic_discovery", {})
    classification = ctx.get("classification", {})
    rules = classification.get("rules", [])

    lines = [
        f"# Classifier Report — {ctx['game_id']}",
        "",
        "## Summary",
        "",
        f"- Launch mode: `{boot.get('launch_mode', 'unknown')}`",
        f"- Initial PC: `{boot.get('pc', 'unknown')}`",
        f"- Ghidra available: `{static_discovery.get('ghidra_available', False)}`",
        f"- Rules generated: `{len(rules)}`",
        "",
        "## Static Discovery",
        "",
        f"- Draw-call candidates: `{len(static_discovery.get('draw_calls', []))}`",
        f"- GTE function candidates: `{len(static_discovery.get('gte_functions', []))}`",
        f"- Explored Ghidra functions: `{len(static_discovery.get('explored_functions', []))}`",
        f"- Branch edges: `{len(static_discovery.get('exploration_edges', []))}`",
        "",
        "## Dynamic Discovery",
        "",
        f"- Candidate PCs: `{len(dynamic_discovery.get('candidate_pcs', []))}`",
        f"- Loop candidates with Ghidra correlation: `{len(dynamic_discovery.get('loop_candidates', []))}`",
        f"- Scene readiness score: `{(dynamic_discovery.get('readiness', {}) or {}).get('score', 0.0)}`",
        "",
        "## Classification",
        "",
        classification.get("summary", "_No summary_"),
        "",
    ]

    boot_exe = boot.get("boot_exe_info", {}) or {}
    if boot_exe:
        lines.extend([
            "## Boot EXE",
            "",
            f"- Source: `{boot_exe.get('source', '')}`",
            f"- Path: `{boot_exe.get('boot_path', '')}`",
            f"- Entry PC: `{boot_exe.get('entry_pc', 0)}`",
            f"- Loaded to RAM: `{boot_exe.get('loaded_to_ram', False)}`",
            f"- Reached entry PC: `{boot_exe.get('reached_entry_pc', False)}`",
            "",
        ])

    boot_hist = boot.get("boot_exe_history", {}) or {}
    events = boot_hist.get("events", []) if isinstance(boot_hist, dict) else []
    if events:
        lines.extend(["## Boot EXE History", ""])
        for ev in events:
            lines.append(
                f"- step `{ev.get('step_index', 0)}` `{ev.get('stage', '')}` "
                f"`{ev.get('source', '')}` pc=`{ev.get('pc', 0)}` entry=`{ev.get('entry_pc', 0)}` "
                f"path=`{ev.get('boot_path', '')}`"
            )
        lines.append("")

    runtime = dynamic_discovery.get("runtime", {}) if isinstance(dynamic_discovery, dict) else {}
    runtime_modules = runtime.get("runtime_modules", {}) if isinstance(runtime, dict) else {}
    module_events = runtime_modules.get("events", []) if isinstance(runtime_modules, dict) else []
    if module_events:
        lines.extend(["## Runtime Module History", ""])
        for ev in module_events:
            base = ev.get("base", 0)
            span = ev.get("span", 0)
            pc = ev.get("pc", 0)
            try:
                base_text = f"0x{int(base):08X}"
            except Exception:
                base_text = str(base)
            try:
                span_text = f"0x{int(span):X}"
            except Exception:
                span_text = str(span)
            try:
                pc_text = f"0x{int(pc):08X}"
            except Exception:
                pc_text = str(pc)
            lines.append(
                f"- step `{ev.get('step_index', 0)}` kind=`{ev.get('kind', '')}` "
                f"label=`{ev.get('label', '')}` pc=`{pc_text}` base=`{base_text}` "
                f"span=`{span_text}` reason=`{ev.get('reason', '')}`"
            )
        lines.append("")

    readiness = dynamic_discovery.get("readiness", {}) if isinstance(dynamic_discovery, dict) else {}
    if readiness:
        lines.extend(["## Scene Readiness", ""])
        lines.append(f"- Score: `{readiness.get('score', 0.0)}`")
        lines.append(f"- Ready: `{readiness.get('ready', False)}`")
        for reason in list(readiness.get("reasons", []) or []):
            lines.append(f"- Signal: `{reason}`")
        lines.append("")

    link_hypotheses = list(static_discovery.get("link_hypotheses", []) or [])
    if link_hypotheses:
        lines.extend(["## Static Link Hypotheses", ""])
        for item in link_hypotheses[:16]:
            if isinstance(item, dict):
                lines.append(
                    f"- kind=`{item.get('kind', '')}` address=`{item.get('address', '')}` reason=`{item.get('reason', '')}`"
                )
        lines.append("")

    relation_hypotheses = list(dynamic_discovery.get("relation_hypotheses", []) or [])
    if relation_hypotheses:
        lines.extend(["## Runtime Relation Hypotheses", ""])
        for item in relation_hypotheses[:16]:
            if isinstance(item, dict):
                lines.append(
                    f"- kind=`{item.get('kind', '')}` address=`{item.get('address', '')}` reason=`{item.get('reason', '')}`"
                )
        lines.append("")

    static_branch_edges = list(dynamic_discovery.get("static_branch_edges", []) or [])
    if static_branch_edges:
        lines.extend(["## Static Branch Edges", ""])
        for item in static_branch_edges[:24]:
            if isinstance(item, dict):
                lines.append(
                    f"- `{item.get('from', '')}` -> `{item.get('to', '')}` kind=`{item.get('kind', '')}` depth=`{item.get('depth', '')}`"
                )
        lines.append("")

    gte_trap = ctx.get("gte_trap")
    if gte_trap:
        lines.extend(["## GTE Trap", ""])
        lines.append(f"- Triggered: `{gte_trap.get('triggered', False)}`")
        lines.append(f"- Frames advanced: `{gte_trap.get('frames_done', 0)}`")
        for pc in list(gte_trap.get("discovered_pcs", []) or []):
            lines.append(f"- Discovered PC: `{pc}`")
        for op in list(gte_trap.get("top_ops", []) or [])[:8]:
            if isinstance(op, dict):
                lines.append(f"- GTE op: `{op.get('name', op.get('op', '?'))}` × {op.get('count', 0)}")
        for fn in list(gte_trap.get("ghidra_functions", []) or []):
            if isinstance(fn, dict):
                lines.append(
                    f"- Ghidra: `{fn.get('function', '')}` at `{fn.get('function_addr', '')}` (caller PC `{fn.get('pc', '')}`)"
                )
        lines.append("")

    signal_seek_passes = ctx.get("signal_seek_passes", []) or []
    if signal_seek_passes:
        lines.extend(["## Signal Seek Passes", ""])
        for entry in signal_seek_passes:
            lines.append(
                f"- pass `{entry.get('pass_index', 0)}` score `{entry.get('score_before', 0.0)}` -> "
                f"`{entry.get('score_after', 0.0)}` reasons=`{', '.join(entry.get('reasons_after', []) or [])}`"
            )
        lines.append("")

    analysis_passes = ctx.get("analysis_passes", []) or []
    if analysis_passes:
        lines.extend(["## Analysis Passes", ""])
        for entry in analysis_passes:
            bits = [f"pass `{entry.get('pass_index', 0)}`", f"action `{entry.get('action', '')}`"]
            if "button" in entry:
                bits.append(f"button `{entry.get('button')}`")
            if "pc_after" in entry:
                bits.append(f"pc_after `{entry.get('pc_after')}`")
            if "readiness_score" in entry:
                bits.append(f"readiness `{entry.get('readiness_score')}`")
            if "confidence" in entry:
                bits.append(f"confidence `{entry.get('confidence')}`")
            lines.append("- " + " ".join(bits))
        lines.append("")

    structured_notes = top_notes(ctx, limit=32)
    if structured_notes:
        lines.extend(["## Structured Notes", ""])
        for note in structured_notes:
            line = f"- [{note.get('stage', '')}/{note.get('kind', '')}] {note.get('summary', '')}"
            details = note.get("details", {})
            if isinstance(details, dict) and details:
                preview = ", ".join(f"{k}={v}" for k, v in list(details.items())[:6])
                if preview:
                    line += f" ({preview})"
            lines.append(line)
        lines.append("")

    if rules:
        lines.append("| Priority | Mode | Link Rule | Confidence |")
        lines.append("|---|---|---|---|")
        for index, rule in enumerate(rules, start=1):
            lines.append(
                f"| {index} | `{rule['mode_kind']}` | `{rule['link_rule']}` | {rule.get('confidence', 0.0):.2f} |"
            )
        lines.append("")

    notes = classification.get("notes", [])
    if notes:
        lines.extend(["## Notes", ""])
        for note in notes:
            lines.append(f"- {note}")
        lines.append("")

    lines.extend([
        "## Outputs",
        "",
        f"- Session log: `{ctx.get('session_log')}`",
        f"- Profile: `{ctx.get('profile_path', '')}`",
        "",
    ])
    memory_snapshots = ctx.get("memory_snapshots", []) or []
    if memory_snapshots:
        lines.extend(["## Memory Snapshots", ""])
        for item in memory_snapshots:
            lines.append(f"- `{item.get('stage', '')}` -> `{item.get('path', '')}`")
        lines.append("")
    return "\n".join(lines)


def write_report(path: str | Path, markdown: str) -> str:
    p = Path(path)
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(markdown, encoding="utf-8")
    return str(p)
