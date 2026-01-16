"""
Python client for MIRenderer viewer using simple ZMQ REQ/REP.

Protocol:
- Server: REP at tcp://127.0.0.1:25957
- Client: REQ
- Request: single JSON frame {"cmd": str, "args": {}}
- Reply: JSON meta frame; export replies add binary frames (one per export)
- Single client, blocking one-request-at-a-time; no reqId/identity.
"""
import json
from typing import Tuple, Dict, Optional, List, Any
import zmq
import numpy as np


class ZmqClientError(RuntimeError):
    pass


def _pixel_format_to_dtype_channels(fmt: str) -> Tuple[np.dtype, int]:
    fmt_upper = fmt.upper()
    if fmt_upper in {"R16G16B16A16_FLOAT"}:
        return np.float16, 4
    if fmt_upper in {"R16G16_FLOAT"}:
        return np.float16, 2
    if fmt_upper in {"R32G32B32A32_FLOAT"}:
        return np.float32, 4
    if fmt_upper in {"R32G32B32_FLOAT"}:
        return np.float32, 3
    if fmt_upper in {"R32G32_FLOAT"}:
        return np.float32, 2
    if fmt_upper in {"R32_FLOAT", "D32_FLOAT"}:
        return np.float32, 1
    if fmt_upper in {"R32G32B32A32_UINT"}:
        return np.uint32, 4
    if fmt_upper in {"R32G32_UINT"}:
        return np.uint32, 2
    if fmt_upper in {"R32_UINT"}:
        return np.uint32, 1
    if fmt_upper in {"R8_UNORM", "R8_UINT"}:
        return np.uint8, 1
    if fmt_upper in {"R8G8_UNORM"}:
        return np.uint8, 2
    if fmt_upper in {"R8G8B8A8_UNORM", "R8G8B8A8_SRGB", "B8G8R8A8_UNORM", "B8G8R8A8_SRGB"}:
        return np.uint8, 4
    raise ZmqClientError(f"Unsupported pixel format: {fmt}")


def _decode_export_frames(meta: Dict[str, Any], frames: List[bytes]) -> List[Dict[str, Any]]:
    exports_meta = meta.get("exports", []) or []
    if len(exports_meta) != len(frames):
        raise ZmqClientError(f"Frame count mismatch: meta has {len(exports_meta)}, frames has {len(frames)}")

    decoded: List[Dict[str, Any]] = []
    for frame_meta, frame_bytes in zip(exports_meta, frames):
        width = int(frame_meta.get("width", 0))
        height = int(frame_meta.get("height", 0))
        fmt = str(frame_meta.get("format", ""))
        dtype, channels = _pixel_format_to_dtype_channels(fmt)

        expected_size = width * height * channels * np.dtype(dtype).itemsize
        if expected_size != len(frame_bytes):
            raise ZmqClientError(
                f"Frame size mismatch for {frame_meta.get('name', '')}: expected {expected_size}, got {len(frame_bytes)}"
            )

        array = np.frombuffer(frame_bytes, dtype=dtype).reshape((height, width, channels))
        decoded.append({
            "name": frame_meta.get("name", ""),
            "width": width,
            "height": height,
            "format": fmt,
            "bytes_per_pixel": frame_meta.get("bytes_per_pixel"),
            "size_bytes": len(frame_bytes),
            "data": array,
        })
    return decoded


class ViewerClient:
    def __init__(self, endpoint: str = "tcp://127.0.0.1:25957",
                 context: Optional[zmq.Context] = None,
                 recv_timeout_ms: int = 10000, send_timeout_ms: int = 5000):
        self.endpoint = endpoint
        self.ctx = context or zmq.Context.instance()
        self.socket = None
        self.recv_timeout_ms = recv_timeout_ms
        self.send_timeout_ms = send_timeout_ms
        self._create_socket()

    def _create_socket(self):
        if self.socket:
            try:
                self.socket.close(linger=0)
            except Exception:
                pass
        self.socket = self.ctx.socket(zmq.REQ)
        self.socket.connect(self.endpoint)
        self.socket.setsockopt(zmq.LINGER, 0)
        self.socket.setsockopt(zmq.RCVTIMEO, self.recv_timeout_ms)
        self.socket.setsockopt(zmq.SNDTIMEO, self.send_timeout_ms)

    def close(self):
        try:
            if self.socket:
                self.socket.close(linger=0)
        except Exception:
            pass

    def _send_recv(self, cmd: str, args: Optional[Dict] = None, expect_payload: bool = False,
                   timeout_ms: Optional[int] = None) -> Tuple[Dict, List[bytes]]:
        if timeout_ms is None:
            timeout_ms = self.recv_timeout_ms
        self.socket.setsockopt(zmq.RCVTIMEO, int(timeout_ms))

        payload = {"cmd": cmd, "args": (args or {})}
        try:
            self.socket.send_string(json.dumps(payload))
            parts = self.socket.recv_multipart()
        except zmq.Again:
            self._create_socket()
            raise TimeoutError(f"No reply to '{cmd}' within {timeout_ms} ms")
        except Exception as e:
            self._create_socket()
            raise ZmqClientError(f"Send/recv error: {e}")

        if not parts:
            raise ZmqClientError("Empty reply frames")

        try:
            meta = json.loads(parts[0].decode("utf-8"))
        except Exception as e:
            raise ZmqClientError(f"Bad JSON meta: {e}")

        if not meta.get("ok", False):
            raise ZmqClientError(f"Server error: {meta}")

        if expect_payload:
            return meta, parts[1:]
        else:
            return meta, []

    # Public API
    def ping(self) -> Dict:
        meta, _ = self._send_recv("ping")
        return meta

    def get_status(self) -> Dict:
        meta, _ = self._send_recv("get_status")
        return meta

    def console_execute(self, line: str) -> Dict:
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def set_suspended(self, value: bool) -> Dict:
        meta, _ = self._send_recv("set_suspended", {"value": bool(value)})
        return meta

    def get_cvar(self, name: str) -> Dict:
        meta, _ = self._send_recv("get_cvar", {"name": name})
        return meta

    def render_and_export_current_frame(self, types: Optional[List[str]] = None, timeout_sec: float = 60.0) -> Dict[str, Any]:
        if types is None:
            types = ["radiance"]
        meta, frames = self._send_recv(
            "render_and_export_current_frame",
            {"types": types},
            expect_payload=True,
            timeout_ms=int(timeout_sec * 1000)
        )
        decoded_exports = _decode_export_frames(meta, frames)
        return {"meta": meta, "exports": decoded_exports}


__all__ = ["ViewerClient", "ZmqClientError"]
