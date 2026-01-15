"""
Minimal Python client for the MIRenderer viewer ZMQ control channel.

Protocol (matches viewer_zmq.cpp):
- Endpoint: tcp://127.0.0.1:5557 (ROUTER on C++ side, use REQ/DEALER here).
- Messages: JSON string as single frame. Responses:
    - For most commands, a single JSON reply frame.
    - For export_frame, a multipart reply: frame 1 JSON with ok=true,width,height,format,bytes_per_pixel; frame 2 raw bytes.

Available helpers:
    ViewerClient(endpoint="tcp://127.0.0.1:5557", socket_type="REQ")
        .ping()
        .get_status()
        .console_execute(line)
        .set_suspended(value: bool)
        .export_frame(timeout=5.0) -> (meta: dict, data: bytes)

Usage example:
    from pymi.zmq_client import ViewerClient
    c = ViewerClient()
    print(c.ping())
    c.console_execute("c dir 0 0 -1")
    c.set_suspended(True)
    meta, data = c.export_frame()
    print(meta, len(data))
"""
import json
import time
from typing import Tuple, Dict, Optional
import zmq


class ViewerClient:
    def __init__(self, endpoint: str = "tcp://127.0.0.1:25957", socket_type: str = "REQ", context: Optional[zmq.Context] = None):
        self.endpoint = endpoint
        self.socket_type_str = socket_type
        self.ctx = context or zmq.Context.instance()
        self.socket = None
        self._create_socket()

    def _create_socket(self):
        if self.socket:
            try:
                self.socket.close(linger=0)
            except Exception:
                pass

        if self.socket_type_str.upper() == "REQ":
            self.socket = self.ctx.socket(zmq.REQ)
        elif self.socket_type_str.upper() == "DEALER":
            self.socket = self.ctx.socket(zmq.DEALER)
        else:
            raise ValueError("socket_type must be REQ or DEALER")

        self.socket.connect(self.endpoint)
        # Avoid hangs on close
        self.socket.setsockopt(zmq.LINGER, 0)

    def close(self):
        try:
            if self.socket:
                self.socket.close(linger=0)
        except Exception:
            pass

    def _send_recv(self, payload: dict, expect_multipart: bool = False, timeout: float = 5.0):
        try:
            self.socket.send_string(json.dumps(payload))
            poller = zmq.Poller()
            poller.register(self.socket, zmq.POLLIN)
            socks = dict(poller.poll(int(timeout * 1000)))

            if socks.get(self.socket) != zmq.POLLIN:
                # Timed out. The REQ socket is now in a state expecting a reply.
                # Since we can't cancel the receive, we must close and recreate the socket.
                self._create_socket()
                raise TimeoutError("No response from viewer")

            if expect_multipart:
                parts = self.socket.recv_multipart()
                if not parts:
                    raise RuntimeError("Empty reply")
                meta = json.loads(parts[0].decode("utf-8"))
                if not meta.get("ok", False):
                    raise RuntimeError(f"export_frame failed: {meta}")
                data = parts[1] if len(parts) > 1 else b""
                return meta, data
            else:
                reply = json.loads(self.socket.recv().decode("utf-8"))
                return reply

        except (zmq.ZMQError, Exception) as e:
            # Check if it is a state error or just timeout logic above
            # If we already reset the socket (TimeoutError case), we don't need to do it again unless this catch block is reached otherwise.
            # But aggressive resetting on any error is safer for REQ sockets.
            if not isinstance(e, TimeoutError):
                 self._create_socket()
            raise e

    def ping(self):
        return self._send_recv({"cmd": "ping"})

    def get_status(self):
        return self._send_recv({"cmd": "get_status"})

    def console_execute(self, line: str):
        return self._send_recv({"cmd": "console_execute", "args": {"line": line}})

    def set_suspended(self, value: bool):
        return self._send_recv({"cmd": "set_suspended", "args": {"value": bool(value)}})

    def next_frame(self):
        return self._send_recv({"cmd": "next_frame"})

    """
    Export a frame with metadata.
    """
    def export_frame(self, timeout: float = 5.0, **kwargs) -> Tuple[Dict, bytes]:
        meta, data = self._send_recv({"cmd": "export_frame", "args": kwargs}, expect_multipart=True, timeout=timeout)
        return meta, data

    def get_cvar(self, name: str):
        return self._send_recv({"cmd": "get_cvar", "args": {"name": name}})


__all__ = ["ViewerClient"]

