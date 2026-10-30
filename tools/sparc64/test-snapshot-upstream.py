#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run upstream startup snapshot, builtin cache and module cache validation."""

import ast
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert variables['node_use_node_snapshot'] == 'true'
assert variables['node_use_node_code_cache'] == 'true'
with (root / 'out/Release/node').open('rb') as stream:
    binary_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
selections = [
    'parallel/test-snapshot-*', 'parallel/test-code-cache',
    'parallel/test-vm-cached-data', 'parallel/test-vm-module-cached-data',
    'parallel/test-vm-createcacheddata', 'parallel/test-v8-startup-snapshot-api',
    'parallel/test-compile-cache-*', 'parallel/test-no-node-snapshot',
    'parallel/test-async-local-storage-snapshot',
    'parallel/test-crypto-random-cache-snapshot',
]
profiles = [
    ('main', selections, []),
    ('worker', ['parallel/test-code-cache', 'parallel/test-vm-cached-data',
                'parallel/test-vm-module-cached-data'], ['--worker']),
]
results = []
for name, tests, options in profiles:
    command = ['python3', '-u', 'tools/test.py',
               f'--shell={root}/out/Release/node', '--arch=sparc64',
               f'--temp-dir={root}/out/sparc64-snapshot-test-tmp',
               '-j8', '-t60', '-p', 'tap', *options, *tests]
    path = root / 'out' / f'sparc64-snapshots-upstream-{name}.tap'
    started = time.monotonic()
    with path.open('w') as output:
        try:
            code = subprocess.run(command, cwd=root, stdout=output,
                                  stderr=subprocess.STDOUT, timeout=600).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    cases = re.findall(r'^(not ok|ok) (\d+) (parallel/[^\s]+)([^\n]*)',
                       path.read_text(), re.M)
    failed = [case[2] for case in cases if case[0] == 'not ok']
    skipped = [case[2] for case in cases if '# SKIP' in case[3].upper()]
    passed = code == 0 and len(cases) >= (50 if name == 'main' else 3) and not failed
    with (root / 'out/Release/node').open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == binary_hash
    results.append(dict(profile=name, passed=passed, exit=code, tests=len(cases),
                        binary_sha256=binary_hash,
                        failures=failed, upstream_skipped=skipped, command=command,
                        seconds=round(time.monotonic() - started, 2)))
    print(json.dumps(results[-1]), flush=True)
(root / 'out/sparc64-snapshots-upstream.json').write_text(
    json.dumps(results, indent=2) + '\n')
raise SystemExit(not all(item['passed'] for item in results))
