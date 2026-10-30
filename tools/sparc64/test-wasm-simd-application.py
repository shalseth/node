#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate native SIMD arithmetic and the actual SIMD llhttp application."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/node')
parser.add_argument('--tier', choices=['liftoff', 'optimized'], default='liftoff')
parser.add_argument('--label', default='current')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binary_hash = digest(args.binary)
output = root / ('out/sparc64-wasm-simd-application-' + args.label)
assert not output.exists(), 'Preserve prior evidence before retrying'
output.mkdir(parents=True)
results = []
for name, fixture in [('matrix', 'tools/sparc64/test-wasm-simd.cjs'),
                       ('undici', 'tools/sparc64/test-wasm-simd-undici.cjs')]:
    command = [str(args.binary), *(['--liftoff-only'] if args.tier == 'liftoff' else ['--no-liftoff-only', '--no-liftoff']),
               '--no-wasm-lazy-compilation', '--allow-natives-syntax', '--expose-gc',
               '--verify-heap', fixture]
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=120,
                             env=dict(os.environ, WASM_SIMD_TIER=args.tier))
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (output / (name + '.stdout')).write_bytes(stdout)
    (output / (name + '.stderr')).write_bytes(stderr)
    data = None
    if code == 0:
        lines = stdout.decode().splitlines()
        data = json.loads(lines[-1].removeprefix('WASM_SIMD_MATRIX_OK ')) if lines else None
    expected = (dict(passed=True, tier=args.tier, operations=59, nativeFunctions=59,
                    invocations=11328, checks=1812480, seed=131160559) if name == 'matrix' else
                dict(checks=1320, nativeFunctions=2, passed=True, simd=True, tier=args.tier,
                    moduleSha256='18403ee56fab07f89a845028bad358bb3ebcda213f8e2ec0989a991e9c33da69'))
    item = dict(name=name, passed=code == 0 and data == expected, command=command,
                exit_code=code, data=data, fixture=fixture, fixture_sha256=digest(root / fixture))
    results.append(item)
    print(json.dumps(item), flush=True)
assert digest(args.binary) == binary_hash
source = 'deps/undici/src/lib/llhttp/llhttp_simd-wasm.js'
assert (root / source).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
result = dict(passed=all(item['passed'] for item in results), binary_sha256=binary_hash,
              tier=args.tier, source=source, source_sha256=digest(root / source), results=results)
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
