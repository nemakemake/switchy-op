"""Interactive operator: a window with a HUD, a gamepad or the keyboard as input."""

import asyncio
import time
from pathlib import Path

from switchy_op.bindings import Bindings, Request, load_profile
from switchy_op.client import RobotClient
from switchy_op.inputs import PygameInput
from switchy_op.session import Operator

_RATE_HZ = 30.0


async def run_interactive(
    profile_path: Path, host: str | None, port: int | None, key: str | None
) -> None:
    try:
        import pygame  # noqa: F401
    except ImportError:
        raise ImportError("pygame is missing: run `uv sync --extra ui`") from None

    profile = load_profile(profile_path)
    target = profile.robot
    client = RobotClient(
        host or target.host, port or target.port, (key or target.key).encode(), name="operator"
    )
    welcome = await client.connect()
    robot_name = welcome["capabilities"]["name"]

    notices: list[str] = []

    def on_result(request: Request, text: str) -> None:
        notices.append(f"{request.name}: {text}")
        del notices[:-4]

    operator = Operator(client, Bindings(profile), on_result)
    pad = PygameInput()
    started = time.monotonic()
    try:
        while client.connected:
            inp = pad.poll()
            if inp is None:
                break
            operator.tick(inp, time.monotonic() - started)
            state = client.state
            pad.draw(
                [
                    f"robot: {robot_name}   input: {pad.source}",
                    f"speed {state.get('speed', 0):.2f} m/s   heading {state.get('heading', 0)}   "
                    f"battery {state.get('battery', 0) * 100:.0f}%",
                    f"light {'on' if state.get('light') else 'off'}   "
                    f"horn {'on' if state.get('horn') else 'off'}   arm {state.get('arm', '?')}   "
                    f"e-stop {'ACTIVE' if state.get('estop') else 'off'}",
                    "",
                    *notices,
                    "",
                    "hold Space/ZR to drive, W/S forward, A/D turn, L light, H horn,",
                    "X (hold) arm, M e-stop, Esc quit",
                ]
            )
            await asyncio.sleep(1 / _RATE_HZ)
    finally:
        pad.close()
        await client.close()
