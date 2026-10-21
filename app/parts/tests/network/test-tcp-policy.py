#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the TCP selector's cross-file contract and optional offline binary policy.

The compiled-policy checks use libsepol in userspace; they never load or change the
host's SELinux policy. Run after compiling sepolicy with --policy and --libsepol.
"""
import argparse
import ctypes as C
from pathlib import Path
import re
import sys
import xml.etree.ElementTree as ET

parts = Path(__file__).resolve().parents[2]
device = parts.parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--policy", type=Path)
parser.add_argument("--libsepol", type=Path)
args = parser.parse_args()
checks = 0


def require(condition, description):
    global checks
    checks += 1
    if not condition:
        raise SystemExit("FAIL: " + description)


source = parts / "src/org/lineageos/settings/tetris/network"
manager = (source / "TcpControlManager.kt").read_text()
session = (source / "TcpControlSession.kt").read_text()
policy_dir = device / "sepolicy/private"
contexts = (policy_dir / "property_contexts").read_text()
policy_text = (policy_dir / "tetris_parts_network.te").read_text()
rc = (parts / "init.tetris_parts_network.rc").read_text()

supported_match = re.search(r"val SUPPORTED = listOf\(([^)]*)\)", manager)
require(supported_match is not None, "TCP algorithm allowlist exists")
supported = re.findall(r'"([a-z0-9_]+)"', supported_match[1])
require(len(supported) == len(set(supported)), "algorithm allowlist is unique")
request = "sys.tetrisparts.tcp_congestion"
saved = "persist.sys.tetrisparts.tcp_congestion"
ready = "sys.tetrisparts.tcp_ready"
restore_event = "tetris-parts-tcp-restore"
ready_event = "tetris-parts-tcp-ready"
sysctl = "/proc/sys/net/ipv4/tcp_congestion_control"
request_context = re.search(
    r"^" + re.escape(request) + r"\s+u:object_r:tetrisparts_tcp_request_prop:s0\s+exact enum (.+)$",
    contexts, re.M)
require(request_context is not None, "request property uses an exact enum context")
require(set(request_context[1].split()) == set(supported), "request enum matches the selector")
saved_context = re.search(
    r"^" + re.escape(saved) + r"\s+u:object_r:tetrisparts_tcp_request_prop:s0\s+exact enum (.+)$",
    contexts, re.M)
require(saved_context is not None, "saved selection uses the restricted request property label")
require(set(saved_context[1].split()) == {"off", *supported},
        "saved selection allows only off and supported algorithms")
require(re.search(r"^" + re.escape(ready) +
    r"\s+u:object_r:tetrisparts_tcp_status_prop:s0\s+exact bool$", contexts, re.M),
    "readiness is a separate bool property")

actions = {}
trigger = None
for raw in rc.splitlines():
    line = raw.split("#", 1)[0].strip()
    if not line:
        continue
    if line.startswith("on "):
        trigger = line[3:]
        require(trigger not in actions, "each init trigger occurs once")
        actions[trigger] = []
    else:
        require(trigger is not None and raw[0].isspace(), "init contains only action commands")
        actions[trigger].append(line)

require(actions.pop("property:sys.boot_completed=1", None) == [
    f"trigger {restore_event}", f"trigger {ready_event}"],
    "boot queues restore before a separate readiness event")
require(actions.pop(ready_event, None) == [f"setprop {ready} 1"],
        "readiness follows boot restoration")
live_triggers = {f"property:{request}={name}" for name in supported}
boot_triggers = {f"{restore_event} && property:{saved}={name}" for name in supported}
require(set(actions) == live_triggers | boot_triggers,
        "only fixed live and boot requests can write TCP")
for name in supported:
    require(actions[f"property:{request}={name}"] == [f"write {sysctl} {name}"],
            f"{name} writes one fixed value")
    require(actions[f"{restore_event} && property:{saved}={name}"] == [f"write {sysctl} {name}"],
            f"{name} restores one fixed saved value")
require(all("off" not in trigger for trigger in boot_triggers),
        "off or an absent saved selection leaves the boot default unchanged")
require(not any(trigger.startswith(f"property:{saved}=") for trigger in actions),
        "saving or disabling boot persistence cannot replay a live write")
require("${" not in rc and not re.search(r"^\s*(?:exec|exec_start|start)\b", rc, re.M),
        "init uses no expanded writes, shell, or external helper")
require(f'SystemProperties.set("{request}", algorithm)' in session,
        "Android wrapper writes the request property")
require(f'SystemProperties.get("{ready}") == "1"' in session,
        "Android wrapper checks readiness")
require('private const val BOOT_ALGORITHM = "persist.sys.tetrisparts.tcp_congestion"' in session
        and 'SystemProperties.get(BOOT_ALGORITHM, "off")' in session
        and 'SystemProperties.set(BOOT_ALGORITHM, algorithm)' in session,
        "Android wrapper uses the persistent selection with an opt-in default")
require(not re.search(r"writeText|FileOutputStream|Runtime\.getRuntime|ProcessBuilder", session),
        "Android wrapper neither writes proc directly nor invokes a shell")

require("set_prop(tetris_parts_app, tetrisparts_tcp_request_prop)" in policy_text,
        "Parts can set only its typed request through the property service")
require("get_prop(tetris_parts_app, tetrisparts_tcp_status_prop)" in policy_text,
        "Parts reads readiness")
require("set_prop(tetris_parts_app, tetrisparts_tcp_status_prop)" not in policy_text,
        "Parts cannot declare itself ready")
require(not re.search(r"\b(?:net_admin|net_raw|permissive|rw_file_perms|w_file_perms)\b",
                      policy_text), "new policy grants no network capability or proc write macro")
require(re.search(r"allow tetris_parts_app\s+\{\s*proc_tetris_tcp_control\s+"
                  r"proc_tetris_tcp_available\s*\}:file r_file_perms;", policy_text),
        "both proc nodes have explicit read-only app access")

build = (parts / "Android.bp").read_text()
module = re.search(r'prebuilt_etc\s*\{[^{}]*name:\s*"init.tetris_parts_network.rc"[^{}]*\}', build)
require(module is not None and "system_ext_specific: true" in module[0]
        and "vendor:" not in module[0], "TCP actions run from system_ext as core init")
require('"init.tetris_parts_network.rc"' in build.split("required:", 1)[1].split("]", 1)[0],
        "app installation requires the TCP init module")
manifest = ET.parse(parts / "AndroidManifest.xml").getroot()
ns = "{http://schemas.android.com/apk/res/android}"
permissions = {item.get(ns + "name") for item in manifest.findall("uses-permission")}
require({"android.permission.ACCESS_NETWORK_STATE", "android.permission.ACCESS_WIFI_STATE",
         "android.permission.READ_PHONE_STATE"} <= permissions, "observation permissions are declared")
require(not {"android.permission.MODIFY_PHONE_STATE", "android.permission.NETWORK_SETTINGS",
             "android.permission.CHANGE_WIFI_STATE", "android.permission.ACCESS_FINE_LOCATION",
             "android.permission.ACCESS_COARSE_LOCATION"} & permissions,
        "new network feature does not request radio control or location permissions")
print(f"PASS: {checks} TCP source-contract checks")

if not args.policy:
    print("Compiled policy not checked (pass --policy to verify effective access).")
    raise SystemExit(0)

sys.path.insert(0, str(parts / "thermal/tests"))
from check_property_access import OfflinePolicy
policy = OfflinePolicy(args.policy, args.libsepol)
lib = policy.lib
lib.sepol_genfs_sid.argtypes = [C.c_char_p, C.c_char_p, C.c_uint16, C.POINTER(C.c_uint32)]
lib.sepol_sid_to_context.argtypes = [C.c_uint32, C.POINTER(C.c_char_p), C.POINTER(C.c_size_t)]
libc = C.CDLL(None)
libc.free.argtypes = [C.c_void_p]


def proc_context(path):
    cls, sid = C.c_uint16(), C.c_uint32()
    require(lib.sepol_string_to_security_class(b"file", C.byref(cls)) == 0, "resolve file class")
    require(lib.sepol_genfs_sid(b"proc", path.encode(), cls, C.byref(sid)) == 0,
            "resolve compiled label for " + path)
    context, length = C.c_char_p(), C.c_size_t()
    require(lib.sepol_sid_to_context(sid, C.byref(context), C.byref(length)) == 0,
            "read compiled context for " + path)
    try:
        return context.value.decode()
    finally:
        libc.free(context)


def access(domain, target, klass, permission, expected):
    actual, _ = policy.allowed(f"u:r:{domain}:s0", target, klass, permission)
    require(actual == expected, f"{domain} {target} {klass}:{permission} = {expected}")


for node, label in (("tcp_congestion_control", "proc_tetris_tcp_control"),
                    ("tcp_available_congestion_control", "proc_tetris_tcp_available")):
    context = proc_context("/sys/net/ipv4/" + node)
    require(context == f"u:object_r:{label}:s0", "exact compiled genfs label for " + node)
    for permission in ("read", "open", "getattr"):
        access("tetris_parts_app", context, "file", permission, True)
    for permission in ("write", "append", "setattr"):
        access("tetris_parts_app", context, "file", permission, False)
    if node == "tcp_congestion_control":
        access("init", context, "file", "write", True)
        access("netd", context, "file", "write", True)

for node in ("ip_forward", "tcp_allowed_congestion_control", "tcp_rmem", "tcp_wmem"):
    context = proc_context("/sys/net/ipv4/" + node)
    require(context == "u:object_r:proc_net:s0", "other sysctls retain platform label: " + node)
    access("tetris_parts_app", context, "file", "write", False)

for label in ("tetrisparts_tcp_request_prop", "tetrisparts_tcp_status_prop"):
    context = f"u:object_r:{label}:s0"
    access("tetris_parts_app", context, "file", "read", True)
    access("init", context, "property_service", "set", True)
    access("tetris_parts_app", context, "property_service", "set", label.endswith("request_prop"))
    for domain in ("untrusted_app", "system_app", "shell", "radio", "vendor_init"):
        access(domain, context, "property_service", "set", False)
for permission in ("net_admin", "net_raw", "sys_admin"):
    access("tetris_parts_app", "u:r:tetris_parts_app:s0", "capability", permission, False)
for service in ("connectivity_service", "wifi_service", "radio_service", "registry_service"):
    access("tetris_parts_app", f"u:object_r:{service}:s0", "service_manager", "find", True)
    access("tetris_parts_app", f"u:object_r:{service}:s0", "service_manager", "add", False)
for server in ("system_server", "radio"):
    access("tetris_parts_app", f"u:r:{server}:s0", "binder", "call", True)
print(f"PASS: {checks} TCP contract and effective-policy checks")
