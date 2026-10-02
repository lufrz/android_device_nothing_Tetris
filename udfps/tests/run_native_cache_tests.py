#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the production JNI translation unit with controlled Android services."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--report', type=Path, help='Optional JSON evidence output')
args = parser.parse_args()
module = Path(__file__).resolve().parents[1]
root = next(p for p in module.parents if (p / 'build/envsetup.sh').is_file())
compiler = os.environ.get('CXX') or shutil.which('c++')
if not compiler:
    raise RuntimeError('A C++17 compiler is required for native cache tests')
source = (module / 'jni/IlluminationSurface.cpp').read_text()
android_headers = ['jni.h', 'binder/ProcessState.h', 'gui/SurfaceComposerClient.h',
                   'gui/SurfaceControl.h', 'log/log.h', 'ui/Fence.h', 'ui/GraphicBuffer.h',
                   'ui/GraphicBufferMapper.h', 'ui/GraphicTypes.h', 'ui/PixelFormat.h',
                   'utils/String8.h']
# Assertion failures in intentional mutants must not create core artifacts.
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='tetris-native-cache-') as temporary:
    temp = Path(temporary)
    for name in android_headers:
        stub = temp / name
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text('// Defined by NativeCacheSurfaceStubs.h.\n')
    for name in ['NativeCacheSurfaceStubs.h', 'NativeCacheSurfaceTest.cpp']:
        shutil.copyfile(module / 'tests' / name, temp / name)
    for name in ['IlluminationBufferCache.h', 'IlluminationRaster.h']:
        shutil.copyfile(module / 'jni' / name, temp / name)

    def compile_and_run(text, name, sanitized):
        production_copy = temp / 'IlluminationSurface.cpp'
        production_copy.write_text(text)
        if sanitized:
            assert production_copy.read_bytes() == (module / 'jni/IlluminationSurface.cpp').read_bytes()
        binary = temp / name
        flags = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitized else []
        subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                        '-pthread', *flags, '-I', str(temp),
                        str(temp / 'NativeCacheSurfaceTest.cpp'), '-o', str(binary)], check=True)
        return subprocess.run([str(binary)], text=True, capture_output=True, timeout=20)

    result = compile_and_run(source, 'native_cache', True)
    print(result.stdout, end='')
    if result.returncode:
        raise RuntimeError(result.stderr or f'Native tests returned {result.returncode}')
    mutations = {
        'hit_skips_new_transaction':
            ('gBuffer = gBufferCache.find(key);',
             'gBuffer = gBufferCache.find(key); if (gBuffer != nullptr) return true;'),
        'cache_never_reused':
            ('gBuffer = gBufferCache.find(key);', 'gBuffer = {};'),
        'unlock_failure_published':
            ('if (gBuffer->unlock() != NO_ERROR)', 'if ((gBuffer->unlock(), false))'),
        'failed_hide_keeps_cache':
            ('if (!presented) {\n        gBufferCache.clear();', 'if (!presented) {'),
        'missing_latch_accepted':
            ('|| !pending->completed || !pending->latched\n', '|| !pending->completed\n'),
        'fence_error_accepted':
            ('if (waitStatus == NO_ERROR && current())', 'if (current())'),
        'dead_object_keeps_cache':
            ('if (status == DEAD_OBJECT) {\n            gClient.clear();\n            gBufferCache.clear();\n            cancelPreparation(true);\n        }',
             'if (status == DEAD_OBJECT) {\n            gClient.clear();\n        }'),
        'cancellation_before_submit_ignored':
            ('const auto deadline = Clock::now() + std::chrono::milliseconds(500);\n    if (!current())',
             'const auto deadline = Clock::now() + std::chrono::milliseconds(500);\n    if (false)'),
    }
    mutations.update({
        'producer_ignores_cancellation':
            ('return token != 0 && gPreparationEpoch.load(std::memory_order_relaxed) == token;',
             'return token != 0;'),
        'prepared_key_not_checked':
            ('if (gPreparedBuffer.buffer != nullptr && gPreparedBuffer.key == key) {',
             'if (gPreparedBuffer.buffer != nullptr && (static_cast<void>(key), true)) {'),
        'generation_keeps_producer_running':
            ('gGeneration.store(generation, std::memory_order_relaxed);\n    cancelPreparation();',
             'gGeneration.store(generation, std::memory_order_relaxed);'),
        'prepared_hit_skips_present':
            ('gBuffer = std::move(ready.buffer);', 'gBuffer = std::move(ready.buffer); return true;'),
        'bad_preparation_unlock_published':
            ('if (unlocked != NO_ERROR) return finish("buffer_unlock_failed");',
             '(void)unlocked;'),
        'failed_hide_keeps_ready_buffer':
            ('if (!presented) {\n        gBufferCache.clear();\n        cancelPreparation(true);',
             'if (!presented) {\n        gBufferCache.clear();'),
    })
    negatives = []
    for name, (old, new) in mutations.items():
        assert source.count(old) == 1, (name, source.count(old))
        failed = compile_and_run(source.replace(old, new), name, False)
        assert failed.returncode != 0, f'Negative control survived: {name}'
        negatives.append({'mutation': name, 'exit_code': failed.returncode,
                          'failure': failed.stderr.strip()})
    print(f'PASS native cache negative controls={len(negatives)}')

