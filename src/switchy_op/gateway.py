"""Gateway: the robot end of the protocol. It owns safety and does not trust the client."""

import asyncio
import contextlib
import hmac
import time
from collections import OrderedDict
from collections.abc import Callable
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Literal

import yaml
from pydantic import BaseModel, ConfigDict, Field

from switchy_op.backend import ActionFailed, RobotBackend
from switchy_op.protocol import (
    MAX_LINE,
    PROTO,
    ProtocolError,
    StreamPacket,
    decode,
    decode_stream,
    encode,
    hello_mac,
)

_RESPONSE_CACHE = 256
_STATE_PERIOD_S = 0.1
_HANDSHAKE_TIMEOUT_S = 3.0


class _Strict(BaseModel):
    model_config = ConfigDict(extra="forbid")


class DriveSpec(_Strict):
    type: Literal["differential"] = "differential"
    max_linear: float = Field(1.0, gt=0)
    max_angular: float = Field(2.0, gt=0)
    max_accel_linear: float = Field(2.0, gt=0)
    max_accel_angular: float = Field(4.0, gt=0)


class Requires(_Strict):
    estop: bool | None = None
    speed_below: float | None = None


class ActionSpec(_Strict):
    """kind: switch is set on/off, hold is active while a button is held (sent in the stream),
    long runs for a while with progress and can be cancelled."""

    kind: Literal["switch", "hold", "long"]
    label: str
    safety: Literal["normal", "confirm"] = "normal"
    requires: Requires = Field(default_factory=Requires)
    min_interval_ms: int = Field(0, ge=0)
    on_link_loss: Literal["keep", "off", "cancel"] | None = None

    @property
    def link_loss(self) -> str:
        if self.on_link_loss is not None:
            return self.on_link_loss
        return "cancel" if self.kind == "long" else "keep"


class GatewayConfig(_Strict):
    name: str
    drive: DriveSpec = Field(default_factory=DriveSpec)
    hb_ms: int = Field(100, gt=0)
    link_timeout_ms: int = Field(400, gt=0)
    actions: dict[str, ActionSpec] = {}


def load_config(path: str | Path) -> GatewayConfig:
    return GatewayConfig.model_validate(yaml.safe_load(Path(path).read_text(encoding="utf-8")))


@dataclass(eq=False)
class _Session:
    id: str
    writer: asyncio.StreamWriter
    last_stream: float
    last_seq: int = -1
    link_ok: bool = True
    cmd: tuple[float, float, bool, frozenset[str]] = (0.0, 0.0, False, frozenset())
    highest_id: int = -1
    responses: OrderedDict[int, list[dict[str, Any]]] = field(default_factory=OrderedDict)
    longs: dict[int, tuple[str, asyncio.Task[None]]] = field(default_factory=dict)
    last_action_at: dict[str, float] = field(default_factory=dict)


def _approach(current: float, target: float, step: float) -> float:
    if target > current:
        return min(target, current + step)
    return max(target, current - step)


class _StreamProtocol(asyncio.DatagramProtocol):
    def __init__(self, gateway: Gateway) -> None:
        self._gateway = gateway

    def datagram_received(self, data: bytes, addr: Any) -> None:
        self._gateway.handle_datagram(data)


