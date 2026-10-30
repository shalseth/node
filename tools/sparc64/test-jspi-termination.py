#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Repeat unchanged JSPI worker-termination cases, including a full stack pool."""
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary_hash = hashlib.sha256((root / 'out/Release/d8').read_bytes()).hexdigest()
output = root / 'out/sparc64-jspi-termination'
output.mkdir(parents=True, exist_ok=True)
results = []
for index in range(20):
    label = f'termination-{index:02d}'
    command = ['python3', '-u', 'tools/sparc64/test-jspi-upstream.py',
               '--case', 'jspi', '--case', 'regress/wasm/regress-361717714',
               '--label', label]
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=150)
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (output / (label + '.stdout')).write_bytes(stdout)
    (output / (label + '.stderr')).write_bytes(stderr)
    evidence_path = root / ('out/sparc64-jspi-' + label + '.json')
    evidence = json.loads(evidence_path.read_text()) if evidence_path.exists() else None
    passed = code == 0 and evidence is not None and evidence['passed']
    if passed:
        assert evidence['binary_sha256'] == binary_hash and len(evidence['cases']) == 2
    item = dict(label=label, command=command, exit_code=code, passed=passed)
    results.append(item)
    print(json.dumps(item), flush=True)
    if not passed:
        break
assert hashlib.sha256((root / 'out/Release/d8').read_bytes()).hexdigest() == binary_hash
result = dict(passed=len(results) == 20 and all(item['passed'] for item in results),
              binary_sha256=binary_hash, rounds=20, cases_per_round=2, results=results)
(root / 'out/sparc64-jspi-termination.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