inputs = [module / 'jni' / name for name in
          ['IlluminationSurface.cpp', 'IlluminationBufferCache.h', 'IlluminationRaster.h']]
inputs += [module / 'tests' / name for name in
           ['run_native_cache_tests.py', 'NativeCacheSurfaceStubs.h', 'NativeCacheSurfaceTest.cpp']]
report = {
    'status': 'PASS',
    'host_result': result.stdout.strip(),
    'source_hashes': {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
                      for p in inputs},
    'method': 'Compile a byte-identical copy of actual IlluminationSurface.cpp and its real cache/raster headers. Substitute only external Android/JNI services; execute with ASan/UBSan.',
    'negative_controls': negatives,
    'checks': [
        'Miss allocates, locks, verifies marker, rasterizes actual pixels and unlocks exactly once',
        'Hit preserves same buffer/pixels but creates a fresh surface, buffer transaction, completed callback, latch and present-fence wait',
        'Two-entry reuse/eviction leaves buffers still retained by a simulated compositor immutable',
        'Allocation, missing marker, mapper, lock, null address and unlock failures never publish bad preparation',
        'Hits reject wrong/unlatched surfaces, invalid/missing fences and fence wait errors',
        'Failed hide submit clears cache and preserves active surface/buffer for retry',
        'Unpresented hide and SurfaceFlinger death invalidate cache; reconnect prepares fresh pixels',
        'Cancellation before preparation, after unlock, after surface creation, during callback and fence wait cannot acknowledge readiness',
        'Connection/surface/invalid-input failures do not submit illumination',
        'Dedicated producer only allocates/marks/rasterizes/unlocks; no compositor client, surface, transaction or worker cache access',
        'Exact complete prepared key is adopted with unchanged fresh transaction/latch/fence requirements; wrong keys fall back immediately',
        'Cancellation at lock/unlock barriers across actual threads prevents partial or stale publication and always unlocks successful locks',
        'Completed pixels survive ordinary epoch/generation cancellation; failed hide and DEAD_OBJECT clear them and revoke in-flight work',
        'Replaced/cleared GraphicBuffers are destroyed outside the publication mutex, checked by a separate lock-probe thread',
        'Actual cancellable raster stops at every checkpoint including sensor rows, preserves stride padding and exact complete pixels',
    ],
    'limits': [
        'Android services and callbacks are deterministic host stubs, not real Binder, gralloc, composer or display hardware.',
        'The real native decision paths and raster execute, but optical behavior and latency gains require device measurements.',
        'Tests exercise callback ordering and cancellation without claiming exhaustive concurrent scheduler interleavings.',
    ],
}
if args.report:
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Saved {args.report}')
