"""Local UDP transport to the ROS-system-Python sidecar (no ROS dependency here)."""

import contextlib
import json
import math
import socket
import time
from collections.abc import Callable
from typing import Any

from switchy_op.backend import ActionFailed


class GazeboRobot:
    def __init__(self, port: int = 7447) -> None:
        self._socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._socket.connect(("127.0.0.1", port))
        self._socket.setblocking(False)
        self._last_state = 0.0
        self._telemetry: dict[str, Any] = {}
        self._estop = False

    def state(self) -> dict[str, Any]:
        ready = time.monotonic() - self._last_state < 0.5
        return {
            **self._telemetry,
            "speed": self._telemetry.get("speed", 0.0),
            "estop": self._estop or not ready,
            "bridge_ready": ready,
        }

    def step(self, dt: float) -> None:
        for _ in range(100):
            try:
                data = self._socket.recv(8192)
            except BlockingIOError, ConnectionRefusedError, ConnectionResetError:
                break
            try:
                state = json.loads(data)
                if not isinstance(state, dict) or state.get("ready") is not True:
                    continue
                if not all(
                    isinstance(state.get(k), (int, float)) and math.isfinite(state[k])
                    for k in ("x", "y", "heading", "speed")
                ):
                    continue
                self._telemetry = {k: state[k] for k in ("x", "y", "heading", "speed")}
                self._last_state = time.monotonic()
            except ValueError, TypeError:
                continue

    def drive(self, vx: float, wz: float) -> None:
        if self.state()["estop"]:
            vx = wz = 0.0
        with contextlib.suppress(ConnectionRefusedError, ConnectionResetError):
            self._socket.send(json.dumps({"vx": vx, "wz": wz}).encode())

    def set_switch(self, name: str, value: bool) -> None:
        if name != "estop":
            raise ValueError(f"unsupported Gazebo switch: {name}")
        self._estop = value
        if value:
            self.stop()

    def set_held(self, names: frozenset[str]) -> None:
        pass

    async def run_long(
        self, name: str, params: dict[str, Any], progress: Callable[[float], None]
    ) -> None:
        raise ActionFailed("unsupported")

    def stop(self) -> None:
        self.drive(0.0, 0.0)

    def close(self) -> None:
        self.stop()
        self._socket.close()
