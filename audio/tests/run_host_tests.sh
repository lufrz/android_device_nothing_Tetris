#!/bin/bash
# All HAL objects and kernel calls are mocks; this script does not use ADB.
set -euo pipefail
repo=$(cd -- "$(dirname -- "$0")/../../../../.." && pwd -P)
audio="$repo/device/nothing/Tetris/audio"
test_out=$(mktemp -d "${TMPDIR:-/tmp}/tetris-audio-tests.XXXXXX")
trap 'rm -rf -- "$test_out"' EXIT
clang="$repo/prebuilts/clang/host/linux-x86/clang-r584948b/bin/clang++"
flags=(-std=c++20 -Wall -Wextra -Werror)
includes=(-isystem "$repo/hardware/libhardware/include_all"
    -isystem "$repo/system/media/audio/include"
    -isystem "$repo/system/core/libcutils/include"
    -isystem "$repo/system/logging/liblog/include"
    -isystem "$repo/system/core/libsystem/include"
    -isystem "$repo/system/libbase/include")
"$clang" "${flags[@]}" "$audio/tests/AudioModeTest.cpp" -o "$test_out/mode"
"$test_out/mode"
"$clang" "${flags[@]}" "${includes[@]}" -O1 -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer "$audio/tests/PlaybackMetadataTest.cpp" -o "$test_out/metadata"
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$test_out/metadata"
"$clang" "${flags[@]}" "${includes[@]}" -O1 -g -fsanitize=address,undefined \
    -fno-omit-frame-pointer "$audio/tests/AudioProxyTest.cpp" -pthread \
    -ldl -Wl,--wrap=open,--wrap=close,--wrap=ioctl,--wrap=dlopen,--wrap=dlsym -o "$test_out/proxy"
TETRIS_AUDIO_STATUS_FIXTURES="$test_out/status.txt" \
    ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 "$test_out/proxy"
# Exercise the same reports against the real app parser, including idle and
# unavailable states. Separate C++/Kotlin mocks can otherwise agree with themselves.
java_root="$repo/prebuilts/jdk/jdk21/linux-x86"
JAVA_HOME="$java_root" "$repo/external/kotlinc/bin/kotlinc" \
    "$repo/device/nothing/Tetris/app/parts/src/org/lineageos/settings/tetris/audio/ExperimentalAudioState.kt" \
    "$audio/tests/PartsAudioStatusTest.kt" \
    -include-runtime -d "$test_out/status-test.jar"
"$java_root/bin/java" -jar "$test_out/status-test.jar" "$test_out/status.txt"
"$clang" "${flags[@]}" "$audio/tests/HookScopeTest.cpp" -ldl -pthread -o "$test_out/scopes"
"$test_out/scopes"
"$clang" "${flags[@]}" "$audio/tests/BessoundProbeTest.cpp" -ldl -pthread -o "$test_out/bessound"
"$test_out/bessound"
"$clang" "${flags[@]}" "$audio/tests/DspStereoHookTest.cpp" -ldl -pthread -o "$test_out/dsp"
"$test_out/dsp"
# Defining the architecture here exercises fingerprint rejection against a real
# host ELF function; no ARM instructions are emitted or executed by this test.
"$clang" "${flags[@]}" -D__aarch64__ -fPIC -shared "$audio/DownmixHook.cpp" "$audio/BessoundProbe.cpp" "$audio/DspStereoHook.cpp" \
    -ldl -Wl,-z,global,--version-script="$audio/hook.map.txt" -o "$test_out/libhook.so"
"$clang" "${flags[@]}" -fPIC -shared "$audio/tests/HookVendor.cpp" -o "$test_out/vendor.so"
"$clang" "${flags[@]}" "$audio/tests/DownmixHookTest.cpp" -L"$test_out" -lhook \
    -ldl -Wl,-rpath,"$test_out" -o "$test_out/hook"
"$test_out/hook" "$test_out/vendor.so"

# Separate production receiver worker/clock tests with simulated ALSA hardware.
bash "$audio/tests/run_receiver_tests.sh"
