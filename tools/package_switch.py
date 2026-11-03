"""Package a built Switch application and its default configs for a GitHub release."""

import hashlib
import re
import shutil
import struct
import zipfile
from pathlib import Path


def main() -> None:
    root = Path(__file__).resolve().parent.parent
    makefile = (root / "native/switch/Makefile").read_text(encoding="utf-8")
    match = re.search(r"^APP_VERSION\s*:=\s*(\S+)", makefile, re.MULTILINE)
    if match is None:
        raise ValueError("APP_VERSION is missing")
    version = match[1]
    nro = root / "native/switch/switchy-op.nro"
    data = nro.read_bytes()
    if len(data) < 32 or data[0x10:0x14] != b"NRO0":
        raise ValueError("Invalid NRO header; rebuild native/switch")
    size = struct.unpack_from("<I", data, 0x18)[0]
    if data[size : size + 4] != b"ASET":
        raise ValueError("Missing NRO assets")
    dist = root / "dist"
    dist.mkdir(exist_ok=True)
    standalone = dist / f"switchy-op-{version}.nro"
    shutil.copyfile(nro, standalone)
    package = dist / f"switchy-op-{version}-switch.zip"
    with zipfile.ZipFile(package, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.write(nro, "switch/switchy-op/switchy-op.nro")
        archive.write(root / "profiles/gamepad.json", "switch/switchy-op/profile.json")
        archive.write(root / "profiles/video.json", "switch/switchy-op/video.json")
        for name in (
            "docs/quick-start.md",
            "docs/profile-sync.md",
            "docs/video.md",
            "guides/switch-install.md",
        ):
            archive.write(root / name, name)
        archive.writestr(
            "README.txt",
            f"""switchy-op {version} - Nintendo Switch homebrew

UPDATE: replace only switch/switchy-op/switchy-op.nro on your SD card.
Keep your profile.json, video.json, connections.json and last-session.json.

Quick start: connect once; next launch A resumes the last successful profile/address,
B opens the manager. Release buttons after connecting, then press deadman to drive.

Wi-Fi profiles: on PC run from the updated project checkout:
  uv run switchy-op serve-profiles --directory profiles --key my-download-key
On Switch press L in Connections. Enter PC IP, port 7080 and the same key.

Video is enabled and required for driving by default. For camera-free control
put {{"enabled": false}} in switch/switchy-op/video.json.

See docs/quick-start.md, docs/profile-sync.md and docs/video.md (Russian).
Use hbmenu title override for full memory. Build and PC tests are verified;
video, touchscreen and SD behavior still require testing on a real console.
""",
        )
    with zipfile.ZipFile(package) as archive:
        if archive.testzip() is not None:
            raise ValueError("Archive validation failed")
    checksums = dist / f"switchy-op-{version}-SHA256SUMS.txt"
    checksums.write_text(
        "".join(
            f"{hashlib.sha256(path.read_bytes()).hexdigest()}  {path.name}\n"
            for path in (standalone, package)
        ),
        encoding="utf-8",
    )
    for path in (standalone, package, checksums):
        print(path)


if __name__ == "__main__":
    main()
