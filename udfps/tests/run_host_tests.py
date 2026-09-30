#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run pure Java illumination checks using the Android checkout's JDK."""
from pathlib import Path
import os
import shutil
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET

module = Path(__file__).resolve().parents[1]
root = next((p for p in module.parents if (p / "build/envsetup.sh").is_file()), None)
java_home = Path(os.environ["JAVA_HOME"]) if "JAVA_HOME" in os.environ else None
if java_home is None and root is not None:
    java_home = root / "prebuilts/jdk/jdk21/linux-x86"
java = str(java_home / "bin/java") if java_home else shutil.which("java")
javac = str(java_home / "bin/javac") if java_home else shutil.which("javac")
resources = ET.parse(module / "res/values/calibration.xml").getroot()
arrays = resources.findall("integer-array")
assert len(arrays) == 1, "Expected exactly one panel transmission curve"
samples = [int(item.text) for item in arrays[0].findall("item")]
assert len(samples) == 256, "Incomplete calibration curve"
with tempfile.TemporaryDirectory(prefix="tetris-udfps-tests-") as temporary:
    temp = Path(temporary)
    fixture = temp / "transmission.txt"
    fixture.write_text("".join(f"{sample}\n" for sample in samples))
    sources = module / "src/org/lineageos/tetris/udfps"
    subprocess.run([javac, "-d", temporary, str(sources / "Calibration.java"),
                    str(sources / "SensorGeometry.java"),
                    str(module / "tests/IlluminationMathTest.java")], check=True)
    subprocess.run([java, "-cp", temporary,
                    "org.lineageos.tetris.udfps.IlluminationMathTest", str(fixture)], check=True)
    compiler = os.environ.get("CXX") or shutil.which("c++")
    if not compiler:
        raise RuntimeError("A C++17 compiler is required for raster tests")
    raster_test = temp / "raster_test"
    subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-I", str(module / "jni"),
                    str(module / "tests/IlluminationRasterTest.cpp"),
                    "-o", str(raster_test)], check=True)
    subprocess.run([str(raster_test)], check=True)

subprocess.run([sys.executable, str(module / "tests/run_lifecycle_tests.py")], check=True)
subprocess.run([sys.executable, str(module / "tests/run_sensor_lifecycle_tests.py")], check=True)
