#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail

test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tree_root=$(cd -- "$test_dir/../../../../../../.." && pwd)
source_dir="$test_dir/../../src/org/lineageos/settings/tetris/audio"
test_output=$(mktemp -d "${TMPDIR:-/tmp}/tetris-audio-tests.XXXXXX")
trap 'rm -rf -- "$test_output"' EXIT
java_root=${JAVA_HOME:-$tree_root/prebuilts/jdk/jdk21/linux-x86}

JAVA_HOME="$java_root" "$tree_root/external/kotlinc/bin/kotlinc" \
    "$source_dir/../consent/PartsControlGate.kt" \
    "$source_dir/../thermal/ThermalController.kt" \
    "$source_dir/../network/TcpControlManager.kt" \
    "$source_dir/../network/TcpControlSession.kt" \
    "$source_dir/ExperimentalAudioState.kt" \
    "$source_dir/ExperimentalAudioRepository.kt" \
    "$test_dir/ExperimentalAudioControlTest.kt" \
    "$test_dir/ExperimentalAudioRepositoryTest.kt" \
    "$test_dir/ExperimentalAudioGainTest.kt" \
    "$test_dir/ExperimentalAudioSettingsTest.kt" \
    "$test_dir/stubs/AudioSystem.kt" \
    "$test_dir/stubs/SystemProperties.kt" \
    -include-runtime -d "$test_output/audio-tests.jar"
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioControlTestKt
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioRepositoryTestKt
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioRepositoryTestKt restart
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioGainTestKt
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioSettingsTestKt
"$java_root/bin/java" -cp "$test_output/audio-tests.jar" \
    org.lineageos.settings.tetris.audio.ExperimentalAudioSettingsTestKt restart
