#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Exercise SIMD/GC roots across concurrent JSPI suspension and tier transitions."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binary = root / 'out/Release/node'
fixture = 'tools/sparc64/test-wasm-production-mixed.cjs'
binary_hash, fixture_hash = digest(binary), digest(root / fixture)
output = root / ('out/sparc64-wasm-production-mixed-' + args.label)
assert not output.exists(), 'Preserve prior evidence before retrying'
output.mkdir(parents=True)
results = []
profiles = {'basic': [], 'compaction': ['--compact-on-every-full-gc'],
            'incremental': ['--stress-incremental-marking']}
expected = dict(passed=True, calls=34, checks=102, suspensions=102, vectors=20,
                concurrentStacks=4, liftoff=True, optimized=True, wasmGC=True)
for name, flags in profiles.items():
    command = [str(binary), '--no-wasm-lazy-compilation', '--allow-natives-syntax',
               '--expose-gc', '--verify-heap', *flags, fixture]
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=120)
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (output / (name + '.stdout')).write_bytes(stdout)
    (output / (name + '.stderr')).write_bytes(stderr)
    markers = [json.loads(line.removeprefix('WASM_PRODUCTION_MIXED_OK '))
               for line in stdout.decode(errors='replace').splitlines()
               if line.startswith('WASM_PRODUCTION_MIXED_OK ')]
    item = dict(name=name, command=command, exit_code=code,
                passed=code == 0 and markers == [expected], data=markers)
    results.append(item)
    print(json.dumps(item), flush=True)
assert digest(binary) == binary_hash and digest(root / fixture) == fixture_hash
result = dict(passed=all(item['passed'] for item in results),
              binary_sha256=binary_hash, fixture=fixture,
              fixture_sha256=fixture_hash, results=results)
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
