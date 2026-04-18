"""Optional narration/TTS hooks for live runs."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from shlex import quote

from . import log as log_mod
from .config import TtsConfig


class Narrator:
    def __init__(self, cfg: TtsConfig) -> None:
        self._cfg = cfg
        self._project_root = Path(__file__).resolve().parents[2]
        self._tts_script = self._project_root / ".claude" / "hooks" / "tts.sh"

    def _to_bash_path(self, path: Path) -> str:
        text = str(path)
        if len(text) >= 3 and text[1:3] == ":\\":
            drive = text[0].lower()
            rest = text[3:].replace("\\", "/")
            return f"/mnt/{drive}/{rest}"
        return text.replace("\\", "/")

    def _build_project_tts_command(self, text: str) -> list[str]:
        if not self._tts_script.exists():
            raise FileNotFoundError(f"Missing TTS hook: {self._tts_script}")

        script_path = self._to_bash_path(self._tts_script)
        project_path = self._to_bash_path(self._project_root)
        home_dir = Path.home()
        piper_dir = home_dir / ".claude" / "piper-voices"
        exports = [f"export CLAUDE_PROJECT_DIR={quote(project_path)}"]
        if piper_dir.exists():
            exports.append(f"export PIPER_VOICES_DIR={quote(self._to_bash_path(piper_dir))}")
        lang_hint = ""
        if self._cfg.language and self._cfg.language.strip().lower() not in {"", "auto"}:
            lang_hint = self._cfg.language.strip()
        command = "; ".join(exports) + f"; {quote(script_path)} {quote(text)}"
        if lang_hint:
            command += f" {quote(lang_hint)}"
        return ["bash", "-lc", command]

    def speak(self, stage: str, text: str) -> None:
        log_mod.log("narration", "speak", stage=stage, text=text, voice=self._cfg.voice)
        if not self._cfg.enabled:
            return
        try:
            env = os.environ.copy()
            if self._cfg.command_template.strip():
                command = self._cfg.command_template.format(
                    text=text,
                    voice=self._cfg.voice,
                    lang=self._cfg.language,
                    stage=stage,
                )
                subprocess.Popen(command, shell=True, env=env)
                return

            subprocess.Popen(
                self._build_project_tts_command(text),
                env=env,
                cwd=str(self._project_root),
                creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
            )
        except Exception as exc:
            log_mod.log("narration", "tts_error", stage=stage, error=str(exc))
