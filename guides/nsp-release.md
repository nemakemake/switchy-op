# Standalone NSP (experimental)

This package contains the application executable itself, without an NRO forwarder.
Title ID: `0100f5370a000000` (project choice; not a registered allocation).

1. Rebuild the current sources with devkitPro: run `make` in `native/switch`.
2. Run `powershell -ExecutionPolicy Bypass -File tools/prepare-nsp.ps1`.
   This prepares ExeFS, control metadata and the 256×256 JPEG icon in `build/nsp`.
3. Package using a locally installed hacBrewPack and your local keyset:

```powershell
./tools/prepare-nsp.ps1 -HacBrewPack C:/tools/hacbrewpack.exe -Keyset C:/private/prod.keys
```

Output: `dist/nsp`. Keep keys outside the repository and release archives.
Packaging requirements: https://github.com/rlaphoenix/hacBrewPack
The NPDM started from that project's template and uses its broad homebrew permissions.

The application still requires `switch/switchy-op/profile.json` on the SD card.
Copy the example from `profiles/gamepad.json` and configure the gateway address and key.
Saved connections remain in `switch/switchy-op/connections.json`.

Prepared NSO/NPDM inputs are not an installable NSP. Before publishing a release,
verify installation, HOME launch, software keyboard, networking, exit and reconnect
on the target console. Standalone launch has not yet been tested on hardware.
