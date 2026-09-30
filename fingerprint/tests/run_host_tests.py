#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile the real HAL illumination controller with host transport/sysfs fakes."""
import os
from pathlib import Path
import subprocess
import tempfile

TESTS = Path(__file__).resolve().parent
SOURCE = TESTS.parent
HEADERS = [
    "aidl/android/hardware/biometrics/fingerprint/ISessionCallback.h",
    "aidl/vendor/nothing/hardware/udfps/BnIlluminationCallback.h",
    "aidl/vendor/nothing/hardware/udfps/IIllumination.h",
    "android-base/file.h", "android-base/parseint.h", "android-base/strings.h",
    "android/binder_manager.h", "fingerprint.sysprop.h", "log/log.h",
]
with tempfile.TemporaryDirectory(prefix="tetris-fingerprint-test-") as temporary:
    output = Path(temporary)
    for relative in HEADERS:
        header = output / relative
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('#include "illumination_stubs.inc"\n')
    executable = output / "illumination-test"
    subprocess.run([
        os.environ.get("CXX", "g++"), "-std=c++17", "-pthread", "-Wall", "-Wextra",
        "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
        "-I", str(output), "-I", str(TESTS), "-I", str(SOURCE / "include"),
        str(SOURCE / "IlluminationController.cpp"),
        str(TESTS / "IlluminationControllerTest.cpp"), "-o", str(executable),
    ], check=True, timeout=60)
    subprocess.run([str(executable)], check=True, timeout=15)