class Gateway:
    """One controlling client at a time. Stopping on lost link is immediate, not ramped."""

    def __init__(
        self,
        config: GatewayConfig,
        backend: RobotBackend,
        key: bytes,
        host: str = "127.0.0.1",
        port: int = 0,
        udp_port: int = 0,
        tick_hz: float = 50.0,
        log: Callable[[str], None] | None = None,
    ) -> None:
        self._config = config
        self._backend = backend
        self._key = key
        self._host, self._port, self._udp_port = host, port, udp_port
        self._tick_hz = tick_hz
        self._log: Callable[[str], None] = log or (lambda text: None)
        self._clock = time.monotonic
        self._session: _Session | None = None
        self._cmd = (0.0, 0.0)
        self._state_v = 0
        self._last_state: dict[str, Any] | None = None
        self._next_state_at = 0.0
        self._server: asyncio.Server | None = None
        self._udp: asyncio.DatagramTransport | None = None
        self._ticker: asyncio.Task[None] | None = None

    @property
    def tcp_port(self) -> int:
        assert self._server is not None
        return int(self._server.sockets[0].getsockname()[1])

    @property
    def udp_port(self) -> int:
        assert self._udp is not None
        return int(self._udp.get_extra_info("sockname")[1])

    async def start(self) -> None:
        loop = asyncio.get_running_loop()
        self._server = await asyncio.start_server(
            self._on_client, self._host, self._port, limit=MAX_LINE
        )
        transport, _ = await loop.create_datagram_endpoint(
            lambda: _StreamProtocol(self), local_addr=(self._host, self._udp_port)
        )
        self._udp = transport
        self._ticker = asyncio.create_task(self._tick_loop())

    async def stop(self) -> None:
        if self._ticker is not None:
            self._ticker.cancel()
            with contextlib.suppress(asyncio.CancelledError):
                await self._ticker
        if self._session is not None:
            self._end_session(self._session, "gateway stopping")
        if self._server is not None:
            self._server.close()
            await self._server.wait_closed()
        if self._udp is not None:
            self._udp.close()
        self._backend.stop()

    def handle_datagram(self, data: bytes) -> None:
        packet = decode_stream(self._key, data)
        if packet is not None:
            self._on_stream(packet)

    def capabilities(self) -> dict[str, Any]:
        drive = self._config.drive
        return {
            "name": self._config.name,
            "drive": {
                "type": drive.type,
                "max_linear": drive.max_linear,
                "max_angular": drive.max_angular,
            },
            "actions": {
                name: {"kind": spec.kind, "label": spec.label, "safety": spec.safety}
                for name, spec in self._config.actions.items()
            },
        }

    async def _on_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        session: _Session | None = None
        try:
            session = await self._handshake(reader, writer)
            if session is None:
                return
            while line := await reader.readline():
                try:
                    self._handle(session, decode(line))
                except ProtocolError as exc:
                    self._send(session, {"t": "error", "reason": str(exc)})
        except ConnectionError, asyncio.IncompleteReadError, ValueError:
            pass
        finally:
            if session is not None:
                self._end_session(session, "connection closed")
            writer.close()
            with contextlib.suppress(OSError):
                await writer.wait_closed()

    async def _handshake(
        self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter
    ) -> _Session | None:
        def refuse(reason: str) -> None:
            self._log(f"refused connection: {reason}")
            writer.write(encode({"t": "reject", "reason": reason}))

        try:
            hello = decode(await asyncio.wait_for(reader.readline(), _HANDSHAKE_TIMEOUT_S))
        except TimeoutError, ProtocolError:
            refuse("bad_hello")
            return None
        session_id, nonce = str(hello.get("session", "")), str(hello.get("nonce", ""))
        if hello["t"] != "hello" or hello.get("proto") != PROTO:
            refuse("unsupported_protocol")
        elif not hmac.compare_digest(
            str(hello.get("mac", "")), hello_mac(self._key, session_id, nonce)
        ):
            refuse("auth_failed")
        elif self._session is not None:
            refuse("busy")
        else:
            session = _Session(session_id, writer, last_stream=self._clock())
            self._session = session
            self._log(f"client connected: {hello.get('client', '?')}")
            self._send(
                session,
                {
                    "t": "welcome",
                    "session": session_id,
                    "hb_ms": self._config.hb_ms,
                    "link_timeout_ms": self._config.link_timeout_ms,
                    "udp_port": self.udp_port,
                    "capabilities": self.capabilities(),
                    "v": self._state_v,
                    "s": self._backend.state(),
                },
            )
            return session
        return None

    def _handle(self, session: _Session, message: dict[str, Any]) -> None:
        kind = message["t"]
        if kind == "action":
            self._action(session, message)
        elif kind == "cancel":
            self._cancel(session, message)
        elif kind == "ping":
            self._send(session, {"t": "pong", "n": message.get("n")})
        else:
            raise ProtocolError(f"unknown message type: {kind}")

    def _action(self, session: _Session, message: dict[str, Any]) -> None:
        action_id, name = message.get("id"), message.get("name")
        if not isinstance(action_id, int) or isinstance(action_id, bool):
            raise ProtocolError("action needs an integer 'id'")
        if not isinstance(name, str):
            raise ProtocolError("action needs a string 'name'")

        if action_id in session.responses:
            self._log(f"duplicate of action {action_id} ({name}): replaying answer")
            for reply in session.responses[action_id]:
                self._send(session, reply)
            return
        if action_id <= session.highest_id:
            self._reply(session, action_id, _ack(action_id, "rejected", "stale_id"))
            return
        session.highest_id = action_id

        reason = self._check(session, name, message)
        if reason is not None:
            status = "expired" if reason == "expired" else "rejected"
            self._log(f"{status} {name}: {reason}")
            self._reply(session, action_id, _ack(action_id, status, reason))
            return

        spec = self._config.actions[name]
        session.last_action_at[name] = self._clock()
        self._reply(session, action_id, _ack(action_id, "accepted"))
        if spec.kind == "switch":
            value = bool(message["value"])
            self._log(f"execute {name}={value}")
            self._backend.set_switch(name, value)
        else:
            params = dict(message.get("params") or {})
            self._log(f"start {name}")
            task = asyncio.create_task(self._run_long(session, action_id, name, params))
            session.longs[action_id] = (name, task)

    def _check(self, session: _Session, name: str, message: dict[str, Any]) -> str | None:
        spec = self._config.actions.get(name)
        if spec is None:
            return "unknown_action"
        try:
            if float(message.get("age_ms", 0)) > float(message.get("ttl_ms", 1000)):
                return "expired"
        except TypeError, ValueError:
            return "bad_request"
        params = message.get("params")
        if params is not None and not isinstance(params, dict):
            return "bad_request"
        if spec.kind == "hold":
            return "use_stream"
        op = message.get("op")
        if spec.kind == "switch" and (op != "set" or not isinstance(message.get("value"), bool)):
            return "bad_request"
        if spec.kind == "long" and op != "start":
            return "bad_request"
        if (blocked := self._blocked(spec)) is not None:
            return blocked
        if spec.safety == "confirm" and (params or {}).get("confirmed") is not True:
            return "confirmation_required"
        last = session.last_action_at.get(name)
        if last is not None and (self._clock() - last) * 1000 < spec.min_interval_ms:
            return "too_soon"
        if spec.kind == "long" and any(n == name for n, _ in session.longs.values()):
            return "already_running"
        return None

    def _blocked(self, spec: ActionSpec) -> str | None:
        state = self._backend.state()
        if spec.requires.estop is not None and bool(state.get("estop")) != spec.requires.estop:
            return "estop_active" if state.get("estop") else "estop_required"
        limit = spec.requires.speed_below
        if limit is not None and abs(float(state.get("speed", 0.0))) >= limit:
            return "moving"
        return None

    def _cancel(self, session: _Session, message: dict[str, Any]) -> None:
        action_id = message.get("id")
        entry = session.longs.get(action_id) if isinstance(action_id, int) else None
        if entry is None:
            self._send(
                session,
                {"t": "result", "id": action_id, "status": "failed", "reason": "not_running"},
            )
        else:
            self._log(f"cancel {entry[0]}")
            entry[1].cancel()

    async def _run_long(
        self, session: _Session, action_id: int, name: str, params: dict[str, Any]
    ) -> None:
        def progress(fraction: float) -> None:
            self._send(session, {"t": "progress", "id": action_id, "fraction": round(fraction, 3)})

        try:
            await self._backend.run_long(name, params, progress)
        except asyncio.CancelledError:
            self._reply(session, action_id, _result(action_id, "canceled"))
            raise
        except ActionFailed as exc:
            self._reply(session, action_id, _result(action_id, "failed", exc.reason))
        else:
            self._reply(session, action_id, _result(action_id, "succeeded"))
        finally:
            session.longs.pop(action_id, None)

    def _on_stream(self, packet: StreamPacket) -> None:
        session = self._session
        if session is None or packet.session != session.id or packet.seq <= session.last_seq:
            return
        session.last_seq = packet.seq
        session.last_stream = self._clock()
        held = frozenset(
            name
            for name in packet.held
            if (spec := self._config.actions.get(name)) is not None and spec.kind == "hold"
        )
        vx, wz = (max(-1.0, min(1.0, value)) for value in (packet.vx, packet.wz))
        session.cmd = (vx, wz, packet.deadman, held)
        if not session.link_ok:
            session.link_ok = True
            self._log("link recovered")

    async def _tick_loop(self) -> None:
        period = 1.0 / self._tick_hz
        last = next_time = self._clock()
        while True:
            next_time += period
            await asyncio.sleep(max(0.0, next_time - self._clock()))
            now = self._clock()
            self._tick(min(now - last, 0.2), now)
            last = now

    def _tick(self, dt: float, now: float) -> None:
        session = self._session
        timeout = self._config.link_timeout_ms / 1000
        if session is not None and session.link_ok and now - session.last_stream > timeout:
            self._link_lost(session, "no stream")

        live = session is not None and session.link_ok
        vx, wz, deadman, held = session.cmd if live and session else (0.0, 0.0, False, frozenset())
        drive = self._config.drive
        if not live or not deadman or self._backend.state().get("estop"):
            self._cmd = (0.0, 0.0)
        else:
            self._cmd = (
                _approach(self._cmd[0], vx * drive.max_linear, drive.max_accel_linear * dt),
                _approach(self._cmd[1], wz * drive.max_angular, drive.max_accel_angular * dt),
            )
        self._backend.drive(*self._cmd)
        self._backend.set_held(held)
        self._backend.step(dt)

        if now >= self._next_state_at:
            self._next_state_at = now + _STATE_PERIOD_S
            self._publish_state(session)

    def _publish_state(self, session: _Session | None) -> None:
        snapshot = self._backend.state()
        if snapshot == self._last_state:
            return
        self._last_state = snapshot
        self._state_v += 1
        if session is not None:
            self._send(session, {"t": "state", "v": self._state_v, "s": snapshot})

    def _link_lost(self, session: _Session, reason: str) -> None:
        if not session.link_ok:
            return
        session.link_ok = False
        self._log(f"link lost ({reason}): stopping")
        for name, task in list(session.longs.values()):
            if self._config.actions[name].link_loss == "cancel":
                task.cancel()
        for name, spec in self._config.actions.items():
            if spec.kind == "switch" and spec.link_loss == "off":
                self._backend.set_switch(name, False)

    def _end_session(self, session: _Session, reason: str) -> None:
        self._link_lost(session, reason)
        if self._session is session:
            self._session = None
            self._log(f"client disconnected ({reason})")

    def _reply(self, session: _Session, action_id: int, message: dict[str, Any]) -> None:
        """Send and remember, so a repeated request gets the same answer without re-running."""
        session.responses.setdefault(action_id, []).append(message)
        while len(session.responses) > _RESPONSE_CACHE:
            session.responses.popitem(last=False)
        self._send(session, message)

    @staticmethod
    def _send(session: _Session, message: dict[str, Any]) -> None:
        if not session.writer.is_closing():
            session.writer.write(encode(message))


def _ack(action_id: int, status: str, reason: str | None = None) -> dict[str, Any]:
    message: dict[str, Any] = {"t": "ack", "id": action_id, "status": status}
    if reason is not None:
        message["reason"] = reason
    return message


def _result(action_id: int, status: str, reason: str | None = None) -> dict[str, Any]:
    message: dict[str, Any] = {"t": "result", "id": action_id, "status": status}
    if reason is not None:
        message["reason"] = reason
    return message
