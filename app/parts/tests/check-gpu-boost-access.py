#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Verify existing PowerHAL APIBOOST access against compiled policy and packaged init files.

This loads an offline userspace policy only. It never reads or writes live GPU nodes.
The proc node is shared with Parts' DEBUG control; SELinux cannot restrict row payloads.
"""
import argparse
import ctypes as C
import hashlib
import json
import re
from pathlib import Path
import sys

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--policy', type=Path, required=True)
parser.add_argument('--libsepol', type=Path, required=True)
parser.add_argument('--gpu-init-rc', type=Path, required=True)
parser.add_argument('--power-service-rc', type=Path, required=True)
parser.add_argument('--vendor-file-contexts', type=Path, required=True)
parser.add_argument('--report', type=Path)
args = parser.parse_args()
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'thermal/tests'))
from check_property_access import OfflinePolicy
policy = OfflinePolicy(args.policy, args.libsepol)
lib = policy.lib
lib.sepol_genfs_sid.argtypes = [C.c_char_p, C.c_char_p, C.c_uint16, C.POINTER(C.c_uint32)]
lib.sepol_sid_to_context.argtypes = [C.c_uint32, C.POINTER(C.c_char_p), C.POINTER(C.c_size_t)]
lib.sepol_transition_sid.argtypes = [C.c_uint32, C.c_uint32, C.c_uint16, C.POINTER(C.c_uint32)]
libc = C.CDLL(None)
libc.free.argtypes = [C.c_void_p]
checks = []


def expect(description, actual, expected):
    checks.append({'check': description, 'actual': actual, 'expected': expected,
                   'passed': actual == expected})


def security_class(name):
    result = C.c_uint16()
    assert lib.sepol_string_to_security_class(name.encode(), C.byref(result)) == 0
    return result


def sid(context):
    result = C.c_uint32()
    encoded = context.encode()
    assert lib.sepol_context_to_sid(encoded, len(encoded) + 1, C.byref(result)) == 0
    return result


def context(security_id):
    result = C.c_char_p()
    size = C.c_size_t()
    assert lib.sepol_sid_to_context(security_id, C.byref(result), C.byref(size)) == 0
    try:
        return result.value.decode()
    finally:
        libc.free(result)


def genfs(filesystem, path, klass='file'):
    result = C.c_uint32()
    assert lib.sepol_genfs_sid(filesystem.encode(), path.encode(), security_class(klass), C.byref(result)) == 0
    return context(result)


def access(domain, target, klass, permission, expected=True):
    actual, _ = policy.allowed('u:r:' + domain + ':s0', target, klass, permission)
    expect(f'{domain} -> {target} {klass}:{permission}', actual, expected)


for path, expected in [('/', 'proc'), ('/gpufreqv2', 'proc_gpufreqv2')]:
    label = genfs('proc', path, 'dir')
    expect('Parent label ' + path, label, 'u:object_r:' + expected + ':s0')
    access('hal_power_default', label, 'dir', 'search')
node = genfs('proc', '/gpufreqv2/limit_table')
expect('Existing range node label', node, 'u:object_r:vendor_tetris_gpu_range:s0')
for permission in ['getattr', 'open', 'read', 'write', 'append', 'lock', 'map', 'ioctl']:
    access('hal_power_default', node, 'file', permission)
for permission in ['getattr', 'open', 'read', 'write']:
    access('tetris_parts_app', node, 'file', permission)
for permission in ['append', 'setattr', 'create', 'unlink']:
    access('tetris_parts_app', node, 'file', permission, False)
for domain in ['untrusted_app', 'system_app', 'mtk_hal_power']:
    access(domain, node, 'file', 'write', False)
for path in ['custom_boost_gpu_freq', 'custom_upbound_gpu_freq', 'gpu_boost_level']:
    sibling = genfs('sysfs', '/kernel/ged/hal/' + path)
    expect('GED node unchanged ' + path, sibling, 'u:object_r:sysfs_ged:s0')
    for permission in ['open', 'read', 'write']:
        access('tetris_parts_app', sibling, 'file', permission, False)

executable = '/vendor/bin/hw/android.hardware.power-service.pixel-libperfmgr'
exact_pattern = r'/vendor/bin/hw/android\.hardware\.power-service\.pixel-libperfmgr'
rules = [line.split() for line in args.vendor_file_contexts.read_text().splitlines()
         if line.strip() and not line.lstrip().startswith('#')]
exact_contexts = [fields[-1] for fields in rules if fields[0] == exact_pattern]
expect('Packaged exact PowerHAL executable label', exact_contexts, ['u:object_r:hal_power_default_exec:s0'])
transition = C.c_uint32()
assert lib.sepol_transition_sid(sid('u:r:init:s0'), sid('u:object_r:hal_power_default_exec:s0'),
                               security_class('process'), C.byref(transition)) == 0
expect('Compiled init -> PowerHAL domain transition', context(transition), 'u:r:hal_power_default:s0')
access('init', 'u:r:hal_power_default:s0', 'process', 'transition')
access('hal_power_default', 'u:object_r:hal_power_default_exec:s0', 'file', 'entrypoint')

rc = args.power_service_rc.read_text()
service = re.search(r'^service\s+vendor\.power-hal-aidl\s+(\S+)\n((?:[ \t]+[^\n]*\n|\n)*)', rc, re.M)
expect('PowerHAL service exists', service is not None, True)
if service:
    expect('Service executable', service.group(1), executable)
    expect('PowerHAL DAC user', re.findall(r'^\s+user\s+(\S+)', service.group(2), re.M), ['root'])
    groups = re.findall(r'^\s+group\s+([^\n]+)', service.group(2), re.M)
    expect('PowerHAL system group', any('system' in group.split() for group in groups), True)
gpu_rc = args.gpu_init_rc.read_text()
expect('GPU nodes configured after modules ready',
       'on property:vendor.all.modules.ready=1' in gpu_rc, True)
expect('Existing range-node owner/group', re.findall(r'^\s*chown\s+(\S+)\s+(\S+)\s+/proc/gpufreqv2/limit_table\s*$', gpu_rc, re.M), [('root', 'system')])
expect('Existing range-node DAC mode', re.findall(r'^\s*chmod\s+(\S+)\s+/proc/gpufreqv2/limit_table\s*$', gpu_rc, re.M), ['0660'])

failed = [item for item in checks if not item['passed']]
report = {'passed': not failed, 'checks': checks,
          'policy': str(args.policy.resolve()),
          'policy_sha256': hashlib.sha256(args.policy.read_bytes()).hexdigest(),
          'note': 'APIBOOST and DEBUG share the existing proc file; row ownership is enforced by controller code, not SELinux.'}
if args.report:
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
print(f'{len(checks)} compiled PowerHAL GPU boost access/domain/DAC checks; {len(failed)} failures')
for item in failed:
    print(f"FAIL {item['check']}: got {item['actual']}, expected {item['expected']}")
raise SystemExit(bool(failed))
