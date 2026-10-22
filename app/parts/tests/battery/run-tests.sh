#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tree_root=$(cd -- "$test_dir/../../../../../../.." && pwd)
source_dir="$test_dir/../../src/org/lineageos/settings/tetris"
test_output=$(mktemp -d "${TMPDIR:-/tmp}/tetris-charge-bypass.XXXXXX")
trap 'rm -rf -- "$test_output"' EXIT
java_root=${JAVA_HOME:-$tree_root/prebuilts/jdk/jdk21/linux-x86}
JAVA_HOME="$java_root" "$tree_root/external/kotlinc/bin/kotlinc" \
    "$source_dir/battery/ChargeBypassManager.kt" "$source_dir/battery/ChargeBypassSession.kt" \
    "$source_dir/consent/PartsControlGate.kt" "$test_dir/ChargeBypassManagerTest.kt" \
    -include-runtime -d "$test_output/tests.jar"
"$java_root/bin/java" -jar "$test_output/tests.jar"
