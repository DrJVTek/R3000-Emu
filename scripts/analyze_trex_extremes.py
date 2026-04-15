#!/usr/bin/env python3
"""
Correlate TREX extreme GPU DMA packets with GTE extreme projection logs.

Inputs:
- system.log: looks for `EXTREME_RTPT`, `EXTREME_RTPS`, and `EXTREME_*_V0/V1/V2`
- gpu_dma.log: looks for `EXTREME primitive` and `EXTREME_CORR`

Goal:
- show whether the final GPU triangle already matches an extreme GTE projection
- surface the input 3D vertices and projected SXY used by the faithful GTE
- help spot "GPU packet extreme but no GTE extreme nearby" cases
"""

from __future__ import annotations

import argparse
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional


RE_GTE_MAIN = re.compile(
    r"EXTREME_(RTPT|RTPS) #(?P<idx>\d+) .*?"
    r"sxy0=\((?P<sx0>-?\d+),(?P<sy0>-?\d+)\) "
    r"sxy1=\((?P<sx1>-?\d+),(?P<sy1>-?\d+)\) "
    r"sxy2=\((?P<sx2>-?\d+),(?P<sy2>-?\d+)\)"
)

RE_GTE_VERTEX = re.compile(
    r"EXTREME_(RTPT|RTPS)_V(?P<v>\d) #(?P<idx>\d+) "
    r"in=\((?P<vx>-?\d+),(?P<vy>-?\d+),(?P<vz>-?\d+)\) .*?"
    r"sz3=(?P<sz3>\d+) h=(?P<h>\d+) q=0x(?P<q>[0-9A-Fa-f]+) .*?"
    r"sxy_preclamp=\((?P<sx>-?\d+),(?P<sy>-?\d+)\)"
)

RE_DMA_TRI = re.compile(
    r"EXTREME primitive .*?tri=\((?P<x0>-?\d+),(?P<y0>-?\d+)\)\((?P<x1>-?\d+),(?P<y1>-?\d+)\)\((?P<x2>-?\d+),(?P<y2>-?\d+)\)"
)

RE_DMA_CORR = re.compile(
    r"EXTREME_CORR source_pc=0x(?P<pc>[0-9A-Fa-f]+) swap=(?P<swap>\d) "
    r"v3d=\((?P<v0x>-?\d+),(?P<v0y>-?\d+),(?P<v0z>-?\d+)\)"
    r"\((?P<v1x>-?\d+),(?P<v1y>-?\d+),(?P<v1z>-?\d+)\)"
    r"\((?P<v2x>-?\d+),(?P<v2y>-?\d+),(?P<v2z>-?\d+)\) "
    r"sz=\((?P<sz0>\d+),(?P<sz1>\d+),(?P<sz2>\d+)\)"
)


def parse_triplet(prefix: str, match: re.Match[str]) -> List[tuple[int, int]]:
    return [
        (int(match.group(f"{prefix}0")), int(match.group(f"{prefix.replace('x', 'y')}0"))),
        (int(match.group(f"{prefix}1")), int(match.group(f"{prefix.replace('x', 'y')}1"))),
        (int(match.group(f"{prefix}2")), int(match.group(f"{prefix.replace('x', 'y')}2"))),
    ]


@dataclass
class GteExtreme:
    idx: int
    kind: str
    sxy: List[tuple[int, int]]
    vertex_lines: List[str] = field(default_factory=list)
    source_pcs: List[int] = field(default_factory=list)


@dataclass
class DmaExtreme:
    tri: List[tuple[int, int]]
    raw_line: str
    corr_line: Optional[str] = None
    source_pc: Optional[int] = None


def load_gte_events(system_log: Path) -> List[GteExtreme]:
    events: List[GteExtreme] = []
    by_idx: dict[tuple[str, int], GteExtreme] = {}

    for line in system_log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = RE_GTE_MAIN.search(line)
        if m:
            idx = int(m.group("idx"))
            evt = GteExtreme(
                idx=idx,
                kind=m.group(1),
                sxy=[
                    (int(m.group("sx0")), int(m.group("sy0"))),
                    (int(m.group("sx1")), int(m.group("sy1"))),
                    (int(m.group("sx2")), int(m.group("sy2"))),
                ],
            )
            events.append(evt)
            by_idx[(evt.kind, idx)] = evt
            continue

        mv = RE_GTE_VERTEX.search(line)
        if mv:
            key = (mv.group(1), int(mv.group("idx")))
            evt = by_idx.get(key)
            if evt:
                evt.vertex_lines.append(line.strip())
    return events


def load_dma_events(gpu_dma_log: Path) -> List[DmaExtreme]:
    events: List[DmaExtreme] = []
    last: Optional[DmaExtreme] = None
    for line in gpu_dma_log.read_text(encoding="utf-8", errors="replace").splitlines():
        m = RE_DMA_TRI.search(line)
        if m:
            last = DmaExtreme(
                tri=[
                    (int(m.group("x0")), int(m.group("y0"))),
                    (int(m.group("x1")), int(m.group("y1"))),
                    (int(m.group("x2")), int(m.group("y2"))),
                ],
                raw_line=line.strip(),
            )
            events.append(last)
            continue
        mc = RE_DMA_CORR.search(line)
        if mc and last is not None and last.corr_line is None:
            last.corr_line = line.strip()
            last.source_pc = int(mc.group("pc"), 16)
    return events


def tri_distance(a: List[tuple[int, int]], b: List[tuple[int, int]]) -> int:
    return sum(abs(ax - bx) + abs(ay - by) for (ax, ay), (bx, by) in zip(a, b))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--system-log", default=r"E:\Projects\github\Live\PSXVR\logs\system.log")
    parser.add_argument("--gpu-dma-log", default=r"E:\Projects\github\Live\PSXVR\logs\gpu_dma.log")
    parser.add_argument("--limit", type=int, default=10)
    args = parser.parse_args()

    system_log = Path(args.system_log)
    gpu_dma_log = Path(args.gpu_dma_log)
    if not system_log.exists():
        raise SystemExit(f"Missing system log: {system_log}")
    if not gpu_dma_log.exists():
        raise SystemExit(f"Missing gpu_dma log: {gpu_dma_log}")

    gte_events = load_gte_events(system_log)
    dma_events = load_dma_events(gpu_dma_log)

    print(f"GTE extreme events: {len(gte_events)}")
    print(f"DMA extreme events: {len(dma_events)}")
    print()

    for idx, dma_evt in enumerate(dma_events[-args.limit :], 1):
        print(f"DMA[{idx}] tri={dma_evt.tri}")
        print(f"  raw: {dma_evt.raw_line}")
        if dma_evt.corr_line:
            print(f"  corr: {dma_evt.corr_line}")
        else:
            print("  corr: unavailable")

        best: Optional[GteExtreme] = None
        best_score: Optional[int] = None
        for evt in gte_events:
            score = tri_distance(dma_evt.tri, evt.sxy)
            if best_score is None or score < best_score:
                best = evt
                best_score = score

        if best is None:
            print("  match: no GTE extreme event found")
            print()
            continue

        print(f"  best_gte: {best.kind} #{best.idx} sxy={best.sxy} distance={best_score}")
        for line in best.vertex_lines:
            print(f"    {line}")
        print()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
