"""Provider-agnostic LLM wrapper via litellm."""

from __future__ import annotations

import json
import os
import time
from typing import Any
from urllib.error import URLError
from urllib.request import Request, urlopen

from ..config import LlmConfig


class LlmError(RuntimeError):
    pass


class LlmClient:
    def __init__(self, cfg: LlmConfig) -> None:
        self._cfg = cfg
        self._cached_discovered_model: str | None = None

    @property
    def enabled(self) -> bool:
        return bool(self._cfg.enabled)

    def _effective_model(self) -> str:
        model = self._cfg.model.strip()
        provider = self._cfg.provider.strip().lower()
        if provider in {"lmstudio", "openai_like", "openai-compatible", "openai_compatible"}:
            discovered = self._discover_openai_compatible_model()
            if discovered and (
                not model
                or model == "qwen/qwen3.6-plus-preview:free"
                or model.startswith("qwen/")
            ):
                model = discovered
        if provider in {"ollama"} and "/" not in model:
            return f"ollama/{model}"
        if provider in {"lmstudio", "openai_like", "openai-compatible", "openai_compatible"} and "/" not in model:
            return f"openai/{model}"
        return model

    def _default_local_api_key(self) -> str | None:
        provider = self._cfg.provider.strip().lower()
        if provider in {"lmstudio", "openai_like", "openai-compatible", "openai_compatible"}:
            return "lm-studio"
        return None

    def _discover_openai_compatible_model(self) -> str | None:
        if self._cached_discovered_model is not None:
            return self._cached_discovered_model
        api_base = (self._cfg.api_base or "").strip().rstrip("/")
        if not api_base:
            return None
        url = f"{api_base}/models"
        request = Request(url, headers={"Authorization": "Bearer lm-studio"})
        try:
            with urlopen(request, timeout=5) as response:
                payload = json.loads(response.read().decode("utf-8"))
        except (URLError, TimeoutError, json.JSONDecodeError, OSError):
            return None
        data = payload.get("data")
        if not isinstance(data, list):
            return None
        for item in data:
            if not isinstance(item, dict):
                continue
            model_id = item.get("id")
            if isinstance(model_id, str) and model_id.strip():
                self._cached_discovered_model = model_id.strip()
                return self._cached_discovered_model
        return None

    def complete_text(self, system_prompt: str, user_prompt: str, retries: int = 2) -> str:
        if not self.enabled:
            raise LlmError("LLM disabled by config")
        try:
            from litellm import completion
        except ImportError as exc:
            raise LlmError("litellm is not installed") from exc

        kwargs: dict[str, Any] = {
            "model": self._effective_model(),
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_prompt},
            ],
            "temperature": self._cfg.temperature,
            "max_tokens": self._cfg.max_tokens,
            "timeout": 180,
        }
        if self._cfg.api_base.strip():
            kwargs["api_base"] = self._cfg.api_base.strip()
        api_key = os.environ.get(self._cfg.api_key_env or "")
        if api_key:
            kwargs["api_key"] = api_key
        else:
            default_local_key = self._default_local_api_key()
            if default_local_key:
                kwargs["api_key"] = default_local_key

        last_error = None
        for attempt in range(retries + 1):
            try:
                resp = completion(**kwargs)
                content = resp["choices"][0]["message"]["content"]
                if isinstance(content, list):
                    content = "".join(str(part.get("text", "")) for part in content if isinstance(part, dict))
                return str(content).strip()
            except Exception as exc:
                last_error = exc
                if attempt >= retries:
                    break
                time.sleep(1.5 * (attempt + 1))
        raise LlmError(str(last_error))

    def complete_json(self, system_prompt: str, user_prompt: str) -> tuple[dict[str, Any], str]:
        text = self.complete_text(system_prompt, user_prompt)
        cleaned = text.strip()
        if cleaned.startswith("```"):
            lines = cleaned.splitlines()
            if lines:
                lines = lines[1:]
            while lines and lines[-1].strip().startswith("```"):
                lines.pop()
            cleaned = "\n".join(lines).strip()
        try:
            return json.loads(cleaned), text
        except json.JSONDecodeError as exc:
            raise LlmError(f"LLM returned non-JSON output: {exc}: {text[:200]!r}")
