#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate JSPI suspension, rejection, nesting and live Wasm references under GC."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', choices=['basic', 'compaction', 'incremental', 'all'], default='all')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
fixture = 'tools/sparc64/test-jspi-runtime.cjs'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binary_hash, fixture_hash = digest(node), digest(root / fixture)
profiles = {'basic': [], 'compaction': ['--compact-on-every-full-gc'],
            'incremental': ['--stress-incremental-marking']}
output = root / 'out/sparc64-jspi-runtime'
output.mkdir(parents=True, exist_ok=True)
results = []
for name, flags in profiles.items():
    if args.profile not in ['all', name]:
        continue
    command = [str(node), '--liftoff-only', '--no-wasm-lazy-compilation',
               '--allow-natives-syntax', '--expose-gc', '--verify-heap', *flags, fixture]
    start = time.monotonic()
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=120)
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (output / (name + '.stdout')).write_bytes(stdout)
    (output / (name + '.stderr')).write_bytes(stderr)
    markers = [line.removeprefix('JSPI_RUNTIME_OK ') for line in stdout.decode(errors='replace').splitlines()
               if line.startswith('JSPI_RUNTIME_OK ')]
    data = json.loads(markers[0]) if len(markers) == 1 else None
    expected = dict(passed=True, checks=151, suspensions=362, nativeFunctions=13,
                    rejections=16, repeatedSuspensions=128, concurrentStacks=8,
                    nestedDepth=8, stackReuse=64)
    item = dict(name=name, command=command, exit_code=code, data=data,
                passed=code == 0 and data == expected, seconds=round(time.monotonic()-start, 3))
    results.append(item)
    print(json.dumps(item), flush=True)
assert digest(node) == binary_hash and digest(root / fixture) == fixture_hash
result = dict(passed=all(item['passed'] for item in results), binary_sha256=binary_hash,
              fixture=fixture, fixture_sha256=fixture_hash, profile=args.profile, results=results)
(root / ('out/sparc64-jspi-runtime-' + args.profile + '.json')).write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
