"""Client library: connects to a gateway, streams drive commands and sends reliable actions."""

import asyncio
import contextlib
import random
import secrets
import time
from collections.abc import Callable, Iterable
from dataclasses import dataclass, field
from typing import Any

from switchy_op.protocol import (
    MAX_LINE,
    PROTO,
    StreamPacket,
    decode,
    encode,
    encode_stream,
    hello_mac,
)


class ConnectionRefused(ConnectionError):
    """The gateway refused the handshake; the reason is the message."""


class ActionTimeout(TimeoutError):
    """No answer to an action before its time to live ran out."""


@dataclass(frozen=True, slots=True)
class Chaos:
    """Makes the event channel unreliable on purpose, to exercise retries and deduplication."""

    drop: float = 0.0
    duplicate: float = 0.0
    seed: int = 1


@dataclass(eq=False)
class LongAction:
    id: int
    ack: dict[str, Any]
    result: asyncio.Future[dict[str, Any]]
    progress: list[float] = field(default_factory=list)


class RobotClient:
    def __init__(
        self,
        host: str,
        port: int,
        key: bytes,
        name: str = "switchy-op",
        rate_hz: float = 30.0,
        retry_s: float = 0.25,
        chaos: Chaos | None = None,
        log: Callable[[str], None] | None = None,
        on_progress: Callable[[int, float], None] | None = None,
    ) -> None:
        self._host, self._port, self._key, self._name = host, port, key, name
        self._period = 1.0 / rate_hz
        self._retry_s = retry_s
        self._chaos = chaos
        self._rng = random.Random(chaos.seed if chaos else 0)
        self._log: Callable[[str], None] = log or (lambda text: None)
        self._on_progress = on_progress
        self._reader: asyncio.StreamReader | None = None
        self._writer: asyncio.StreamWriter | None = None
        self._udp: asyncio.DatagramTransport | None = None
        self._tasks: list[asyncio.Task[None]] = []
        self._pending: dict[int, asyncio.Future[dict[str, Any]]] = {}
        self._longs: dict[int, LongAction] = {}
        self._next_id = 1
        self._seq = 0
        self._desired: tuple[float, float, bool, tuple[str, ...]] = (0.0, 0.0, False, ())
        self.session = ""
        self.capabilities: dict[str, Any] = {}
        self.state: dict[str, Any] = {}
        self.state_version = -1
        self.connected = False
        self.stream_paused = False

    async def connect(self) -> dict[str, Any]:
        self._reader, self._writer = await asyncio.open_connection(
            self._host, self._port, limit=MAX_LINE
        )
        self.session, nonce = secrets.token_hex(8), secrets.token_hex(8)
        self._writer.write(
            encode(
                {
                    "t": "hello",
                    "proto": PROTO,
                    "client": self._name,
                    "session": self.session,
                    "nonce": nonce,
                    "mac": hello_mac(self._key, self.session, nonce),
                }
            )
        )
        answer = decode(await asyncio.wait_for(self._reader.readline(), 5.0))
        if answer["t"] != "welcome":
            self._writer.close()
            raise ConnectionRefused(str(answer.get("reason", answer["t"])))
        self.capabilities = answer["capabilities"]
        self.state, self.state_version = answer["s"], answer["v"]
        loop = asyncio.get_running_loop()
        transport, _ = await loop.create_datagram_endpoint(
            asyncio.DatagramProtocol, remote_addr=(self._host, answer["udp_port"])
        )
        self._udp = transport
        self.connected = True
        self._tasks = [
            asyncio.create_task(self._read_loop()),
            asyncio.create_task(self._stream_loop()),
        ]
        return answer

    async def close(self) -> None:
        self.connected = False
        for task in self._tasks:
            task.cancel()
        for task in self._tasks:
            with contextlib.suppress(asyncio.CancelledError):
                await task
        self._tasks = []
        if self._udp is not None:
            self._udp.close()
        if self._writer is not None:
            self._writer.close()
            with contextlib.suppress(OSError):
                await self._writer.wait_closed()

    def drive(self, vx: float, wz: float, deadman: bool, held: Iterable[str] = ()) -> None:
        """Set what the stream sends from now on. vx and wz are -1..1."""
        self._desired = (vx, wz, deadman, tuple(held))

    async def action(
        self,
        name: str,
        op: str = "set",
        value: bool | None = None,
        params: dict[str, Any] | None = None,
        ttl_ms: int = 1500,
    ) -> dict[str, Any]:
        """Send an action, repeating until acknowledged. Returns the ack message."""
        action_id = self._take_id()
        return await self._request(action_id, name, op, value, params, ttl_ms)

    async def start_long(
        self,
        name: str,
        params: dict[str, Any] | None = None,
        confirmed: bool = False,
        ttl_ms: int = 1500,
    ) -> LongAction:
        action_id = self._take_id()
        result: asyncio.Future[dict[str, Any]] = asyncio.get_running_loop().create_future()
        handle = LongAction(action_id, {}, result)
        self._longs[action_id] = handle
        merged = {**(params or {}), "confirmed": confirmed}
        handle.ack = await self._request(action_id, name, "start", None, merged, ttl_ms)
        if handle.ack["status"] != "accepted":
            self._longs.pop(action_id, None)
            result.set_result({"t": "result", "id": action_id, "status": "not_started"})
        return handle

    async def cancel(self, action_id: int) -> None:
        self._send_event({"t": "cancel", "id": action_id})

    def _take_id(self) -> int:
        action_id, self._next_id = self._next_id, self._next_id + 1
        return action_id

    async def _request(
        self,
        action_id: int,
        name: str,
        op: str,
        value: bool | None,
        params: dict[str, Any] | None,
        ttl_ms: int,
    ) -> dict[str, Any]:
        loop = asyncio.get_running_loop()
        answer: asyncio.Future[dict[str, Any]] = loop.create_future()
        self._pending[action_id] = answer
        created = time.monotonic()
        try:
            while True:
                age_ms = int((time.monotonic() - created) * 1000)
                if age_ms >= ttl_ms:
                    raise ActionTimeout(f"no answer to {name} within {ttl_ms} ms")
                message: dict[str, Any] = {
                    "t": "action",
                    "id": action_id,
                    "name": name,
                    "op": op,
                    "age_ms": age_ms,
                    "ttl_ms": ttl_ms,
                }
                if value is not None:
                    message["value"] = value
                if params:
                    message["params"] = params
                self._send_event(message)
                with contextlib.suppress(TimeoutError):
                    return await asyncio.wait_for(asyncio.shield(answer), self._retry_s)
                self._log(f"retry {name} (id {action_id})")
        finally:
            self._pending.pop(action_id, None)

    def _send_event(self, message: dict[str, Any]) -> None:
        if self._writer is None or self._writer.is_closing():
            raise ConnectionError("not connected")
        chaos = self._chaos
        if chaos is not None and self._rng.random() < chaos.drop:
            self._log(f"chaos: dropped {message['t']} {message.get('name', '')}")
            return
        data = encode(message)
        self._writer.write(data)
        if chaos is not None and self._rng.random() < chaos.duplicate:
            self._log(f"chaos: duplicated {message['t']} {message.get('name', '')}")
            self._writer.write(data)

    async def _read_loop(self) -> None:
        assert self._reader is not None
        try:
            while line := await self._reader.readline():
                self._dispatch(decode(line))
        except ConnectionError, ValueError:
            pass
        finally:
            self.connected = False
            for future in self._pending.values():
                if not future.done():
                    future.set_exception(ConnectionError("connection lost"))
            for handle in self._longs.values():
                if not handle.result.done():
                    handle.result.set_result({"t": "result", "id": handle.id, "status": "lost"})

    def _dispatch(self, message: dict[str, Any]) -> None:
        kind = message["t"]
        action_id = message.get("id", -1)
        if kind == "state":
            if message["v"] > self.state_version:
                self.state, self.state_version = message["s"], message["v"]
        elif kind == "ack":
            future = self._pending.get(action_id)
            if future is not None and not future.done():
                future.set_result(message)
        elif kind == "progress":
            handle = self._longs.get(action_id)
            if handle is not None:
                handle.progress.append(message["fraction"])
                if self._on_progress is not None:
                    self._on_progress(handle.id, message["fraction"])
        elif kind == "result":
            handle = self._longs.pop(action_id, None)
            if handle is not None and not handle.result.done():
                handle.result.set_result(message)

    async def _stream_loop(self) -> None:
        assert self._udp is not None
        next_time = time.monotonic()
        while True:
            next_time += self._period
            await asyncio.sleep(max(0.0, next_time - time.monotonic()))
            if self.stream_paused:
                continue
            self._seq += 1
            vx, wz, deadman, held = self._desired
            packet = StreamPacket(self.session, self._seq, vx, wz, deadman, held)
            self._udp.sendto(encode_stream(self._key, packet))
