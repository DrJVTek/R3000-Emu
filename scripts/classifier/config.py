"""Configuration loader for the classifier orchestrator.

Precedence (highest → lowest):
    1. CLI args (handled in __main__.py, not here)
    2. Environment variables (CLASSIFIER_<SECTION>_<KEY> in UPPER_SNAKE)
    3. YAML file passed via --config (or default scripts/classifier/config.yaml)
    4. Hardcoded defaults in :class:`Config`

PyYAML is required for YAML parsing.  If it's not installed and no YAML file
is passed, we fall back to the hardcoded defaults and warn.
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Any, Optional


PROJECT_ROOT = Path(__file__).resolve().parents[2]  # scripts/classifier/ -> repo root


@dataclass
class EmuConfig:
    exe: str = str(PROJECT_ROOT / "lib" / "Release" / "r3000_emu.exe")
    # Legacy bridge path kept for compatibility/docs; the classifier now talks
    # directly to r3000_emu.exe --mcp-stdio.
    mcp_bridge: str = str(PROJECT_ROOT / "lib" / "Release" / "r3000_mcp.exe")
    bios: str = str(PROJECT_ROOT / "bios" / "ps1_bios.bin")
    # Legacy TCP port kept for compatibility/docs; ignored by native stdio mode.
    port: int = 9742
    boot_timeout_s: int = 60
    # paused = pause after BIOS hands off to the loaded EXE/menu runtime.
    # pause_immediate = stop at session start before any real progress.
    launch_mode: str = "paused"
    pause_pc: int = 0xBFC00000
    step_chunk: int = 50000
    observe_steps: int = 20000
    track_runtime_modules: bool = False


@dataclass
class GhidraConfig:
    base_url: str = "http://localhost:8080"
    required: bool = False  # if True, orchestrator errors when Ghidra unreachable


@dataclass
class LlmConfig:
    provider: str = "openrouter"
    model: str = "qwen/qwen3.6-plus-preview:free"
    api_key_env: str = "OPENROUTER_API_KEY"
    api_base: str = ""
    temperature: float = 0.0
    max_tokens: int = 4000
    enabled: bool = True


@dataclass
class PlaybookConfig:
    path: str = str(PROJECT_ROOT / "docs" / "PSX_RENDER_LOOP_PLAYBOOK.md")


@dataclass
class OutputConfig:
    profiles_dir: str = str(PROJECT_ROOT / "psx3dprof")
    reports_dir: str = str(PROJECT_ROOT / "logs" / "classifier_reports")
    jsonl_dir: str = str(PROJECT_ROOT / "logs" / "classifier")
    memory_dir: str = str(PROJECT_ROOT / "logs" / "classifier_memory")


@dataclass
class WorkflowConfig:
    static_first: bool = True
    resume_after_static: bool = True
    analysis_passes: int = 3
    ghidra_branch_depth: int = 2
    ghidra_branch_fanout: int = 6
    ghidra_static_seed_limit: int = 16
    signal_seek_enabled: bool = True
    signal_seek_max_passes: int = 3
    signal_seek_min_score: float = 4.0
    dynamic_probe_enabled: bool = True
    dynamic_probe_buttons: str = "start,cross"
    max_dynamic_probes: int = 2
    min_confidence: float = 0.70
    gte_trap_enabled: bool = True
    gte_trap_max_frames: int = 300
    gte_trap_frame_chunk: int = 30
    gte_trap_buttons: str = "start,cross,triangle,circle"
    gte_trap_max_pcs: int = 12
    bios_gte_trap_enabled: bool = True
    bios_gte_trap_max_steps: int = 100_000_000
    bios_gte_trap_max_pcs: int = 8


@dataclass
class TtsConfig:
    enabled: bool = True
    command_template: str = ""
    voice: str = "nova"
    language: str = "auto"


@dataclass
class Config:
    emu: EmuConfig = field(default_factory=EmuConfig)
    ghidra: GhidraConfig = field(default_factory=GhidraConfig)
    llm: LlmConfig = field(default_factory=LlmConfig)
    playbook: PlaybookConfig = field(default_factory=PlaybookConfig)
    output: OutputConfig = field(default_factory=OutputConfig)
    workflow: WorkflowConfig = field(default_factory=WorkflowConfig)
    tts: TtsConfig = field(default_factory=TtsConfig)

    def to_dict(self) -> dict[str, Any]:
        return asdict(self)


def _apply_yaml_overrides(cfg: Config, yaml_path: Path) -> None:
    try:
        import yaml  # type: ignore
    except ImportError:
        import sys
        sys.stderr.write(
            f"[config] PyYAML not installed; ignoring {yaml_path}\n"
            "         pip install pyyaml  OR  use --no-config\n"
        )
        return
    if not yaml_path.exists():
        return
    raw = yaml.safe_load(yaml_path.read_text(encoding="utf-8")) or {}
    _assign_nested(cfg, raw)


def _apply_env_overrides(cfg: Config) -> None:
    """Env var format: CLASSIFIER_<SECTION>_<KEY>, case-insensitive."""
    mapping = {
        "EMU_PORT":         ("emu", "port", int),
        "EMU_EXE":          ("emu", "exe", str),
        "EMU_BIOS":         ("emu", "bios", str),
        "EMU_TIMEOUT":      ("emu", "boot_timeout_s", int),
        "EMU_LAUNCH_MODE":  ("emu", "launch_mode", str),
        "EMU_PAUSE_PC":     ("emu", "pause_pc", lambda v: int(v, 0)),
        "EMU_STEP_CHUNK":   ("emu", "step_chunk", int),
        "EMU_OBSERVE_STEPS":("emu", "observe_steps", int),
        "EMU_TRACK_RUNTIME_MODULES": ("emu", "track_runtime_modules", _to_bool),
        "GHIDRA_URL":       ("ghidra", "base_url", str),
        "GHIDRA_REQUIRED":  ("ghidra", "required", _to_bool),
        "LLM_PROVIDER":     ("llm", "provider", str),
        "LLM_MODEL":        ("llm", "model", str),
        "LLM_API_BASE":     ("llm", "api_base", str),
        "LLM_ENABLED":      ("llm", "enabled", _to_bool),
        "LLM_TEMPERATURE":  ("llm", "temperature", float),
        "LLM_MAX_TOKENS":   ("llm", "max_tokens", int),
        "PLAYBOOK_PATH":    ("playbook", "path", str),
        "WORKFLOW_STATIC_FIRST": ("workflow", "static_first", _to_bool),
        "WORKFLOW_RESUME_AFTER_STATIC": ("workflow", "resume_after_static", _to_bool),
        "WORKFLOW_ANALYSIS_PASSES": ("workflow", "analysis_passes", int),
        "WORKFLOW_GHIDRA_BRANCH_DEPTH": ("workflow", "ghidra_branch_depth", int),
        "WORKFLOW_GHIDRA_BRANCH_FANOUT": ("workflow", "ghidra_branch_fanout", int),
        "WORKFLOW_GHIDRA_STATIC_SEED_LIMIT": ("workflow", "ghidra_static_seed_limit", int),
        "WORKFLOW_SIGNAL_SEEK_ENABLED": ("workflow", "signal_seek_enabled", _to_bool),
        "WORKFLOW_SIGNAL_SEEK_MAX_PASSES": ("workflow", "signal_seek_max_passes", int),
        "WORKFLOW_SIGNAL_SEEK_MIN_SCORE": ("workflow", "signal_seek_min_score", float),
        "WORKFLOW_DYNAMIC_PROBE": ("workflow", "dynamic_probe_enabled", _to_bool),
        "WORKFLOW_DYNAMIC_PROBE_BUTTONS": ("workflow", "dynamic_probe_buttons", str),
        "WORKFLOW_MAX_DYNAMIC_PROBES": ("workflow", "max_dynamic_probes", int),
        "WORKFLOW_MIN_CONFIDENCE": ("workflow", "min_confidence", float),
        "WORKFLOW_GTE_TRAP_ENABLED":     ("workflow", "gte_trap_enabled", _to_bool),
        "WORKFLOW_GTE_TRAP_MAX_FRAMES":  ("workflow", "gte_trap_max_frames", int),
        "WORKFLOW_GTE_TRAP_FRAME_CHUNK": ("workflow", "gte_trap_frame_chunk", int),
        "WORKFLOW_GTE_TRAP_BUTTONS":     ("workflow", "gte_trap_buttons", str),
        "WORKFLOW_GTE_TRAP_MAX_PCS":          ("workflow", "gte_trap_max_pcs", int),
        "WORKFLOW_BIOS_GTE_TRAP_ENABLED":     ("workflow", "bios_gte_trap_enabled", _to_bool),
        "WORKFLOW_BIOS_GTE_TRAP_MAX_STEPS":   ("workflow", "bios_gte_trap_max_steps", int),
        "WORKFLOW_BIOS_GTE_TRAP_MAX_PCS":     ("workflow", "bios_gte_trap_max_pcs", int),
        "TTS_ENABLED":      ("tts", "enabled", _to_bool),
        "TTS_COMMAND":      ("tts", "command_template", str),
        "TTS_VOICE":        ("tts", "voice", str),
        "TTS_LANGUAGE":     ("tts", "language", str),
    }
    for env_key, (section, field_name, caster) in mapping.items():
        val = os.environ.get(f"CLASSIFIER_{env_key}")
        if val is None:
            continue
        try:
            setattr(getattr(cfg, section), field_name, caster(val))
        except Exception as exc:
            import sys
            sys.stderr.write(f"[config] ignoring bad env CLASSIFIER_{env_key}={val!r}: {exc}\n")


def _to_bool(s: str) -> bool:
    return s.strip().lower() in ("1", "true", "yes", "on")


def _assign_nested(cfg: Config, raw: dict[str, Any]) -> None:
    for section, values in raw.items():
        section_obj = getattr(cfg, section, None)
        if section_obj is None or not isinstance(values, dict):
            continue
        for k, v in values.items():
            if hasattr(section_obj, k):
                setattr(section_obj, k, v)


def load(yaml_path: Optional[Path] = None, skip_env: bool = False) -> Config:
    """Load a Config, applying YAML then env overrides on top of defaults."""
    cfg = Config()
    if yaml_path is None:
        default_yaml = Path(__file__).parent / "config.yaml"
        if default_yaml.exists():
            yaml_path = default_yaml
    if yaml_path is not None:
        _apply_yaml_overrides(cfg, yaml_path)
    if not skip_env:
        _apply_env_overrides(cfg)
    return cfg
