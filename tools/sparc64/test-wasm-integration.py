#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate the real llhttp Wasm parser, fetch and builtin WebSocket."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=['all', 'parser', 'apis', 'upstream'], default='all')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
output = root / 'out/sparc64-wasm-integration'
output.mkdir(parents=True, exist_ok=True)
evidence = dict(passed=False, binary_sha256=digest(node), steps=[])
groups = ['parser', 'apis', 'upstream'] if args.group == 'all' else [args.group]
for name in groups:
    if name == 'upstream':
        tests = ['parallel/test-fetch', 'parallel/test-fetch-mock',
                 'parallel/test-process-exit-after-fetch-throw',
                 'parallel/test-http-import-websocket', 'parallel/test-websocket',
                 'parallel/test-inspector-network-fetch',
                 'parallel/test-inspector-network-websocket']
        command = ['python3', '-u', 'tools/test.py', '--shell=' + str(node),
                   '--arch=sparc64', '-j2', '-t60', '-p', 'tap', *tests]
        timeout = 180
    else:
        fixture = root / 'tools/sparc64' / ('test-wasm-undici.cjs' if name == 'parser'
                                           else 'test-wasm-node-apis.cjs')
        command = [str(node), '--allow-natives-syntax', '--liftoff-only',
                   '--no-wasm-lazy-compilation', str(fixture)]
        timeout = 60
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=timeout)
        stdout, stderr, code = result.stdout, result.stderr, result.returncode
        data = {}
        if code == 0 and name != 'upstream':
            data = json.loads(stdout.splitlines()[-1])
            assert data.get('passed'), data
            if name == 'parser':
                assert data['nativeFunctions'] == 2 and data['checks'] >= 190, data
            else:
                assert data['fetch'] and data['websocket'] and data['echoes'] == 3, data
        if name == 'upstream':
            cases = re.findall(rb'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', stdout, re.M)
            data = dict(tests=len(cases), failures=[c[2].decode() for c in cases if c[0] == b'not ok'])
            assert code != 0 or (len(cases) >= len(tests) and not data['failures']), data
        step = dict(name=name, command=command, exit_code=code, data=data,
                    seconds=round(time.monotonic() - started, 3))
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or b'', error.stderr or b''
        step = dict(name=name, command=command, timeout=True, seconds=timeout)
    (output / f'{name}.stdout').write_bytes(stdout)
    (output / f'{name}.stderr').write_bytes(stderr)
    evidence['steps'].append(step)
    output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(step), flush=True)
    assert step.get('exit_code') == 0, step
assert digest(node) == evidence['binary_sha256']
evidence['passed'] = True
output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('WASM_INTEGRATION_OK', flush=True)
