#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Cross-compile bundled xxHash with a Wasm atomic work queue and native oracle."""

import ctypes
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess

root = Path(__file__).resolve().parents[2]
output = root / 'out/wasm-atomics-workload'
output.mkdir(parents=True, exist_ok=True)
clang = os.environ.get('CLANG', 'clang')
linker = os.environ.get('WASM_LD', 'wasm-ld')
cc = os.environ.get('CC', 'cc')
source = 'tools/sparc64/wasm-threaded-hash.c'
command = [clang, '--target=wasm32', '-ffreestanding', '-nostdlib', '-O2',
           '-matomics', '-mbulk-memory', '-pthread', '-I.',
           '-Itools/sparc64/wasm-libc', '-fuse-ld=' + linker,
           '-Wl,--no-entry', '-Wl,--import-memory', '-Wl,--shared-memory',
           '-Wl,--max-memory=67108864', '-Wl,--initial-memory=131072',
           *['-Wl,--export=' + name for name in [
               'hash_block', 'hash_queue', 'add_lanes', 'tear64', 'store_buffering', 'load32', 'store32', 'poll_growth', 'wait32', 'wait64', 'notify', '__stack_pointer']],
           source, 'tools/sparc64/wasm-libc.c', '-o', str(output / 'threaded-hash.wasm')]
subprocess.run(command, cwd=root, check=True)
native_command = [cc, '-O2', '-shared', '-fPIC', '-I.', source,
                  '-o', str(output / 'native-hash.so')]
subprocess.run(native_command, cwd=root, check=True)
native = ctypes.CDLL(str(output / 'native-hash.so'))
native.hash_block.argtypes = [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_uint64]
native.hash_block.restype = ctypes.c_uint64
count, block_size, seed = 1024, 16384, 0x0123456789abcdef
# Include full-byte-range, unaligned and binary data in the independent oracle.
pattern = bytes((i * 37 + (i >> 3) * 11) & 255 for i in range(65536))
data = pattern * (count * block_size // len(pattern))
(output / 'input.bin').write_bytes(data)
buffer = ctypes.create_string_buffer(data)
golden = bytearray()
for index in range(count):
    value = native.hash_block(ctypes.addressof(buffer) + index * block_size,
                              block_size, seed + index)
    golden.extend(struct.pack('<Q', value))
(output / 'expected.bin').write_bytes(golden)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
sources = [source, 'tools/sparc64/wasm-libc.c', 'tools/sparc64/wasm-libc/string.h',
           'deps/zstd/lib/common/xxhash.h']
evidence = dict(command=command, native_command=native_command,
                clang_version=subprocess.check_output([clang, '--version'], text=True),
                linker_version=subprocess.check_output([linker, '--version'], text=True),
                cc_version=subprocess.check_output([cc, '--version'], text=True),
                clang_sha256=digest(Path(shutil.which(clang) or clang)),
                linker_sha256=digest(Path(shutil.which(linker) or linker)),
                source_sha256={name: digest(root / name) for name in sources},
                files={name: digest(output / name) for name in [
                    'threaded-hash.wasm', 'native-hash.so', 'input.bin', 'expected.bin']},
                count=count, block_size=block_size, seed=str(seed))
(output / 'manifest.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('WASM_THREADED_WORKLOAD_BUILD_OK', flush=True)
