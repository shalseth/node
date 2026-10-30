#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run the unchanged upstream JSPI async-stack inspector test and expected output."""
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8', 'inspector-test']}
fixtures = ['deps/v8/test/inspector/debugger/wasm-jspi-async-stack.js',
            'deps/v8/test/inspector/debugger/wasm-jspi-async-stack-expected.txt',
            'deps/v8/test/inspector/wasm-inspector-test.js',
            'deps/v8/test/inspector/protocol-test.js',
            'deps/v8/test/inspector/inspector.status']
for name in fixtures:
    assert (root / name).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)
raw = root / 'out/sparc64-jspi-inspector-raw.json'
assert not raw.exists(), 'Preserve any prior inspector run before retrying'
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=' + str(root / 'out/Release'),
           '--variants=default', '--no-presubmit', '--run-skipped', '-j1', '-t120', '--progress=verbose',
           '--random-seed=42', '--rerun-failures-count=0',
           '--extra-flags=--liftoff-only --no-wasm-lazy-compilation',
           '--json-test-results=' + str(raw), 'inspector/debugger/wasm-jspi-async-stack']
try:
    run = subprocess.run(command, cwd=root / 'deps/v8', capture_output=True, timeout=180)
    code, stdout, stderr = run.returncode, run.stdout, run.stderr
except subprocess.TimeoutExpired as error:
    code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
(root / 'out/sparc64-jspi-inspector.stdout').write_bytes(stdout)
(root / 'out/sparc64-jspi-inspector.stderr').write_bytes(stderr)
data = json.loads(raw.read_text()) if raw.exists() else None
assert all(digest(root / 'out/Release' / name) == value for name, value in current.items())
result = dict(passed=code == 0 and data is not None and data['test_total'] == 1 and not data['results'],
              command=command, exit_code=code, binary_sha256=current,
              source_sha256={name: digest(root / name) for name in fixtures})
(root / 'out/sparc64-jspi-inspector.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not result['passed'])
