"""Structured note helpers for low-context multi-pass analysis."""

from __future__ import annotations

from typing import Any


def add_note(ctx: dict[str, Any], stage: str, kind: str, summary: str, **details: Any) -> None:
    notes = ctx.setdefault("analysis_notes", [])
    entry = {
        "stage": stage,
        "kind": kind,
        "summary": summary,
    }
    if details:
        entry["details"] = details
    notes.append(entry)


def top_notes(ctx: dict[str, Any], limit: int = 24) -> list[dict[str, Any]]:
    notes = ctx.get("analysis_notes", [])
    if not isinstance(notes, list):
        return []
    return [n for n in notes if isinstance(n, dict)][:limit]
