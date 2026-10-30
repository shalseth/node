#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Bounded native Liftoff execution tests, including explicit compiler proof."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', choices=['all', 'scalar', 'integer', 'float', 'control', 'calls', 'tables', 'stack', 'reentry', 'memory'], default='all')
parser.add_argument('--binary', type=Path, default=root / 'out/Release/d8')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary = args.binary.resolve()
fixture = root / 'tools/sparc64/test-wasm-baseline.js'
output = root / 'out/sparc64-wasm-baseline'
output.mkdir(parents=True, exist_ok=True)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
evidence = dict(binary_sha256=digest(binary), fixture_sha256=digest(fixture), passed=False, cases=[])
profiles = ['scalar', 'integer', 'float', 'control', 'calls', 'tables', 'stack', 'reentry', 'memory'] if args.case == 'all' else [args.case]
for name in profiles:
    command = [str(binary), '--allow-natives-syntax', '--expose-gc', '--liftoff-only',
               '--no-wasm-lazy-compilation', str(fixture), '--', name]
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root / 'deps/v8', capture_output=True, timeout=30)
        (output / f'{name}.stdout').write_bytes(result.stdout)
        (output / f'{name}.stderr').write_bytes(result.stderr)
        data = json.loads(result.stdout.splitlines()[-1]) if result.returncode == 0 else {}
        item = dict(name=name, command=command, exit_code=result.returncode,
                    seconds=round(time.monotonic() - started, 3), data=data)
    except subprocess.TimeoutExpired as error:
        (output / f'{name}.stdout').write_bytes(error.stdout or b'')
        (output / f'{name}.stderr').write_bytes(error.stderr or b'')
        item = dict(name=name, command=command, timeout=True, seconds=30)
    evidence['cases'].append(item)
    (output.with_suffix('.json')).write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(item), flush=True)
    assert item.get('exit_code') == 0 and item['data'].get('nativeFunctions', 0) > 0, item
    assert item['data']['passed'] and item['data']['case'] == name, item
assert digest(binary) == evidence['binary_sha256']
evidence['passed'] = True
(output.with_suffix('.json')).write_text(json.dumps(evidence, indent=2) + '\n')
print('WASM_BASELINE_OK', flush=True)
