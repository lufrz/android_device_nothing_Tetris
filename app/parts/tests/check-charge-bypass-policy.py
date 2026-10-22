#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify charge-bypass permissions against an offline compiled Android policy."""
import argparse
import ctypes as C
from pathlib import Path
import sys

PARTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(PARTS / "thermal/tests"))
from check_property_access import OfflinePolicy

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--policy", type=Path, required=True)
parser.add_argument("--libsepol", type=Path)
args = parser.parse_args()
policy = OfflinePolicy(args.policy, args.libsepol)
lib = policy.lib
lib.sepol_genfs_sid.argtypes = [C.c_char_p, C.c_char_p, C.c_uint16, C.POINTER(C.c_uint32)]
lib.sepol_sid_to_context.argtypes = [C.c_uint32, C.POINTER(C.c_char_p), C.POINTER(C.c_size_t)]
libc = C.CDLL(None)
libc.free.argtypes = [C.c_void_p]
checks = 0


def require(condition, message):
    global checks
    checks += 1
    if not condition:
        raise SystemExit("FAIL: " + message)


def context(fs, path, klass="file"):
    cls, sid = C.c_uint16(), C.c_uint32()
    require(lib.sepol_string_to_security_class(klass.encode(), C.byref(cls)) == 0,
            "resolve class " + klass)
    require(lib.sepol_genfs_sid(fs.encode(), path.encode(), cls, C.byref(sid)) == 0,
            "resolve label for " + path)
    result, size = C.c_char_p(), C.c_size_t()
    require(lib.sepol_sid_to_context(sid, C.byref(result), C.byref(size)) == 0,
            "read label for " + path)
    try:
        return result.value.decode()
    finally:
        libc.free(result)


def access(domain, target, permission, expected, klass="file"):
    actual, _ = policy.allowed(f"u:r:{domain}:s0", target, klass, permission)
    require(actual == expected, f"{domain} {target} {klass}:{permission} = {expected}")


control = context("sysfs", "/devices/platform/charger/charge_bypass")
status = context("sysfs", "/devices/platform/charger/charge_bypass_status")
require(control == "u:object_r:vendor_tetris_charge_bypass:s0", "dedicated request label")
require(status == "u:object_r:vendor_tetris_charge_bypass_status:s0", "dedicated status label")
for target in (control, status):
    for permission in ("getattr", "open", "read"):
        access("tetris_parts_app", target, permission, True)
    access("tetris_parts_app", target, "write", target == control)
    access("tetris_parts_app", target, "setattr", False)
    access("vendor_init", target, "setattr", True)
    for domain in ("untrusted_app", "system_app", "shell"):
        access(domain, target, "write", False)

for path in ("/devices", "/devices/platform", "/devices/platform/charger"):
    access("tetris_parts_app", context("sysfs", path, "dir"), "search", True, "dir")

# The feature must not grant access to ordinary charging policy or current limits.
for fs, path in (
    ("proc", "/charger/usb_charger_en"),
    ("proc", "/charger/scenario_fcc"),
    ("proc", "/mtk_battery_cmd/current_cmd"),
    ("sysfs", "/devices/platform/charger/enable_power_path"),
):
    access("tetris_parts_app", context(fs, path), "write", False)

access("hal_lineage_health_default", context("proc", "/charger/usb_charger_en"),
       "write", True)
print(f"PASS: {checks} effective charge-bypass policy checks")
