"""Structured JSONL logger for the classifier orchestrator.

Each call to `log(phase, event, **fields)` appends a single JSON line to the
session's log file (path fixed at init time).  Lines are UTF-8 with trailing
`\n`.  Timestamps are UTC ISO-8601 with `Z` suffix.

Also mirrors a subset of events to stderr for live visibility during a run,
without adding dependencies like rich/loguru.

Design goals:
    - No runtime cost beyond file append + JSON serialization.
    - Safe to call from anywhere; no global state beyond the module-level
      current-session sink.
    - Replayable: the JSONL file is self-contained; a single line is a self-
      describing event.
"""

from __future__ import annotations

import json
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Optional


_sink: Optional["JsonlSink"] = None


class JsonlSink:
    """Append-only JSONL writer for one classifier session."""

    def __init__(self, path: Path, mirror_stderr: bool = True) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        # Line-buffered text mode so each event is flushed immediately,
        # letting the user `tail -f` the log while the orchestrator runs.
        self._fh = path.open("a", encoding="utf-8", buffering=1)
        self._path = path
        self._mirror_stderr = mirror_stderr
        self._t0 = time.monotonic()

    def write(self, phase: str, event: str, **fields: Any) -> None:
        rec: dict[str, Any] = {
            "ts": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
            "mono_s": round(time.monotonic() - self._t0, 6),
            "phase": phase,
            "event": event,
        }
        rec.update(fields)
        line = json.dumps(rec, ensure_ascii=False, separators=(",", ":"))
        self._fh.write(line)
        self._fh.write("\n")
        if self._mirror_stderr:
            # Concise stderr mirror — just phase.event and a tiny preview of
            # the most-likely-interesting fields, so the user sees progress
            # without being flooded.
            preview_keys = ("pc", "pid", "fn_count", "loop_entry", "type",
                            "mode_kind", "confidence", "tool", "err")
            parts = []
            for k in preview_keys:
                if k in fields:
                    parts.append(f"{k}={fields[k]}")
            tail = " " + " ".join(parts) if parts else ""
            sys.stderr.write(f"[{phase}.{event}]{tail}\n")
            sys.stderr.flush()

    def close(self) -> None:
        try:
            self._fh.close()
        except Exception:
            pass

    @property
    def path(self) -> Path:
        return self._path


def init(path: Path, mirror_stderr: bool = True) -> JsonlSink:
    """Install a session-wide JSONL sink.  Subsequent ``log()`` calls route
    into it.  Safe to call multiple times (last init wins, previous is closed).
    """
    global _sink
    if _sink is not None:
        _sink.close()
    _sink = JsonlSink(path, mirror_stderr=mirror_stderr)
    return _sink


def log(phase: str, event: str, **fields: Any) -> None:
    """Emit a structured event.  No-op if ``init()`` was never called, but
    prints a warning to stderr so lost events are visible.
    """
    if _sink is None:
        sys.stderr.write(f"[WARN log uninit] {phase}.{event} {fields}\n")
        return
    _sink.write(phase, event, **fields)


def close() -> None:
    """Close the current sink.  Subsequent ``log()`` calls warn."""
    global _sink
    if _sink is not None:
        _sink.close()
        _sink = None
