#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run Node's buffer, stream, network, TLS, worker and child-process tests."""

import json
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
excluded = {
    'test-worker-message-port-wasm-threads': 'Requires disabled WebAssembly',
    'test-worker-message-port-wasm-module': 'Requires disabled WebAssembly',
    'test-tls-set-default-ca-certificates-append-fetch':
        'Undici HTTP/1 parser requires disabled WebAssembly',
    'test-tls-set-default-ca-certificates-reset-fetch':
        'Undici HTTP/1 parser requires disabled WebAssembly',
}
command = ['python3', '-u', 'tools/test.py',
           f'--shell={root}/out/Release/node', '-j16', '-t30', '--arch=sparc64',
           '-p', 'tap', '--skip-tests=' + ','.join(excluded),
           *('parallel/test-' + name + '*' for name in
             ['buffer', 'stream', 'net', 'tls', 'worker', 'child-process'])]
log = root / 'out/sparc64-turbofan-node-core.tap'
started = time.monotonic()
with log.open('w') as stream:
    try:
        code = subprocess.run(command, cwd=root, stdout=stream,
                              stderr=subprocess.STDOUT, timeout=900).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
cases = re.findall(r'^(not ok|ok) (\d+) (parallel/[^\s]+)([^\n]*)',
                   log.read_text(), re.M)
failures = [case[2] for case in cases if case[0] == 'not ok']
skipped = [case[2] for case in cases if '# SKIP' in case[3].upper()]
passed = code == 0 and len(cases) >= 990 and not failures
result = dict(passed=passed, exit=code, tests=len(cases), failures=failures,
              upstream_skipped=skipped, excluded=excluded, command=command,
              seconds=round(time.monotonic() - started, 2))
(root / 'out/sparc64-turbofan-node-core.json').write_text(
    json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not passed)
