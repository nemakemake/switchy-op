"""What the gateway needs from a robot. A ROS 2 backend would implement the same interface."""

from collections.abc import Callable
from typing import Any, Protocol


class ActionFailed(Exception):
    """A long action ended without succeeding."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


class RobotBackend(Protocol):
    def state(self) -> dict[str, Any]:
        """Telemetry as plain JSON-compatible values. Must include `estop` and `speed`."""
        ...

    def step(self, dt: float) -> None:
        """Advance internal simulation; real robots do nothing here."""
        ...

    def drive(self, vx: float, wz: float) -> None:
        """Set already limited velocities in m/s and rad/s."""
        ...

    def set_switch(self, name: str, value: bool) -> None: ...

    def set_held(self, names: frozenset[str]) -> None:
        """Hold-type actions that are currently active; everything else must be off."""
        ...

    async def run_long(
        self, name: str, params: dict[str, Any], progress: Callable[[float], None]
    ) -> None:
        """Run a long action. Must stop cleanly when cancelled; raise ActionFailed on failure."""
        ...

    def stop(self) -> None: ...
