"""A simulated differential-drive robot for developing the operator side without hardware."""

import asyncio
import math
from collections.abc import Callable
from typing import Any

from switchy_op.backend import ActionFailed


class SimRobot:
    def __init__(self) -> None:
        self.x = 0.0
        self.y = 0.0
        self.theta = 0.0
        self.vx = 0.0
        self.wz = 0.0
        self.light = False
        self.horn = False
        self.estop = False
        self.battery = 1.0
        self.arm = "stowed"
        self.executed: list[tuple[str, bool]] = []

    def state(self) -> dict[str, Any]:
        return {
            "x": round(self.x, 2),
            "y": round(self.y, 2),
            "heading": round(math.degrees(self.theta) % 360, 1),
            "speed": round(abs(self.vx), 3),
            "battery": round(self.battery, 3),
            "light": self.light,
            "horn": self.horn,
            "estop": self.estop,
            "arm": self.arm,
        }

    def step(self, dt: float) -> None:
        self.theta += self.wz * dt
        self.x += self.vx * math.cos(self.theta) * dt
        self.y += self.vx * math.sin(self.theta) * dt
        self.battery = max(0.0, self.battery - dt * (0.0005 + 0.0005 * abs(self.vx)))

    def drive(self, vx: float, wz: float) -> None:
        self.vx, self.wz = (0.0, 0.0) if self.estop else (vx, wz)

    def set_switch(self, name: str, value: bool) -> None:
        self.executed.append((name, value))
        if name == "light":
            self.light = value
        elif name == "estop":
            self.estop = value
            if value:
                self.vx = self.wz = 0.0
        else:
            raise ValueError(f"unknown switch: {name}")

    def set_held(self, names: frozenset[str]) -> None:
        self.horn = "horn" in names

    async def run_long(
        self, name: str, params: dict[str, Any], progress: Callable[[float], None]
    ) -> None:
        if name != "arm_home":
            raise ActionFailed("unsupported")
        self.arm = "moving"
        try:
            for step in range(1, 11):
                await asyncio.sleep(0.2)
                progress(step / 10)
        except asyncio.CancelledError:
            self.arm = "halted"
            raise
        self.arm = "home"

    def stop(self) -> None:
        self.vx = self.wz = 0.0
