"""Shared classifier taxonomy helpers."""

from __future__ import annotations

from difflib import get_close_matches


KNOWN_MODE_KINDS = (
    "ot_classic_rtpt",
    "paired_edge_rtpt_gt4",
    "direct_dma_submission",
    "chained_polygon_stream",
    "skinned_cpu_transform",
    "tmd_compiled",
    "subdivided_ft4_intpl_rtpt",
    "billboard_radial",
)

KNOWN_LINK_RULES = (
    "unknown",
    "packet_edge_pairs",
)


def normalize_mode_kind(value: str | None) -> tuple[str, bool]:
    if not value:
        return "unknown", False
    value = value.strip()
    if value in KNOWN_MODE_KINDS:
        return value, True
    match = get_close_matches(value, KNOWN_MODE_KINDS, n=1, cutoff=0.45)
    return (match[0], False) if match else ("unknown", False)


def normalize_link_rule(value: str | None) -> str:
    if not value:
        return "unknown"
    value = value.strip()
    if value in KNOWN_LINK_RULES:
        return value
    if value == "packet_segments":
        return "packet_edge_pairs"
    return "unknown"
