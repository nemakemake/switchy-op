"""Wire format shared by the gateway and the client.

Events travel over TCP as newline-delimited JSON objects, each with a string field "t".
The drive stream is one JSON object per UDP datagram, signed with a shared key.
See docs/protocol.md for the message catalogue.
"""

import hashlib
import hmac
import json
import math
from dataclasses import dataclass
from typing import Any

PROTO = 1
MAX_LINE = 65536
_STREAM_MAC_CHARS = 16


class ProtocolError(ValueError):
    """A message could not be understood."""


def sign(key: bytes, text: str, chars: int = 32) -> str:
    return hmac.new(key, text.encode(), hashlib.sha256).hexdigest()[:chars]


def hello_mac(key: bytes, session: str, nonce: str) -> str:
    return sign(key, f"hello:{session}:{nonce}")


def encode(message: dict[str, Any]) -> bytes:
    return (json.dumps(message, separators=(",", ":"), sort_keys=True) + "\n").encode()


def decode(line: bytes | str) -> dict[str, Any]:
    try:
        message = json.loads(line)
    except ValueError as exc:
        raise ProtocolError(f"invalid JSON: {exc}") from None
    if not isinstance(message, dict) or not isinstance(message.get("t"), str):
        raise ProtocolError("a message must be an object with a string field 't'")
    return message


@dataclass(frozen=True, slots=True)
class StreamPacket:
    """One drive command: vx and wz are normalized to -1..1, held lists active hold actions."""

    session: str
    seq: int
    vx: float
    wz: float
    deadman: bool
    held: tuple[str, ...]


def _stream_text(packet: StreamPacket) -> str:
    held = ",".join(sorted(packet.held))
    drive = f"{packet.vx:.4f}:{packet.wz:.4f}:{int(packet.deadman)}"
    return f"{packet.session}:{packet.seq}:{drive}:{held}"


def encode_stream(key: bytes, packet: StreamPacket) -> bytes:
    packet = StreamPacket(
        packet.session,
        packet.seq,
        round(packet.vx, 4),
        round(packet.wz, 4),
        packet.deadman,
        tuple(sorted(packet.held)),
    )
    body = {
        "s": packet.session,
        "q": packet.seq,
        "vx": packet.vx,
        "wz": packet.wz,
        "dm": packet.deadman,
        "h": list(packet.held),
        "mac": sign(key, _stream_text(packet), _STREAM_MAC_CHARS),
    }
    return json.dumps(body, separators=(",", ":")).encode()


def decode_stream(key: bytes, data: bytes) -> StreamPacket | None:
    """Return the packet, or None if it is malformed or not signed with the key."""
    try:
        body = json.loads(data)
        packet = StreamPacket(
            str(body["s"]),
            int(body["q"]),
            float(body["vx"]),
            float(body["wz"]),
            bool(body["dm"]),
            tuple(sorted(str(name) for name in body["h"])),
        )
        mac = str(body["mac"])
    except ValueError, KeyError, TypeError:
        return None
    if not (math.isfinite(packet.vx) and math.isfinite(packet.wz)):
        return None
    if not hmac.compare_digest(mac, sign(key, _stream_text(packet), _STREAM_MAC_CHARS)):
        return None
    return packet
