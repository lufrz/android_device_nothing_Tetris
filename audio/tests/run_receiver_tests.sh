#!/bin/bash
# Runs the production receiver with a simulated PCM device; no ADB or audio I/O.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../../../../.." && pwd -P)
audio="$repo/device/nothing/Tetris/audio"
test_out=$(mktemp -d "${TMPDIR:-/tmp}/tetris-receiver-tests.XXXXXX")
trap 'rm -rf -- "$test_out"' EXIT
clang="$repo/prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang++"
"$clang" -std=c++20 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
    -fno-omit-frame-pointer -I "$repo/external/tinyalsa/include" \
    "$audio/StereoReceiver.cpp" "$audio/tests/StereoReceiverTest.cpp" \
    -pthread -Wl,--wrap=ioctl -o "$test_out/receiver"
ASAN_OPTIONS=detect_leaks=0 "$test_out/receiver"
