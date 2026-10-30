#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Prove optimized JS reaches the native FFI trampoline, including stack args."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
prefix = root / ('out/sparc64-node-features-ffi-fast-native-' + args.label)
prefix.mkdir(exist_ok=False)
graphs = prefix / 'graphs'
graphs.mkdir()
sources = ['tools/sparc64/ffi-call-oracle.c', 'tools/sparc64/test-ffi-fast-native.cjs']
source_hashes = {name: digest(root / name) for name in sources}
for name in sources:
    destination = prefix / 'source' / name
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(root / name, destination)
library = prefix / 'oracle.so'
compile_command = ['gcc', '-O2', '-fPIC', '-shared', str(root / sources[0]),
                   '-ldl', '-o', str(library)]
with (prefix / 'build.log').open('w') as log:
    assert subprocess.run(compile_command, cwd=root, stdout=log,
                          stderr=subprocess.STDOUT).returncode == 0
binary = root / 'out/Release/node'
binary_hash = digest(binary)
command = [str(binary), '--allow-natives-syntax', '--expose-internals', '--expose-gc', '--verify-heap',
           '--trace-turbo', '--trace-turbo-filter=*',
           '--trace-turbo-path=' + str(graphs), str(root / sources[1])]
env = dict(os.environ, SPARC64_FFI_ORACLE=str(library))
with (prefix / 'stdout').open('w') as stdout, (prefix / 'stderr').open('w') as stderr:
    try:
        code = subprocess.run(command, cwd=prefix, env=env, stdout=stdout,
                              stderr=stderr, timeout=120).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
output = (prefix / 'stdout').read_text()
proof = [str(path.relative_to(root)) for path in graphs.glob('*.json')
         if 'FastApiCall' in path.read_text()]
assert digest(binary) == binary_hash
assert source_hashes == {name: digest(root / name) for name in sources}
has_both_graphs = any('turbo-callEight-' in path for path in proof) and len(proof) >= 2
report = dict(passed=code == 0 and has_both_graphs and 'FFI_FAST_NATIVE_OK ' in output,
              command=command, exit_code=code, binary_sha256=binary_hash,
              source_sha256=source_hashes, library_sha256=digest(library),
              compile_command=compile_command, fast_api_graphs=proof,
              graph_sha256={name: digest(root / name) for name in proof})
prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
raise SystemExit(not report['passed'])
