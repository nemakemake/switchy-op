"""Scripted end-to-end demonstration: operator, flaky link, safety interlocks and link loss.

Runs a gateway with a simulated robot and a client on this machine, then checks what happened.
"""

import asyncio
import time
from collections.abc import Callable
from pathlib import Path

from switchy_op.bindings import Bindings, InputState, Request, load_profile
from switchy_op.client import Chaos, RobotClient
from switchy_op.gateway import Gateway, load_config
from switchy_op.inputs import ScriptedInput
from switchy_op.session import Operator, sleep_ticks
from switchy_op.sim import SimRobot

ROOT = Path(__file__).resolve().parents[2]
KEY = b"dev-key"
RATE_HZ = 30.0


def _hold(*buttons: str, **axes: float) -> InputState:
    return InputState(axes, frozenset(buttons))


# Operator script: drive forward, switch the light on, honk, switch it off, then hold X to
# start the arm. Times are seconds from the start of the run.
TIMELINE = [
    (0.3, _hold("zr", left_y=-1.0)),
    (1.3, _hold()),
    (1.5, _hold("y")),
    (1.6, _hold()),
    (1.8, _hold("b")),
    (2.3, _hold()),
    (2.5, _hold("y")),
    (2.6, _hold()),
    (3.0, _hold("x")),
    (4.2, _hold()),
]
SCRIPT_END = 7.0


async def run_demo(
    robot_config: Path | None = None,
    profile_path: Path | None = None,
    out: Callable[[str], None] = print,
) -> bool:
    started = time.monotonic()

    def say(who: str, text: str) -> None:
        out(f"[{time.monotonic() - started:5.2f}s] {who:<8} {text}")

    robot = SimRobot()
    gateway = Gateway(
        load_config(robot_config or ROOT / "robots" / "sim.yaml"),
        robot,
        KEY,
        log=lambda text: say("gateway", text),
    )
    await gateway.start()
    chaos = Chaos(drop=0.25, duplicate=0.25, seed=7)
    results: dict[str, str] = {}

    def on_result(request: Request, text: str) -> None:
        results[request.name] = text
        say("operator", f"{request.name} -> {text}")

    client = RobotClient(
        "127.0.0.1",
        gateway.tcp_port,
        KEY,
        chaos=chaos,
        rate_hz=RATE_HZ,
        log=lambda text: say("link", text),
    )
    checks: list[tuple[str, bool]] = []

    def check(name: str, ok: bool) -> None:
        checks.append((name, ok))
        say("check", f"{'PASS' if ok else 'FAIL'}  {name}")

    try:
        welcome = await client.connect()
        say("operator", f"connected to {welcome['capabilities']['name']}")
        operator = Operator(
            client,
            Bindings(load_profile(profile_path or ROOT / "profiles" / "gamepad.yaml")),
            on_result,
        )

        say("operator", "--- phase 1: driving and buttons over a lossy link ---")
        script = ScriptedInput(TIMELINE)
        horn_seen = False

        def step(t: float) -> None:
            nonlocal horn_seen
            operator.tick(script.at(t), t)
            horn_seen = horn_seen or robot.horn

        await sleep_ticks(1 / RATE_HZ, int(SCRIPT_END * RATE_HZ), step)
        await operator.settle()
        light_log = [entry for entry in robot.executed if entry[0] == "light"]
        check("robot drove forward", robot.x > 0.3)
        check(
            "light switched on then off exactly once each (no duplicates)",
            light_log == [("light", True), ("light", False)],
        )
        check("horn sounded only while the button was held", horn_seen and not robot.horn)
        check("arm homed after a confirmed one-second hold", robot.arm == "home")

        say("operator", "--- phase 2: safety interlocks ---")
        await client.action("estop", "set", True)
        refused = await client.action("arm_home", "start", params={"confirmed": True})
        check(
            f"arm refused while e-stop is active ({refused.get('reason')})",
            refused.get("reason") == "estop_active",
        )
        await client.action("estop", "set", False)
        unconfirmed = await client.action("arm_home", "start")
        check(
            f"arm refused without confirmation ({unconfirmed.get('reason')})",
            unconfirmed.get("reason") == "confirmation_required",
        )

        say("operator", "--- phase 3: link loss while moving and while the arm works ---")
        await asyncio.sleep(1.1)
        long_action = await client.start_long("arm_home", confirmed=True)
        client.drive(1.0, 0.0, True)
        await asyncio.sleep(0.5)
        say("operator", f"driving at {robot.state()['speed']} m/s, now cutting the link")
        client.stream_paused = True
        await asyncio.sleep(0.9)
        check("robot stopped on its own", robot.state()["speed"] == 0.0)
        outcome = await asyncio.wait_for(long_action.result, 2.0)
        check(
            f"running arm action was cancelled ({outcome['status']})",
            outcome["status"] == "canceled",
        )
        client.stream_paused = False
        await asyncio.sleep(0.5)
        check("driving resumes after the link returns", robot.state()["speed"] > 0.0)
        client.drive(0.0, 0.0, False)

        say("operator", "--- phase 4: reconnect ---")
        light_before = robot.light
        await client.close()
        await asyncio.sleep(0.3)
        again = RobotClient("127.0.0.1", gateway.tcp_port, KEY, log=lambda text: say("link", text))
        await again.connect()
        check("new session sees the robot's real state", again.state["light"] == light_before)
        await again.close()
    finally:
        await client.close()
        await gateway.stop()

    failed = [name for name, ok in checks if not ok]
    say("demo", f"{len(checks) - len(failed)} of {len(checks)} checks passed")
    return not failed
