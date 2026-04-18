"""CLI entrypoint for the render-loop classifier orchestrator."""

from __future__ import annotations

import argparse
import os
import sys
import time
from pathlib import Path

from . import __version__
from . import config as cfg_mod
from . import log as log_mod
from .orchestrator import ClassifierOrchestrator


def _build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="python -m scripts.classifier",
        description="Automated PSX render-loop classifier.",
    )
    p.add_argument("--version", action="version", version=f"classifier {__version__}")
    p.add_argument("--game", required=True, help="Game ID (e.g. SCUS-94300, TREX).  Used for log + profile filenames.")
    src = p.add_mutually_exclusive_group(required=False)
    src.add_argument("--cd", help="Path to CD image (.cue/.bin).  Loaded via --cd=...")
    src.add_argument("--exe", help="Path to PS-EXE.  Loaded via --load=...")
    p.add_argument("--devkit-hle", action="store_true", help="Pass --devkit-hle to the emu (PS-EXE mode).")
    p.add_argument("--hle", action="store_true", help="Pass --hle to the emu.")
    p.add_argument("--config", type=Path, default=None, help="Path to config YAML.  Defaults to scripts/classifier/config.yaml")
    p.add_argument("--no-config", action="store_true", help="Ignore the YAML config and use hardcoded defaults + env only.")
    p.add_argument("--no-launch", action="store_true",
                   help="Deprecated with native stdio MCP. The classifier now launches and owns its emu session.")
    p.add_argument("--port", type=int, default=None,
                   help="Deprecated legacy bridge option. Ignored by the native stdio MCP workflow.")
    p.add_argument("--boot-timeout", type=int, default=None, help="Seconds to wait for the emu to leave BIOS.")
    p.add_argument("--launch-mode", choices=("paused", "pause_after_exe_load", "pause_immediate", "run_to_ram"), default=None,
                   help="paused/pause_after_exe_load = stop when the game EXE/menu context is loaded; "
                        "pause_immediate = stop right at startup; run_to_ram = reach runtime before continuing.")
    p.add_argument("--pause-pc", default=None, help="PC used for coarse paused launch, e.g. 0xBFC00000.")
    p.add_argument("--track-runtime-modules", action="store_true",
                   help="Enable coarse runtime code-region transition tracking in the emulator for this run.")
    p.add_argument("--llm-provider", default=None, help="Override llm.provider (openrouter, ollama, openai_like, ...).")
    p.add_argument("--llm-model", default=None, help="Override llm.model.")
    p.add_argument("--llm-api-base", default=None, help="Override llm.api_base for OpenAI-compatible backends.")
    p.add_argument("--disable-llm", action="store_true", help="Run discovery/report/profile without contacting an LLM.")
    return p


def main(argv: list[str] | None = None) -> int:
    args = _build_argparser().parse_args(argv)

    cfg = cfg_mod.Config() if args.no_config else cfg_mod.load(args.config)

    # Apply CLI overrides on top of the merged config.
    if args.port is not None:
        log_mod.log("session", "deprecated_arg", name="port", value=args.port)
    if args.boot_timeout is not None:
        cfg.emu.boot_timeout_s = args.boot_timeout
    if args.launch_mode is not None:
        cfg.emu.launch_mode = args.launch_mode
    if args.pause_pc is not None:
        cfg.emu.pause_pc = int(args.pause_pc, 0)
    if args.track_runtime_modules:
        cfg.emu.track_runtime_modules = True
    if args.llm_provider is not None:
        cfg.llm.provider = args.llm_provider
    if args.llm_model is not None:
        cfg.llm.model = args.llm_model
    if args.llm_api_base is not None:
        cfg.llm.api_base = args.llm_api_base
    if args.disable_llm:
        cfg.llm.enabled = False
    if args.no_launch:
        sys.stderr.write("error: --no-launch is no longer supported with the native stdio MCP backend\n")
        return 2
    if not cfg.llm.enabled:
        sys.stderr.write("error: LLM classification is required by project policy; --disable-llm is not supported\n")
        return 2

    provider = (cfg.llm.provider or "").strip().lower()
    api_base = (cfg.llm.api_base or "").strip()
    needs_key = provider in {"openrouter", "openai", "anthropic"}
    local_compatible = provider in {"lmstudio", "ollama", "openai_like"} and bool(api_base)
    if needs_key and not local_compatible:
        key_env = (cfg.llm.api_key_env or "").strip() or "OPENROUTER_API_KEY"
        if not os.environ.get(key_env):
            sys.stderr.write(
                f"error: missing required API key env {key_env} for provider {cfg.llm.provider}\n"
            )
            return 2

    # Prepare log paths rooted in cfg.output.jsonl_dir.
    ts = time.strftime("%Y%m%dT%H%M%S")
    jsonl_dir = Path(cfg.output.jsonl_dir)
    jsonl_dir.mkdir(parents=True, exist_ok=True)
    session_log = jsonl_dir / f"{args.game}-{ts}.jsonl"
    log_mod.init(session_log)
    log_mod.log("session", "start", game=args.game, version=__version__,
                args=vars(args), config=cfg.to_dict())

    # Compose the ROM argv for the emu launcher.
    rom_args: list[str] = []
    if args.cd:
        rom_args.append(f"--cd={args.cd}")
    if args.exe:
        rom_args.append(f"--load={args.exe}")
    if args.devkit_hle:
        rom_args.append("--devkit-hle")
    if args.hle:
        rom_args.append("--hle")

    if not rom_args:
        log_mod.log("session", "error", reason="no rom source specified")
        sys.stderr.write("error: either --cd or --exe is required\n")
        return 2

    try:
        orchestrator = ClassifierOrchestrator(
            cfg=cfg,
            game_id=args.game,
            rom_args=rom_args,
            no_launch=args.no_launch,
            session_log=session_log,
            emu_log=jsonl_dir / f"{args.game}-{ts}.emu.log",
        )
        result = orchestrator.run()
    except Exception as exc:
        log_mod.log("session", "fatal_error", error=str(exc))
        sys.stderr.write(f"error: classifier failed: {exc}\n")
        log_mod.close()
        return 1

    log_mod.log("session", "done", result=result, log_file=str(session_log))
    sys.stderr.write("\nclassifier complete.\n")
    if "report_path" in result:
        sys.stderr.write(f"  report      : {result['report_path']}\n")
    if "profile_path" in result:
        sys.stderr.write(f"  profile     : {result['profile_path']}\n")
    sys.stderr.write(f"  session log : {session_log}\n")
    log_mod.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
