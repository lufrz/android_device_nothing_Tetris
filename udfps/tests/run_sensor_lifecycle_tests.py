#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compile production Sensor.cpp with host Android types and simulated sysfs nodes."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile

module = Path(__file__).resolve().parents[1]
sensors = module.parent / 'sensors'
compiler = os.environ.get('CXX') or shutil.which('c++')
assert compiler, 'A C++17 compiler is required'
stubs = {
'android/hardware/sensors/2.1/types.h': r'''
#pragma once
#include <cstdint>
#include <string>
namespace android::hardware::sensors::V1_0 {
enum class OperationMode { NORMAL, DATA_INJECTION };
enum class Result { OK, BAD_VALUE, INVALID_OPERATION };
enum class MetaDataEventType { META_DATA_FLUSH_COMPLETE };
enum class SensorStatus { ACCURACY_HIGH };
enum class SensorFlagBits : uint32_t { WAKE_UP=1, ONE_SHOT_MODE=2, DATA_INJECTION=4 };
inline uint32_t& operator|=(uint32_t& flags,SensorFlagBits bit) {return flags|=static_cast<uint32_t>(bit);}
}
namespace android::hardware::sensors::V2_1 {
enum class SensorType : int32_t { META_DATA=0, ADDITIONAL_INFO=33, DEVICE_PRIVATE_BASE=0x10000 };
struct SensorInfo { int32_t sensorHandle=0,version=0,maxDelay=0,minDelay=0;
 uint32_t fifoReservedEventCount=0,fifoMaxEventCount=0,flags=0;
 std::string vendor,requiredPermission,name,typeAsString;SensorType type=SensorType::META_DATA;
 float maxRange=0,resolution=0,power=0;};
struct Event { int32_t sensorHandle=0;SensorType sensorType=SensorType::META_DATA;int64_t timestamp=0;
 struct {struct {V1_0::MetaDataEventType what;} meta;
 struct {float x,y,z;V1_0::SensorStatus status;}vec3;float data[16];}u;};
}
''',
'hardware/sensors.h': '#pragma once\n#include <cstdio>\n',
'log/log.h': '''#pragma once
#define ALOGE(...) ((void)0)
#define ALOGI(...) ((void)0)
#define ALOGW(...) ((void)0)
''',
'utils/SystemClock.h': '''#pragma once
#include <chrono>
namespace android {inline int64_t elapsedRealtimeNano() {return
 std::chrono::steady_clock::now().time_since_epoch().count();}}
''',
}
with tempfile.TemporaryDirectory(prefix='tetris-sensor-lifecycle-') as temporary:
    temp = Path(temporary)
    for name, text in stubs.items():
        file = temp / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
    binary = temp / 'sensor_lifecycle'
    subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-missing-field-initializers', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-ftrivial-auto-var-init=pattern',
                    '-I', str(temp), '-I', str(sensors), str(sensors / 'Sensor.cpp'),
                    '-x', 'c++', str(module / 'tests/SensorLifecycleTest.inc'),
                    '-Wl,--wrap=open,--wrap=pipe2,--wrap=close,--wrap=read,--wrap=lseek,--wrap=poll',
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)
