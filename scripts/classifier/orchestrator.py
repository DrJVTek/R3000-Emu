"""Classifier orchestration layer."""

from __future__ import annotations

import time
from pathlib import Path

from . import log as log_mod
from .analysis_memory import write_snapshot
from .config import Config
from .mcp.emu_stdio import EmuMcp
from .mcp.ghidra_http import GhidraHttpClient
from .mcp.llm_litellm import LlmClient
from .narrator import Narrator
from .notes import add_note
from .phases import p1_boot, p2_gte_discovery, p3_loop_discovery, p3b_gte_trap, p4_classify, p5_profile
from .report import build_markdown, write_report


class ClassifierOrchestrator:
    def __init__(
        self,
        cfg: Config,
        game_id: str,
        rom_args: list[str],
        no_launch: bool,
        session_log: Path,
        emu_log: Path,
    ) -> None:
        self.cfg = cfg
        self.game_id = game_id
        self.rom_args = rom_args
        self.no_launch = no_launch
        self.session_log = session_log
        self.emu_log = emu_log
        self.llm = LlmClient(cfg.llm)
        self.narrator = Narrator(cfg.tts)
        self.ghidra = GhidraHttpClient(cfg.ghidra.base_url)

    def _write_memory_snapshot(self, ctx: dict, stage: str) -> None:
        try:
            path = write_snapshot(self.cfg.output.memory_dir, self.game_id, stage, ctx)
            ctx.setdefault("memory_snapshots", []).append({"stage": stage, "path": path})
            log_mod.log("orchestrator", "memory_snapshot", stage=stage, path=path)
        except Exception as exc:
            log_mod.log("orchestrator", "memory_snapshot_failed", stage=stage, error=str(exc))

    def _launch_rom_args(self) -> list[str]:
        args = list(self.rom_args)
        launch_mode = (self.cfg.emu.launch_mode or "paused").strip().lower()
        if self.cfg.emu.track_runtime_modules:
            args.append("--track-runtime-modules")
        if launch_mode in ("pause_immediate", "pause-now", "pause_now", "boot-pause"):
            args.append(f"--stop-on-pc=0x{self.cfg.emu.pause_pc:08X}")
        return args

    def _maybe_resume_runtime(self, ctx: dict) -> None:
        if not ctx["boot_info"].get("paused"):
            return
        self.narrator.speak("resume", "Je reprends l'émulateur pour collecter les signaux runtime du pipeline d'affichage.")
        launch_mode = str(ctx["boot_info"].get("launch_mode", ""))
        if launch_mode == "pause_immediate":
            # Clear the --stop-on-pc breakpoint so emu.resume can advance past it.
            try:
                ctx["emu"].call_tool_json("emu.clear_breakpoint_pc", {"pc": int(self.cfg.emu.pause_pc)})
            except Exception:
                pass
            ctx["boot_info"]["paused"] = False
            ctx["resume_info"] = {"pc": ctx["boot_info"].get("pc", "0x00000000")}
            return
        elif launch_mode == "pause_after_exe_load":
            resume_info = p1_boot.advance_runtime(
                emu=ctx["emu"],
                max_steps=2_000_000,  # PSX ≈ 1.1M instr/frame; observe_steps (20K) too small
                max_frames=1,
            )
        else:
            resume_info = p1_boot.resume_until_runtime(
                emu=ctx["emu"],
                timeout_s=self.cfg.emu.boot_timeout_s,
                step_chunk=self.cfg.emu.step_chunk,
                clear_pause_pc=self.cfg.emu.pause_pc,
            )
        ctx["resume_info"] = resume_info
        ctx["boot_info"]["pc"] = resume_info["pc"]
        ctx["boot_info"]["paused"] = False

    def _readiness_score(self, discovery: dict) -> float:
        readiness = discovery.get("readiness", {}) if isinstance(discovery, dict) else {}
        try:
            return float(readiness.get("score", 0.0) or 0.0)
        except Exception:
            return 0.0

    def _classify_current_state(self, ctx: dict) -> None:
        self.narrator.speak("classify", "Je synthétise les preuves de cette passe pour estimer le mode de rendu.")
        ctx["classification"] = p4_classify.run(ctx)

    def _top_confidence(self, ctx: dict) -> float:
        rules = ctx.get("classification", {}).get("rules", [])
        try:
            return float(rules[0]["confidence"]) if rules else 0.0
        except Exception:
            return 0.0

    def _advance_and_refresh(self, ctx: dict, reason: str, pass_index: int) -> None:
        self.narrator.speak("signal", f"{reason} J'avance un peu l'émulateur et je réévalue la scène.")
        advance = p1_boot.advance_runtime(
            emu=ctx["emu"],
            max_steps=2_000_000,  # observe_steps (20K) too small for even 1 frame
            max_frames=1,
        )
        ctx.setdefault("analysis_passes", []).append({
            "pass_index": pass_index,
            "action": "advance_runtime",
            "pc_after": advance.get("pc_after"),
        })
        ctx["dynamic_discovery"] = p3_loop_discovery.run(ctx)

    def _run_named_button_probes(self, ctx: dict, pass_index: int) -> None:
        if not self.cfg.workflow.dynamic_probe_enabled:
            return
        buttons = [x.strip() for x in self.cfg.workflow.dynamic_probe_buttons.split(",") if x.strip()]
        if not buttons:
            return
        emu = ctx["emu"]
        for idx, button_name in enumerate(buttons[: self.cfg.workflow.max_dynamic_probes], start=1):
            self.narrator.speak("probe", f"Je tente une interaction pad générique: {button_name}.")
            probe = emu.call_tool_json(
                "emu.step_with_pad_observation",
                {
                    "names_csv": button_name,
                    "hold_steps": 1,
                    "observe_steps": self.cfg.emu.observe_steps,
                    "max_groups": 12,
                    "max_targets": 6,
                },
            )
            ctx.setdefault("dynamic_probes", []).append(probe)
            ctx.setdefault("analysis_passes", []).append({
                "pass_index": pass_index,
                "action": "pad_probe",
                "button": button_name,
            })
            log_mod.log("orchestrator", "dynamic_probe", button=button_name, index=idx, pass_index=pass_index)
        ctx["dynamic_discovery"] = p3_loop_discovery.run(ctx)

    def _seek_runtime_signal_if_needed(self, ctx: dict) -> None:
        if not self.cfg.workflow.signal_seek_enabled:
            return
        max_passes = max(0, int(self.cfg.workflow.signal_seek_max_passes))
        min_score = float(self.cfg.workflow.signal_seek_min_score)
        discovery = ctx.get("dynamic_discovery", {}) or {}
        readiness = discovery.get("readiness", {}) if isinstance(discovery, dict) else {}
        score = float(readiness.get("score", 0.0) or 0.0)
        passes = 0
        while passes < max_passes and score < min_score:
            passes += 1
            self.narrator.speak(
                "signal",
                f"Le signal rendu est encore faible, j'avance un peu l'émulateur pour chercher une scène plus exploitable. Passe {passes}.",
            )
            ctx.setdefault("signal_seek_passes", []).append({
                "pass_index": passes,
                "score_before": score,
            })
            # Advance more frames when score is still 0 (game in loading screen)
            seek_frames = 10 if score == 0.0 else 1
            p1_boot.advance_runtime(
                emu=ctx["emu"],
                max_steps=2_000_000 * seek_frames,
                max_frames=seek_frames,
            )
            ctx["dynamic_discovery"] = p3_loop_discovery.run(ctx, advance=False)
            discovery = ctx["dynamic_discovery"]
            readiness = discovery.get("readiness", {}) if isinstance(discovery, dict) else {}
            score = float(readiness.get("score", 0.0) or 0.0)
            ctx["signal_seek_passes"][-1]["score_after"] = score
            ctx["signal_seek_passes"][-1]["reasons_after"] = list(readiness.get("reasons", []) or [])
            log_mod.log(
                "orchestrator",
                "signal_seek_pass",
                pass_index=passes,
                score_after=score,
                reasons=readiness.get("reasons", []),
            )

    def _run_gte_trap_if_needed(self, ctx: dict) -> None:
        if not self.cfg.workflow.gte_trap_enabled:
            return
        score = self._readiness_score(ctx.get("dynamic_discovery", {}))
        if score >= float(self.cfg.workflow.signal_seek_min_score):
            return
        self.narrator.speak(
            "gte_trap",
            "Le signal reste faible après la recherche. Je lance le GTE trap pour trouver les PCs 3D par injection pad.",
        )
        ctx["gte_trap"] = p3b_gte_trap.run(ctx)
        log_mod.log(
            "orchestrator",
            "gte_trap_done",
            triggered=ctx["gte_trap"]["triggered"],
            discovered=ctx["gte_trap"]["discovered_pcs"],
            frames=ctx["gte_trap"]["frames_done"],
        )
        if ctx["gte_trap"]["triggered"]:
            ctx["static_discovery"].setdefault("gte_trap_pcs", []).extend(
                ctx["gte_trap"]["discovered_pcs"]
            )

    def _run_multi_pass_analysis(self, ctx: dict) -> None:
        total_passes = max(1, int(self.cfg.workflow.analysis_passes))
        ctx.setdefault("analysis_passes", [])
        ctx["dynamic_discovery"] = p3_loop_discovery.run(ctx)
        self._seek_runtime_signal_if_needed(ctx)
        self._run_gte_trap_if_needed(ctx)
        self._classify_current_state(ctx)

        for pass_index in range(2, total_passes + 1):
            score = self._readiness_score(ctx["dynamic_discovery"])
            top_conf = self._top_confidence(ctx)
            if score >= float(self.cfg.workflow.signal_seek_min_score) and top_conf >= self.cfg.workflow.min_confidence:
                log_mod.log("orchestrator", "analysis_pass_stop", pass_index=pass_index, reason="ready_and_confident", score=score, confidence=top_conf)
                break

            if score < float(self.cfg.workflow.signal_seek_min_score):
                self._advance_and_refresh(
                    ctx,
                    f"Le signal reste faible (score {score:.2f}).",
                    pass_index,
                )
            else:
                self._run_named_button_probes(ctx, pass_index)

            self._classify_current_state(ctx)

            updated_score = self._readiness_score(ctx["dynamic_discovery"])
            updated_conf = self._top_confidence(ctx)
            ctx.setdefault("analysis_passes", []).append({
                "pass_index": pass_index,
                "action": "classify",
                "readiness_score": updated_score,
                "confidence": updated_conf,
            })
            log_mod.log(
                "orchestrator",
                "analysis_pass_done",
                pass_index=pass_index,
                readiness_score=updated_score,
                confidence=updated_conf,
            )

    def run(self) -> dict:
        ctx: dict = {
            "config": self.cfg,
            "game_id": self.game_id,
            "session_log": str(self.session_log),
            "started_at": time.strftime("%Y-%m-%dT%H:%M:%S"),
            "llm": self.llm,
        }

        if self.no_launch:
            raise RuntimeError("--no-launch is no longer supported with the native stdio MCP backend")

        self.narrator.speak("boot", "Je prépare un boot contrôlé, d'abord pausé pour laisser la place à l'analyse statique.")
        with EmuMcp(emu_exe=self.cfg.emu.exe, bios=self.cfg.emu.bios, rom_args=self._launch_rom_args()) as emu:
            emu.initialize(client_name="classifier.run")
            ctx["emu"] = emu
            boot_info = p1_boot.run(
                emu=emu,
                boot_timeout_s=self.cfg.emu.boot_timeout_s,
                launch_mode=self.cfg.emu.launch_mode,
                pause_pc=self.cfg.emu.pause_pc,
                step_chunk=self.cfg.emu.step_chunk,
            )
            ctx["boot_info"] = boot_info
            add_note(
                ctx,
                "boot",
                "boot_state",
                "Controlled boot reached analysis entry point.",
                launch_mode=boot_info.get("launch_mode"),
                pc=boot_info.get("pc"),
                boot_exe=(boot_info.get("boot_exe_info", {}) or {}).get("boot_path"),
            )
            self._write_memory_snapshot(ctx, "boot")

            if not self.cfg.workflow.static_first and self.cfg.workflow.resume_after_static:
                self._maybe_resume_runtime(ctx)

            self.narrator.speak("static", "Je corrèle Ghidra et les MCP pour repérer GTE, DrawOT et les premiers points d'entrée du rendu.")
            ctx["ghidra"] = self.ghidra
            ctx["static_discovery"] = p2_gte_discovery.run(ctx)
            add_note(
                ctx,
                "static",
                "static_discovery",
                "Static discovery pass completed.",
                ghidra_available=ctx["static_discovery"].get("ghidra_available"),
                draw_calls=len(ctx["static_discovery"].get("draw_calls", [])),
                gte_functions=len(ctx["static_discovery"].get("gte_functions", [])),
                explored_functions=len(ctx["static_discovery"].get("explored_functions", [])),
                branch_edges=len(ctx["static_discovery"].get("exploration_edges", [])),
                gte_opcodes=list(ctx["static_discovery"].get("gte_opcodes_found", []) or [])[:12],
            )
            for item in list(ctx["static_discovery"].get("link_hypotheses", []) or [])[:8]:
                if isinstance(item, dict):
                    add_note(
                        ctx,
                        "static",
                        "link_hypothesis",
                        str(item.get("reason", "Static branch analysis produced a link hypothesis.")),
                        address=item.get("address"),
                        kind=item.get("kind"),
                    )
            self._write_memory_snapshot(ctx, "static")

            if self.cfg.ghidra.required and not ctx["static_discovery"].get("ghidra_available", False):
                raise RuntimeError("Ghidra is required by config but the HTTP bridge is unavailable")

            if self.cfg.workflow.static_first and self.cfg.workflow.resume_after_static:
                self._maybe_resume_runtime(ctx)

            self.narrator.speak("dynamic", "Je collecte maintenant les indices runtime pour confirmer les vraies boucles d'affichage.")
            self._run_multi_pass_analysis(ctx)
            add_note(
                ctx,
                "dynamic",
                "dynamic_discovery",
                "Dynamic multi-pass analysis completed.",
                readiness_score=(ctx["dynamic_discovery"].get("readiness", {}) or {}).get("score", 0.0),
                candidate_pcs=list(ctx["dynamic_discovery"].get("candidate_pcs", []) or [])[:12],
            )
            for item in list(ctx["dynamic_discovery"].get("relation_hypotheses", []) or [])[:8]:
                if isinstance(item, dict):
                    add_note(
                        ctx,
                        "dynamic",
                        "relation_hypothesis",
                        str(item.get("reason", "Dynamic correlation produced a relation hypothesis.")),
                        address=item.get("address"),
                        kind=item.get("kind"),
                    )
            self._write_memory_snapshot(ctx, "dynamic")

            profile_info = p5_profile.run(ctx)
            ctx.update(profile_info)
            add_note(
                ctx,
                "profile",
                "profile_generated",
                "Generated psx3d profile from current classification.",
                profile_path=ctx.get("profile_path"),
                rule_count=profile_info.get("rule_count", 0),
            )
            self._write_memory_snapshot(ctx, "profile")

            report_md = build_markdown(ctx)
            report_path = Path(self.cfg.output.reports_dir) / f"{self.game_id}-{time.strftime('%Y%m%dT%H%M%S')}.md"
            ctx["report_path"] = write_report(report_path, report_md)
            self._write_memory_snapshot(ctx, "report")

        self.narrator.speak("done", "Analyse terminée. Le rapport et le profile généré sont prêts.")
        return {
            "boot_info": ctx["boot_info"],
            "static_discovery": {
                "ghidra_available": ctx["static_discovery"].get("ghidra_available", False),
                "gte_functions": len(ctx["static_discovery"].get("gte_functions", [])),
            },
            "dynamic_discovery": {
                "candidate_pcs": len(ctx["dynamic_discovery"].get("candidate_pcs", [])),
                "runtime_module_events": len(
                    ((ctx["dynamic_discovery"].get("runtime", {}) or {}).get("runtime_modules", {}) or {}).get("events", [])
                ),
                "readiness_score": (ctx["dynamic_discovery"].get("readiness", {}) or {}).get("score", 0.0),
                "analysis_passes": len(ctx.get("analysis_passes", []) or []),
            },
            "classification": {
                "rule_count": len(ctx["classification"].get("rules", [])),
                "summary": ctx["classification"].get("summary", ""),
            },
            "profile_path": ctx["profile_path"],
            "report_path": ctx["report_path"],
        }
