#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Recompute every threaded-workload hash using the target's native C compiler."""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess

root = Path(__file__).resolve().parents[2]
output = root / 'out/wasm-atomics-workload'
manifest = json.loads((output / 'manifest.json').read_text())
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
command = ['gcc', '-O2', '-shared', '-fPIC', '-I.',
           'tools/sparc64/wasm-threaded-hash.c', '-o', str(output / 'target-native-hash.so')]
subprocess.run(command, cwd=root, check=True, timeout=30)
native = ctypes.CDLL(str(output / 'target-native-hash.so'))
native.hash_block.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64]
native.hash_block.restype = ctypes.c_uint64
data = (output / 'input.bin').read_bytes()
assert hashlib.sha256(data).hexdigest() == manifest['files']['input.bin']
expected = (output / 'expected.bin').read_bytes()
assert hashlib.sha256(expected).hexdigest() == manifest['files']['expected.bin']
buffer = ctypes.create_string_buffer(data)
actual = bytearray()
for i in range(manifest['count']):
    value = native.hash_block(ctypes.addressof(buffer) + i * manifest['block_size'],
                              manifest['block_size'], int(manifest['seed']) + i)
    actual.extend(struct.pack('<Q', value))
assert actual == expected
(output / 'target-expected.bin').write_bytes(actual)
result = dict(passed=True, machine=os.uname().machine, checks=manifest['count'],
              command=command, compiler_version=subprocess.check_output(['gcc', '--version'], text=True),
              source_sha256=manifest['source_sha256'],
              native_sha256=digest(output / 'target-native-hash.so'),
              expected_sha256=digest(output / 'target-expected.bin'))
(output / 'target-oracle.json').write_text(json.dumps(result, indent=2) + '\n')
print('WASM_TARGET_ORACLE_OK', flush=True)
