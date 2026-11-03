#!/usr/bin/env bash
# Builds the native client for this PC and runs it against a real Python gateway.
# Needs g++ (C++20) and uv. Usage: native/check_pc.sh
set -euo pipefail
cd "$(dirname "$0")/.."

mkdir -p build
g++ -std=c++20 -O1 -Wall -Wextra -Wpedantic native/pc/video_selftest.cpp -o build/video_selftest
./build/video_selftest
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) libs="-lws2_32" ;;
    *) libs="" ;;
esac

g++ -std=c++20 -O1 -Wall -Wextra -Wpedantic \
    native/core/bindings.cpp native/core/profile_sync.cpp native/core/json.cpp \
    native/core/net.cpp native/core/sha256.cpp native/pc/profile_sync_check.cpp \
    $libs -o build/profile_sync_check
uv run python native/check_profile_sync.py -v
g++ -std=c++20 -O1 -Wall -Wextra -Wpedantic \
    native/core/bindings.cpp native/core/profile_sync.cpp native/core/quick_start.cpp \
    native/core/json.cpp native/core/net.cpp native/core/sha256.cpp \
    native/pc/quick_start_selftest.cpp $libs -o build/quick_start_selftest
./build/quick_start_selftest
g++ -std=c++20 -O1 -Wall -Wextra -Wpedantic -DSWITCHY_RENAME_NO_REPLACE \
    native/core/bindings.cpp native/core/profile_sync.cpp native/core/json.cpp \
    native/core/net.cpp native/core/sha256.cpp native/pc/profile_sync_check.cpp \
    $libs -o build/profile_sync_fat_check
SWITCHY_PROFILE_CHECK=build/profile_sync_fat_check uv run python native/check_profile_sync.py -v

g++ -std=c++20 -O1 -Wall -Wextra -Wpedantic \
    native/core/*.cpp native/pc/main.cpp $libs -o build/pc_check

# The C++ HMAC must match Python's byte for byte, including at SHA-256 block boundaries.
g++ -std=c++20 -O1 -Wall -Wextra native/core/sha256.cpp native/pc/selftest.cpp -o build/selftest
uv run python - > build/hmac_expected.txt <<'PY'
import hashlib, hmac
cases = [(b"dev-key", b"hello:s1:n1"), (b"dev-key", b"")]
cases += [(b"k", b"x" * n) for n in (55, 56, 63, 64, 65)] + [(b"k", b"y" * 200)]
cases += [(b"K" * 100, b"long key message")]
for key, message in cases:
    print(hmac.new(key, message, hashlib.sha256).hexdigest())
PY
./build/selftest > build/hmac_actual.txt
diff build/hmac_expected.txt build/hmac_actual.txt && echo "HMAC-SHA256 matches Python"

# The gateway stops by itself, so there is no process to clean up.
PYTHONUNBUFFERED=1 uv run switchy-op gateway --port 7100 --udp-port 7101 --run-for 60 > build/gateway.log 2>&1 &

status=0
./build/pc_check --port 7100 --profile profiles/gamepad.json || status=$?
echo "--- gateway log (tail)"
tail -n 12 build/gateway.log || true
exit "$status"
