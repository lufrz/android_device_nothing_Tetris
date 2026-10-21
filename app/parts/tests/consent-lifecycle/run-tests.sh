#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
set -euo pipefail
test_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
tree_root=$(cd -- "$test_dir/../../../../../../.." && pwd)
test_output=$(mktemp -d "${TMPDIR:-/tmp}/tetris-consent-lifecycle.XXXXXX")
trap 'rm -rf -- "$test_output"' EXIT
java_root=${JAVA_HOME:-$tree_root/prebuilts/jdk/jdk21/linux-x86}
python3 "$test_dir/check-platform.py" "$tree_root"
JAVA_HOME="$java_root" "$tree_root/external/kotlinc/bin/kotlinc" \
    "$test_dir/../../src/org/lineageos/settings/tetris/consent/TermsLifecycle.kt" \
    "$test_dir"/stubs/*.kt "$test_dir/TermsLifecycleTest.kt" \
    -nowarn -include-runtime -d "$test_output/tests.jar"
"$java_root/bin/java" -jar "$test_output/tests.jar"
