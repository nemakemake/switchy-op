"""Small authenticated LAN service for downloading control profiles to the Switch."""

import asyncio
import contextlib
import hashlib
import hmac
import json
import secrets
from pathlib import Path
from typing import Any

import yaml

from switchy_op.bindings import load_profile

MAX_PROFILE = 32 * 1024
MAX_WIRE = 128 * 1024
MAX_PROFILES = 32


class ProfileServer:
    def __init__(self, directory: Path, key: str) -> None:
        self.directory = directory.resolve(strict=True)
        if not self.directory.is_dir() or not key:
            raise ValueError("profiles directory and a nonempty key are required")
        self.key = key.encode()

    def _read(self, name: str) -> dict[str, Any]:
        if (
            not name
            or len(name.encode()) > 120
            or Path(name).name != name
            or any(c in name for c in "/\\\r\n\t")
        ):
            raise ValueError("invalid profile name")
        path = (self.directory / name).resolve(strict=True)
        if path.parent != self.directory or path.suffix.lower() not in {".json", ".yaml", ".yml"}:
            raise ValueError("invalid profile path")
        if path.stat().st_size > MAX_PROFILE:
            raise ValueError("profile exceeds 32 KiB")
        profile = load_profile(path).model_dump(mode="json", exclude_none=True)
        if len(json.dumps(profile, ensure_ascii=False).encode()) > MAX_PROFILE:
            raise ValueError("converted profile exceeds 32 KiB")
        return profile

    def catalog(self) -> list[str]:
        names: list[str] = []
        for path in sorted(self.directory.iterdir()):
            try:
                self._read(path.name)
            except ValueError, OSError, UnicodeError, yaml.YAMLError:
                continue
            names.append(path.name)
            if len(names) >= MAX_PROFILES:
                break
        return names

    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        nonce = secrets.token_hex(16)
        try:
            writer.write(
                json.dumps({"service": "switchy-profiles", "version": 1, "nonce": nonce}).encode()
                + b"\n"
            )
            await writer.drain()
            line = await asyncio.wait_for(reader.readline(), 5)
            request = json.loads(line)
            if not isinstance(request, dict):
                return
            op, name = request.get("op"), request.get("name", "")
            if op not in {"list", "get"} or not isinstance(name, str):
                return
            expected = hmac.new(
                self.key, f"profiles-request:{nonce}:{op}:{name}".encode(), hashlib.sha256
            ).hexdigest()
            mac = request.get("mac")
            if not isinstance(mac, str) or not hmac.compare_digest(mac, expected):
                return
            try:
                if op == "list":
                    answer: dict[str, Any] = {"profiles": self.catalog()}
                else:
                    answer = {"name": name, "profile": self._read(name)}
            except ValueError, OSError, UnicodeError, yaml.YAMLError:
                answer = {"error": "Profile missing or invalid; refresh the list on Switch"}
            payload = json.dumps(answer, ensure_ascii=False, separators=(",", ":"))
            signature = hmac.new(
                self.key, f"profiles-response:{nonce}:{payload}".encode(), hashlib.sha256
            ).hexdigest()
            wire = json.dumps({"payload": payload, "mac": signature}, ensure_ascii=False).encode()
            if len(wire) <= MAX_WIRE:
                writer.write(wire + b"\n")
                await writer.drain()
        except ValueError, TypeError, OSError, TimeoutError, UnicodeError:
            pass
        finally:
            writer.close()
            with contextlib.suppress(OSError):
                await writer.wait_closed()


async def serve_profiles(directory: Path, host: str, port: int, key: str) -> None:
    service = ProfileServer(directory, key)
    server = await asyncio.start_server(service.handle, host, port, limit=4096)
    async with server:
        print(f"Profiles: {service.directory} ({len(service.catalog())} valid, max {MAX_PROFILES})")
        print(f"Profile server listening on {host}:{port}; on Switch press L in Connections.")
        await server.serve_forever()
