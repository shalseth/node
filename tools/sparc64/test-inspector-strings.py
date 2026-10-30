#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Test aligned and unaligned inspector UTF-16 and real protocol round trips."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import signal
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--native-only', action='store_true')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
results = []
output = root / 'out/sparc64-inspector-strings'
output.mkdir(exist_ok=True)
for name, source, expected in [
    ('fixed', root / 'deps/v8/src/inspector/string-16.cc', True),
    ('parent', root.with_name('node-sparc64-regexp') / 'deps/v8/src/inspector/string-16.cc', False),
]:
    binary = output / name
    command = ['g++', '-std=gnu++20', '-O2', '-mcpu=v9', '-DNDEBUG',
               '-DV8_TARGET_ARCH_SPARC64', '-ffunction-sections', '-fdata-sections',
               '-I' + str(root / 'deps/v8'), '-I' + str(root / 'deps/v8/include'),
               '-I' + str(root / 'deps/v8/third_party/abseil-cpp'),
               'tools/sparc64/test-inspector-strings.cc', str(source),
               '-Wl,--gc-sections', '-o', str(binary)]
    build = subprocess.run(command, cwd=root, capture_output=True, timeout=60)
    (output / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
    assert build.returncode == 0, build.stderr.decode(errors='replace')
    result = subprocess.run([str(binary)], cwd=root, capture_output=True, timeout=15)
    (output / (name + '.log')).write_bytes(result.stdout + result.stderr)
    passed = (result.returncode == 0 and b'INSPECTOR_UNALIGNED_UTF16_OK' in result.stdout)
    if not expected:
        assert result.returncode == -signal.SIGBUS, result.returncode
    assert passed == expected, (name, result.returncode, result.stdout, result.stderr)
    results.append(dict(name=name, command=command, exit_code=result.returncode,
                        expected_success=expected, passed=True))
if not args.native_only:
    node = root / 'out/Release/node'
    command = [str(node), '--verify-heap', 'tools/sparc64/test-inspector-strings.cjs']
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=30)
    (output / 'protocol.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0 and b'INSPECTOR_UTF16_ROUNDTRIP_OK 256' in result.stdout
    results.append(dict(name='protocol', command=command, exit_code=0, passed=True,
                        binary_sha256=hashlib.sha256(node.read_bytes()).hexdigest()))
(root / 'out/sparc64-hardening-inspector-strings.json').write_text(json.dumps(results, indent=2) + '\n')
print('INSPECTOR_STRING_TESTS_OK', json.dumps(results), flush=True)
