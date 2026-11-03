"""End-to-end profile transfer tests. Build profile_sync_check first (check_pc.sh does it)."""

import asyncio
import hashlib
import hmac
import json
import os
import tempfile
import unittest
from pathlib import Path
from typing import Any

from switchy_op.profile_server import ProfileServer

ROOT = Path(__file__).resolve().parent.parent
CHECK = Path(
    os.environ.get(
        "SWITCHY_PROFILE_CHECK",
        str(
            ROOT / "build" / ("profile_sync_check.exe" if os.name == "nt" else "profile_sync_check")
        ),
    )
)
KEY = "profile-test-key"


class TransferTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.folder = self.root / "shared"
        self.folder.mkdir()
        self.original = json.loads((ROOT / "profiles/gamepad.json").read_text())
        self.changed = json.loads(json.dumps(self.original))
        self.changed["drive"]["forward"]["invert"] = False
        self.changed["drive"]["turn"]["scale"] = 0.45
        (self.folder / "rover.json").write_text(json.dumps(self.changed))
        (self.folder / "invalid.yaml").write_text("drive: [broken")
        (self.folder / "video.json").write_text('{"enabled": true}')
        (self.folder / "oversize.json").write_text(" " * (33 * 1024))
        self.destination = self.root / "profile.json"
        self.destination.write_text(json.dumps(self.original))
        self.service = ProfileServer(self.folder, KEY)
        self.server = await asyncio.start_server(self.service.handle, "127.0.0.1", 0, limit=4096)
        self.port = self.server.sockets[0].getsockname()[1]

    async def asyncTearDown(self) -> None:
        self.server.close()
        await self.server.wait_closed()
        self.temporary.cleanup()

    async def native(
        self,
        key: str = KEY,
        name: str = "rover.json",
        destination: Path | None = None,
        port: int | None = None,
    ) -> tuple[int, str]:
        process = await asyncio.create_subprocess_exec(
            str(CHECK),
            str(port or self.port),
            key,
            name,
            str(destination or self.destination),
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
        )
        stdout, _ = await asyncio.wait_for(process.communicate(), 15)
        return process.returncode or 0, stdout.decode(errors="replace")

    async def exchange(self, op: str, name: str) -> dict[str, Any]:
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        try:
            challenge = json.loads(await reader.readline())
            nonce = challenge["nonce"]
            signature = hmac.new(
                KEY.encode(), f"profiles-request:{nonce}:{op}:{name}".encode(), hashlib.sha256
            ).hexdigest()
            writer.write(json.dumps({"op": op, "name": name, "mac": signature}).encode() + b"\n")
            await writer.drain()
            envelope = json.loads(await reader.readline())
            expected = hmac.new(
                KEY.encode(),
                f"profiles-response:{nonce}:{envelope['payload']}".encode(),
                hashlib.sha256,
            ).hexdigest()
            self.assertEqual(envelope["mac"], expected)
            answer: dict[str, Any] = json.loads(envelope["payload"])
            return answer
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_native_apply_backup_and_second_update(self) -> None:
        code, output = await self.native()
        self.assertEqual(code, 0, output)
        self.assertIn("PROFILE rover.json", output)
        self.assertNotIn("invalid.yaml", output)
        self.assertEqual(json.loads(self.destination.read_text()), self.changed)
        backup = self.destination.with_suffix(".json.bak")
        self.assertEqual(json.loads(backup.read_text()), self.original)
        # Replacing an existing profile and backup must work on Windows as well as Switch.
        code, output = await self.native()
        self.assertEqual(code, 0, output)
        self.assertEqual(json.loads(backup.read_text()), self.changed)

    async def test_wrong_key_preserves_profile(self) -> None:
        before = self.destination.read_bytes()
        code, _ = await self.native(key="wrong-key")
        self.assertNotEqual(code, 0)
        self.assertEqual(self.destination.read_bytes(), before)
        self.assertFalse(self.destination.with_suffix(".json.bak").exists())

    async def test_yaml_profile_is_converted_for_native_client(self) -> None:
        (self.folder / "keyboard.yaml").write_bytes((ROOT / "profiles/gamepad.yaml").read_bytes())
        code, output = await self.native(name="keyboard.yaml")
        self.assertEqual(code, 0, output)
        loaded = json.loads(self.destination.read_text())
        self.assertEqual(loaded["drive"]["deadman"], "zr")
        self.assertIn("light", loaded["actions"])

    async def test_invalid_and_missing_profile_preserve_profile(self) -> None:
        for name in ("invalid.yaml", "missing.json", "oversize.json"):
            code, _ = await self.native(name=name)
            self.assertNotEqual(code, 0)
            self.assertEqual(json.loads(self.destination.read_text()), self.original)

    async def test_path_traversal_rejected(self) -> None:
        outside = self.root / "outside.json"
        outside.write_text(json.dumps(self.original))
        for name in ("../outside.json", str(outside), "..\\outside.json"):
            self.assertIn("error", await self.exchange("get", name))

    async def test_save_failure_preserves_existing_profile(self) -> None:
        # An obstructed temporary path deterministically simulates an SD write failure.
        self.destination.with_suffix(".json.tmp").mkdir()
        code, _ = await self.native()
        self.assertNotEqual(code, 0)
        self.assertEqual(json.loads(self.destination.read_text()), self.original)

    async def test_first_install_without_existing_profile(self) -> None:
        fresh = self.root / "new-profile.json"
        code, output = await self.native(destination=fresh)
        self.assertEqual(code, 0, output)
        self.assertEqual(json.loads(fresh.read_text()), self.changed)

    async def test_interrupted_replacement_recovers_previous_profile(self) -> None:
        # Simulate power loss between moving the old file aside and installing the new one.
        self.destination.rename(self.destination.with_suffix(".json.rollback"))
        code, _ = await self.native(key="wrong-key")
        self.assertNotEqual(code, 0)
        self.assertEqual(json.loads(self.destination.read_text()), self.original)

    async def test_switch_rollback_obstruction_preserves_profile(self) -> None:
        if "SWITCHY_PROFILE_CHECK" not in os.environ:
            self.skipTest("only relevant with Switch no-overwrite rename emulation")
        self.destination.with_suffix(".json.rollback").mkdir()
        code, _ = await self.native()
        self.assertNotEqual(code, 0)
        self.assertEqual(json.loads(self.destination.read_text()), self.original)

    async def test_tampered_signed_response_rejected_by_native(self) -> None:
        async def forged(reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
            nonce = "a" * 32
            writer.write(
                json.dumps({"service": "switchy-profiles", "version": 1, "nonce": nonce}).encode()
                + b"\n"
            )
            await writer.drain()
            await reader.readline()
            writer.write(
                json.dumps({"payload": '{"profiles":["rover.json"]}', "mac": "0" * 64}).encode()
                + b"\n"
            )
            await writer.drain()
            writer.close()
            await writer.wait_closed()

        server = await asyncio.start_server(forged, "127.0.0.1", 0)
        try:
            port = server.sockets[0].getsockname()[1]
            code, output = await self.native(port=port)
            self.assertNotEqual(code, 0)
            self.assertIn("signature invalid", output)
            self.assertEqual(json.loads(self.destination.read_text()), self.original)
        finally:
            server.close()
            await server.wait_closed()


if __name__ == "__main__":
    unittest.main()
