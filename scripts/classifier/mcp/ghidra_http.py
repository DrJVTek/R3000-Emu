"""Tiny HTTP client for the GhidraMCP helper plugin."""

from __future__ import annotations

import json
import re
from dataclasses import dataclass
from typing import Any
from urllib.error import URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


class GhidraHttpError(RuntimeError):
    pass


@dataclass
class GhidraHttpClient:
    base_url: str
    timeout_s: float = 5.0
    _functions_cache: list[dict[str, str]] | None = None

    def _request(self, path: str, params: dict[str, Any] | None = None) -> Any:
        url = self.base_url.rstrip("/") + path
        if params:
            url += "?" + urlencode(params)
        req = Request(url, headers={"Accept": "application/json, text/plain;q=0.9"})
        try:
            with urlopen(req, timeout=self.timeout_s) as resp:
                payload = resp.read().decode("utf-8", errors="replace")
        except URLError as exc:
            raise GhidraHttpError(str(exc)) from exc

        payload = payload.strip()
        if not payload:
            return {}
        try:
            return json.loads(payload)
        except json.JSONDecodeError:
            return payload

    def ping(self) -> bool:
        try:
            self.list_functions(limit=1)
            return True
        except GhidraHttpError:
            return False

    def list_functions(self, limit: int | None = None) -> Any:
        params = {"limit": limit} if limit is not None else None
        return self._request("/list_functions", params)

    def _parse_functions_payload(self, payload: Any) -> list[dict[str, str]]:
        if isinstance(payload, list):
            out: list[dict[str, str]] = []
            for item in payload:
                if isinstance(item, dict):
                    name = str(item.get("name") or item.get("function_name") or item.get("label") or "")
                    address = str(item.get("address") or item.get("entry") or item.get("addr") or "")
                    if name and address:
                        out.append({"name": name, "address": address})
            return out
        if isinstance(payload, dict):
            for key in ("functions", "items", "results", "data"):
                if isinstance(payload.get(key), list):
                    return self._parse_functions_payload(payload.get(key))
        text = str(payload or "")
        out = []
        for raw_line in text.splitlines():
            line = raw_line.strip()
            if not line:
                continue
            match = re.match(r"^(?P<name>.+?)\s+at\s+(?P<addr>[0-9A-Fa-fx]+)$", line)
            if not match:
                continue
            out.append({"name": match.group("name").strip(), "address": match.group("addr").strip()})
        return out

    def list_functions_parsed(self, limit: int | None = None, refresh: bool = False) -> list[dict[str, str]]:
        if refresh or self._functions_cache is None:
            self._functions_cache = self._parse_functions_payload(self.list_functions(limit=None))
        if limit is None:
            return list(self._functions_cache)
        return list(self._functions_cache[:limit])

    def search_functions(self, query: str, limit: int | None = None) -> Any:
        params = {"query": query}
        if limit is not None:
            params["limit"] = limit
        for path in ("/search_functions_by_name", "/search_functions"):
            try:
                return self._request(path, params)
            except GhidraHttpError:
                continue
        query_lower = query.strip().lower()
        if not query_lower:
            return []
        matches = []
        for item in self.list_functions_parsed():
            if query_lower in str(item.get("name", "")).lower():
                matches.append(item)
                if limit is not None and len(matches) >= limit:
                    break
        if matches:
            return matches
        raise GhidraHttpError("search_functions endpoint unavailable")

    def disassemble_function(self, address: str) -> Any:
        return self._request("/disassemble_function", {"address": address})

    def decompile_function(self, address: str) -> Any:
        for path in ("/decompile_function_by_address", "/decompile_function"):
            try:
                return self._request(path, {"address": address})
            except GhidraHttpError:
                continue
        raise GhidraHttpError("decompile endpoint unavailable")

    def get_xrefs_to(self, address: str) -> Any:
        for path in ("/xrefs_to", "/get_xrefs_to"):
            try:
                return self._request(path, {"address": address})
            except GhidraHttpError:
                continue
        raise GhidraHttpError("xrefs_to endpoint unavailable")

    def get_xrefs_from(self, address: str) -> Any:
        for path in ("/xrefs_from", "/get_xrefs_from"):
            try:
                return self._request(path, {"address": address})
            except GhidraHttpError:
                continue
        raise GhidraHttpError("xrefs_from endpoint unavailable")

    def get_function_by_address(self, address: str) -> Any:
        for path in ("/get_function_by_address", "/function_by_address"):
            try:
                return self._request(path, {"address": address})
            except GhidraHttpError:
                continue
        raise GhidraHttpError("function_by_address endpoint unavailable")
