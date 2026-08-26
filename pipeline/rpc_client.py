"""LSP-style JSON-RPC 2.0 client for dlss-compositor-cli --rpc-stdio."""
from __future__ import annotations

import json
import subprocess
from typing import Any


class RpcError(RuntimeError):
    def __init__(self, message: str, payload: dict[str, Any] | None = None) -> None:
        super().__init__(message)
        self.payload = payload or {}


def encode_frame(payload: dict[str, Any]) -> bytes:
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    header = f"Content-Length: {len(body)}\r\n\r\n".encode("ascii")
    return header + body


def read_frame(stream: Any) -> dict[str, Any]:
    content_length = 0
    while True:
        line = stream.readline()
        if not line:
            raise RpcError("RPC stdout closed")
        if isinstance(line, bytes):
            text = line.decode("utf-8", errors="replace")
        else:
            text = line
        stripped = text.strip()
        if not stripped:
            break
        if stripped.lower().startswith("content-length:"):
            content_length = int(stripped.split(":", 1)[1].strip())
    if content_length <= 0:
        raise RpcError("RPC frame missing Content-Length")
    body = stream.read(content_length)
    if isinstance(body, str):
        raw = body
    else:
        raw = body.decode("utf-8")
    try:
        return json.loads(raw)
    except json.JSONDecodeError as exc:
        raise RpcError(f"RPC JSON parse failed: {exc}") from exc


class StdioRpcClient:
    def __init__(self, proc: subprocess.Popen[bytes]) -> None:
        self.proc = proc
        self._next_id = 1

    def request(self, method: str, params: dict[str, Any] | None = None, timeout: float = 30.0) -> dict[str, Any]:
        if self.proc.stdin is None or self.proc.stdout is None:
            raise RpcError("RPC pipes are not connected")
        req_id = self._next_id
        self._next_id += 1
        frame = encode_frame(
            {
                "jsonrpc": "2.0",
                "id": req_id,
                "method": method,
                "params": params or {},
            }
        )
        self.proc.stdin.write(frame)
        self.proc.stdin.flush()
        response = read_frame(self.proc.stdout)
        if "error" in response and response["error"] is not None:
            err = response["error"]
            if isinstance(err, dict):
                raise RpcError(str(err.get("message", err)), err)
            raise RpcError(str(err), {"error": err})
        result = response.get("result", {})
        if isinstance(result, dict) and result.get("ok") is False:
            raise RpcError(str(result.get("error", result)), result)
        return result if isinstance(result, dict) else {"result": result}

    def close(self) -> None:
        if self.proc.stdin and not self.proc.stdin.closed:
            try:
                self.proc.stdin.close()
            except OSError:
                pass
