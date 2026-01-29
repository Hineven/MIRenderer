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
from pathlib import Path
import math


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

    def set_cvar(self, name: str, value: Any) -> Dict:
        # Use `s <cvar> <value>` console command to set cvar
        line = f"s {name} {value}"
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def render_and_export_current_frame(self, types: Optional[List[str]] = None, timeout_sec: float = 60.0) -> Dict[str, Any]:
        '''
        Render the current frame and export specified types of data.
        :return: A dictionary containing metadata and decoded export frames. Example:
        {"meta": {...}, "exports": [ { "name": str, "width": int, "height": int,
                                      "format": str, "bytes_per_pixel": int,
                                      "size_bytes": int, "data": np.ndarray }, ... ] }
        '''
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

    def load_gltf_abs_path(self, path: str | Path) -> Dict:
        meta, _ = self._send_recv("load_gltf_abs_path", {"path": str(path)})
        return meta

    def load_ply_abs_path(self, path: str | Path) -> Dict:
        meta, _ = self._send_recv("load_ply_abs_path", {"path": str(path)})
        return meta

    def remove_renderable_node(self, index: int) -> Dict:
        meta, _ = self._send_recv("remove_renderable_node", {"index": int(index)})
        return meta

    def set_transform(self, index: int, pos, rot_deg, scale) -> Dict:
        """Set world transform of a renderable via console command (uses degrees for rotation)."""
        def _vec3(v):
            if len(v) != 3:
                raise ValueError("expected length-3 iterable")
            return float(v[0]), float(v[1]), float(v[2])
        px, py, pz = _vec3(pos)
        rx, ry, rz = _vec3(rot_deg)
        sx, sy, sz = _vec3(scale)
        line = f"set_transform {int(index)} {px} {py} {pz} {rx} {ry} {rz} {sx} {sy} {sz}"
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def set_camera_pos(self, pos) -> Dict:
        """Set camera position using console command 'c pos'."""
        def _vec3(v):
            if len(v) != 3:
                raise ValueError("expected length-3 iterable")
            return float(v[0]), float(v[1]), float(v[2])
        px, py, pz = _vec3(pos)
        line = f"c pos {px} {py} {pz}"
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def set_camera_dir(self, direction) -> Dict:
        """Set camera direction (normalized if possible) using console command 'c dir'."""
        def _vec3(v):
            if len(v) != 3:
                raise ValueError("expected length-3 iterable")
            return float(v[0]), float(v[1]), float(v[2])
        dx, dy, dz = _vec3(direction)
        line = f"c dir {dx} {dy} {dz}"
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def set_camera_fovy(self, fovy_deg: float) -> Dict:
        """Set camera vertical FOV in degrees using console command 'c fovy'."""
        line = f"c fovy {float(fovy_deg)}"
        meta, _ = self._send_recv("console_execute", {"line": line})
        return meta

    def set_camera_dir_euler(self, euler_deg) -> Dict:
        """Set camera direction from Euler angles (degrees). Order: pitch(x), yaw(y), roll(z)."""
        if len(euler_deg) != 3:
            raise ValueError("expected length-3 iterable for euler_deg")
        pitch = math.radians(float(euler_deg[0]))
        yaw = math.radians(float(euler_deg[1]))
        roll = math.radians(float(euler_deg[2]))

        x, y, z = 0.0, 0.0, -1.0  # forward in view space
        cy, sy = math.cos(yaw), math.sin(yaw)
        x, y, z = cy * x + sy * z, y, -sy * x + cy * z  # yaw about Y
        cp, sp = math.cos(pitch), math.sin(pitch)
        x, y, z = x, cp * y - sp * z, sp * y + cp * z   # pitch about X
        cr, sr = math.cos(roll), math.sin(roll)
        x, y, z = cr * x - sr * y, sr * x + cr * y, z   # roll about Z

        norm = math.sqrt(x * x + y * y + z * z)
        if norm < 1e-6:
            raise ValueError("direction magnitude too small after euler conversion")
        dir_vec = (x / norm, y / norm, z / norm)
        return self.set_camera_dir(dir_vec)


__all__ = ["ViewerClient", "ZmqClientError", "_decode_export_frames"]
