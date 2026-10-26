"""Command line: switchy-op gateway | demo | client."""

import argparse
import asyncio
import contextlib
import sys
from collections.abc import Sequence
from pathlib import Path

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
    gateway = Gateway(
        load_config(args.robot),
        SimRobot(),
        args.key.encode(),
        host=args.host,
        port=args.port,
        udp_port=args.udp_port,
        log=print,
    )
    await gateway.start()
    print(f"gateway listening on {args.host}: tcp {gateway.tcp_port}, udp {gateway.udp_port}")
    try:
        await asyncio.Event().wait()
    finally:
        await gateway.stop()


def main(argv: Sequence[str] | None = None) -> int:
    args = _parser().parse_args(argv)
    try:
        if args.command == "gateway":
            with contextlib.suppress(KeyboardInterrupt):
                asyncio.run(_serve(args))
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
