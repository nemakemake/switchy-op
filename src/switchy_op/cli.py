"""Command line: switchy-op gateway | demo | client."""

import argparse
import asyncio
import contextlib
import json
import sys
from collections.abc import Sequence
from pathlib import Path

from switchy_op.backend import RobotBackend
from switchy_op.bindings import load_profile
from switchy_op.demo import ROOT, run_demo
from switchy_op.gateway import Gateway, load_config
from switchy_op.sim import SimRobot

DEFAULT_ROBOT = ROOT / "robots" / "sim.yaml"
DEFAULT_PROFILE = ROOT / "profiles" / "gamepad.yaml"


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="switchy-op", description="Remote robot operator")
    commands = parser.add_subparsers(dest="command", required=True)

    gateway = commands.add_parser("gateway", help="run a gateway with a simulated robot")
    gateway.add_argument("--robot", type=Path, default=DEFAULT_ROBOT, help="gateway config")
    gateway.add_argument("--host", default="127.0.0.1")
    gateway.add_argument("--port", type=int, default=7000)
    gateway.add_argument("--udp-port", type=int, default=7001)
    gateway.add_argument("--key", default="dev-key")
    gateway.add_argument("--backend", choices=("sim", "gazebo"), default="sim")
    gateway.add_argument("--bridge-port", type=int, default=7447)
    gateway.add_argument(
        "--run-for", type=float, default=None, metavar="SECONDS", help="stop by itself after this"
    )

    export = commands.add_parser(
        "export-profile", help="convert a YAML profile to the JSON the native client reads"
    )
    export.add_argument("profile", type=Path)
    export.add_argument("-o", "--output", type=Path, default=None, help="default: next to input")

    profiles = commands.add_parser(
        "serve-profiles", help="share control profiles with a Switch over Wi-Fi"
    )
    profiles.add_argument("--directory", type=Path, default=ROOT / "profiles")
    profiles.add_argument("--host", default="0.0.0.0")
    profiles.add_argument("--port", type=int, default=7080)
    profiles.add_argument("--key", required=True, help="shared download key entered on Switch")

    demo = commands.add_parser("demo", help="scripted end-to-end demonstration with self-checks")
    demo.add_argument("--robot", type=Path, default=None)
    demo.add_argument("--profile", type=Path, default=None)

    client = commands.add_parser("client", help="interactive operator (needs the 'ui' extra)")
    client.add_argument("--profile", type=Path, default=DEFAULT_PROFILE)
    client.add_argument("--host", default=None)
    client.add_argument("--port", type=int, default=None)
    client.add_argument("--key", default=None)
    return parser


async def _serve(args: argparse.Namespace) -> None:
    backend: RobotBackend

    def close() -> None:
        return None

    if args.backend == "gazebo":
        from switchy_op.gazebo import GazeboRobot

        backend = robot = GazeboRobot(args.bridge_port)
        close = robot.close
        if args.robot == DEFAULT_ROBOT:
            args.robot = ROOT / "robots" / "gazebo.yaml"
        config = load_config(args.robot)
        if any(name != "estop" or spec.kind != "switch" for name, spec in config.actions.items()):
            close()
            raise ValueError("Gazebo backend supports only the estop switch")
    else:
        backend = SimRobot()
        config = load_config(args.robot)
    gateway = Gateway(
        config,
        backend,
        args.key.encode(),
        host=args.host,
        port=args.port,
        udp_port=args.udp_port,
        log=print,
    )
    try:
        await gateway.start()
        print(f"gateway listening on {args.host}: tcp {gateway.tcp_port}, udp {gateway.udp_port}")
        if args.run_for is None:
            await asyncio.Event().wait()
        else:
            await asyncio.sleep(args.run_for)
    finally:
        await gateway.stop()
        close()


def _export_profile(args: argparse.Namespace) -> None:
    profile = load_profile(args.profile)
    output = args.output or args.profile.with_suffix(".json")
    text = json.dumps(profile.model_dump(mode="json", exclude_none=True), indent=2)
    output.write_text(text + "\n", encoding="utf-8", newline="\n")
    print(f"wrote {output}")


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "gateway":
            with contextlib.suppress(KeyboardInterrupt):
                asyncio.run(_serve(args))
            return 0
        if args.command == "export-profile":
            _export_profile(args)
            return 0
        if args.command == "serve-profiles":
            from switchy_op.profile_server import serve_profiles

            with contextlib.suppress(KeyboardInterrupt):
                asyncio.run(serve_profiles(args.directory, args.host, args.port, args.key))
            return 0
        if args.command == "demo":
            return 0 if asyncio.run(run_demo(args.robot, args.profile)) else 1
        from switchy_op.interactive import run_interactive

        with contextlib.suppress(KeyboardInterrupt):
            asyncio.run(run_interactive(args.profile, args.host, args.port, args.key))
        return 0
    except (ValueError, OSError, ImportError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
