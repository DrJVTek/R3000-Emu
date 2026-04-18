"""Native stdio MCP client for r3000_emu.exe --mcp-stdio."""

from __future__ import annotations

import json
import subprocess
import time
from pathlib import Path
from typing import Any, Optional

from .. import log as log_mod


class EmuMcpError(RuntimeError):
    """Raised when the emulator MCP backend returns an error or the protocol breaks."""


class EmuMcp:
    """Launches r3000_emu.exe in native MCP stdio mode and speaks MCP directly."""

    def __init__(
        self,
        emu_exe: str,
        bios: str,
        rom_args: Optional[list[str]] = None,
        startup_wait_ms: int = 200,
        extra_args: Optional[list[str]] = None,
    ) -> None:
        if not Path(emu_exe).exists():
            raise EmuMcpError(f"emulator not found: {emu_exe}")
        self._emu_exe = emu_exe
        self._bios = bios
        self._rom_args = list(rom_args or [])
        self._extra_args = list(extra_args or [])
        self._proc: Optional[subprocess.Popen[bytes]] = None
        self._next_id = 1
        self._startup_wait_ms = startup_wait_ms

    def __enter__(self) -> "EmuMcp":
        self.start()
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        self.stop()

    def start(self) -> None:
        cmd = [
            self._emu_exe,
            "--mcp-stdio",
            "--log-level=error",
            "--emu-log-level=error",
            f"--bios={self._bios}",
            *self._rom_args,
            *self._extra_args,
        ]
        log_mod.log("mcp_emu", "emu_start", cmd=cmd)
        self._proc = subprocess.Popen(
            cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            bufsize=0,
        )
        time.sleep(self._startup_wait_ms / 1000.0)
        if self._proc.poll() is not None:
            stderr_tail = self._drain_stderr()
            raise EmuMcpError(
                f"native MCP emu exited immediately (code={self._proc.returncode}). "
                f"stderr: {stderr_tail!r}"
            )

    def stop(self) -> None:
        if self._proc is None:
            return
        try:
            self._proc.stdin.close()
        except Exception:
            pass
        try:
            self._proc.wait(timeout=3)
        except Exception:
            self._proc.kill()
        self._proc = None
        log_mod.log("mcp_emu", "emu_stop")

    def _write_frame(self, body: dict[str, Any]) -> None:
        if self._proc is None or self._proc.stdin is None:
            raise EmuMcpError("emu MCP not running")
        json_body = json.dumps(body, ensure_ascii=False, separators=(",", ":"))
        payload = json_body.encode("utf-8")
        header = f"Content-Length: {len(payload)}\r\n\r\n".encode("ascii")
        self._proc.stdin.write(header)
        self._proc.stdin.write(payload)
        self._proc.stdin.flush()

    def _read_frame(self, timeout_s: float = 30.0) -> dict[str, Any]:
        if self._proc is None or self._proc.stdout is None:
            raise EmuMcpError("emu MCP not running")
        stdout = self._proc.stdout
        deadline = time.monotonic() + timeout_s
        content_len = -1
        while True:
            if time.monotonic() > deadline:
                raise EmuMcpError(f"timeout reading MCP frame headers (>{timeout_s}s)")
            line = stdout.readline()
            if not line:
                raise EmuMcpError("emu MCP closed stdout while reading headers")
            if not line.startswith(b"Content-Length:"):
                continue
            s = line.decode("ascii", errors="replace").rstrip("\r\n")
            try:
                content_len = int(s.split(":", 1)[1].strip())
            except ValueError:
                raise EmuMcpError(f"malformed Content-Length header: {s!r}")
            blank = stdout.readline()
            if blank not in (b"\r\n", b"\n", b""):
                # Tolerate stray log/noise by continuing from this header anyway.
                pass
            break
        if content_len < 0:
            raise EmuMcpError("missing Content-Length header")
        body = b""
        while len(body) < content_len:
            chunk = stdout.read(content_len - len(body))
            if not chunk:
                raise EmuMcpError("emu MCP closed stdout mid-body")
            body += chunk
        try:
            return json.loads(body.decode("utf-8"))
        except json.JSONDecodeError as exc:
            raise EmuMcpError(f"malformed JSON body: {exc}")

    def _drain_stderr(self) -> str:
        if self._proc is None or self._proc.stderr is None:
            return ""
        try:
            return self._proc.stderr.read(4096).decode("utf-8", errors="replace")
        except Exception:
            return ""

    def _rpc(self, method: str, params: Optional[dict] = None) -> dict[str, Any]:
        req_id = self._next_id
        self._next_id += 1
        body = {"jsonrpc": "2.0", "id": req_id, "method": method}
        if params is not None:
            body["params"] = params
        log_mod.log("mcp_emu", "rpc_send", method=method, id=req_id,
                    params_keys=list(params.keys()) if isinstance(params, dict) else None)
        self._write_frame(body)
        while True:
            resp = self._read_frame()
            if resp.get("id") != req_id:
                continue
            if "error" in resp:
                err = resp["error"]
                log_mod.log("mcp_emu", "rpc_error", method=method, id=req_id,
                            code=err.get("code"), message=err.get("message"))
                raise EmuMcpError(f"{method} failed: {err.get('message')} (code={err.get('code')})")
            log_mod.log("mcp_emu", "rpc_ok", method=method, id=req_id)
            return resp.get("result", {})

    def _notify(self, method: str, params: Optional[dict] = None) -> None:
        body = {"jsonrpc": "2.0", "method": method}
        if params is not None:
            body["params"] = params
        self._write_frame(body)
        log_mod.log("mcp_emu", "notify_sent", method=method)

    def initialize(self, client_name: str = "classifier.py", client_version: str = "0.1.0") -> dict[str, Any]:
        result = self._rpc("initialize", {
            "protocolVersion": "2024-11-05",
            "clientInfo": {"name": client_name, "version": client_version},
            "capabilities": {},
        })
        self._notify("notifications/initialized")
        return result

    def list_tools(self) -> list[dict[str, Any]]:
        result = self._rpc("tools/list")
        return result.get("tools", [])

    def call_tool(self, name: str, arguments: Optional[dict] = None) -> dict[str, Any]:
        return self._rpc("tools/call", {
            "name": name,
            "arguments": arguments or {},
        })

    def call_tool_text(self, name: str, arguments: Optional[dict] = None) -> str:
        result = self.call_tool(name, arguments)
        content = result.get("content", [])
        for item in content:
            if item.get("type") == "text":
                return item.get("text", "")
        return ""

    def call_tool_json(self, name: str, arguments: Optional[dict] = None) -> Any:
        result = self.call_tool(name, arguments)
        structured = result.get("structuredContent")
        if structured is not None:
            return structured
        text = ""
        for item in result.get("content", []):
            if item.get("type") == "text":
                text = item.get("text", "")
                break
        try:
            return json.loads(text)
        except json.JSONDecodeError as exc:
            raise EmuMcpError(f"{name} returned non-JSON text: {exc}; text={text[:200]!r}")
