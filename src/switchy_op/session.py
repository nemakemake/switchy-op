"""Runs bindings against a client: the loop shared by the demo and the interactive operator."""

import asyncio
from collections.abc import Callable
from typing import Any

from switchy_op.bindings import Bindings, InputState, Request
from switchy_op.client import ActionTimeout, RobotClient


class Operator:
    """Feeds input through the bindings every tick, drives the client and fires action requests."""

    def __init__(
        self,
        client: RobotClient,
        bindings: Bindings,
        on_result: Callable[[Request, str], None] | None = None,
    ) -> None:
        self._client = client
        self._bindings = bindings
        self._on_result = on_result or (lambda request, text: None)
        self._jobs: set[asyncio.Task[None]] = set()

    def tick(self, inp: InputState, now: float) -> None:
        out = self._bindings.update(inp, now, self._client.state)
        self._client.drive(out.vx, out.wz, out.deadman, out.held)
        for request in out.requests:
            job = asyncio.create_task(self._fire(request))
            self._jobs.add(job)
            job.add_done_callback(self._jobs.discard)

    async def settle(self) -> None:
        """Wait for requests that are still in flight."""
        if self._jobs:
            await asyncio.gather(*self._jobs, return_exceptions=True)

    async def _fire(self, request: Request) -> None:
        try:
            if request.op == "start":
                confirmed = bool((request.params or {}).get("confirmed"))
                handle = await self._client.start_long(request.name, confirmed=confirmed)
                if handle.ack["status"] != "accepted":
                    self._on_result(request, _describe(handle.ack))
                    return
                self._on_result(request, "started")
                result = await handle.result
                self._on_result(request, _describe(result))
            else:
                ack = await self._client.action(request.name, "set", request.value)
                self._on_result(request, _describe(ack))
        except (ActionTimeout, ConnectionError) as exc:
            self._on_result(request, f"failed: {exc}")


def _describe(message: dict[str, Any]) -> str:
    reason = f" ({message['reason']})" if "reason" in message else ""
    return f"{message['status']}{reason}"


async def sleep_ticks(period: float, ticks: int, step: Callable[[float], None]) -> None:
    """Call step(elapsed_seconds) at a steady rate; a small helper for scripted runs."""
    loop = asyncio.get_running_loop()
    start = loop.time()
    for index in range(ticks):
        step(loop.time() - start)
        await asyncio.sleep(max(0.0, start + (index + 1) * period - loop.time()))
