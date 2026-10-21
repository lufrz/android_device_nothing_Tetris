#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tree_root=$(cd -- "$test_dir/../../../../../../.." && pwd)
source_dir="$test_dir/../../src/org/lineageos/settings/tetris/consent"
test_output=$(mktemp -d "${TMPDIR:-/tmp}/tetris-terms-activity.XXXXXX")
trap 'rm -rf -- "$test_output"' EXIT
java_root=${JAVA_HOME:-$tree_root/prebuilts/jdk/jdk21/linux-x86}
coroutines="$tree_root/external/kotlinc/lib/kotlinx-coroutines-core-jvm.jar"
JAVA_HOME="$java_root" "$tree_root/external/kotlinc/bin/kotlinc" \
    "$source_dir/TermsActivity.kt" "$source_dir/TermsState.kt" \
    "$test_dir"/activity-stubs/*.kt "$test_dir/TermsActivityTest.kt" \
    -classpath "$coroutines" -nowarn -include-runtime -d "$test_output/activity-tests.jar"
"$java_root/bin/java" -cp "$test_output/activity-tests.jar:$coroutines" \
    org.lineageos.settings.tetris.consent.TermsActivityTestKt
