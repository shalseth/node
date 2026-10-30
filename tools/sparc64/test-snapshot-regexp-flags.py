#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Verify restored RegExp state under consumer tiering flags and heap checks."""

import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
digest = hashlib.sha256(node.read_bytes()).hexdigest()
results = []
for snapshot in [True, False]:
    for flags in [[], ['--no-regexp-tier-up'], ['--regexp-tier-up-ticks=0'],
                  ['--regexp-tier-up-ticks=5'], ['--regexp-interpret-all']]:
        index = len(results)
        command = [str(node), '--allow-natives-syntax', '--expose-gc', '--verify-heap',
                   '--compact-on-every-full-gc', *flags,
                   *([] if snapshot else ['--no-node-snapshot']),
                   'tools/sparc64/test-snapshot-regexp-flags.cjs']
        result = subprocess.run(command, cwd=root, capture_output=True, timeout=45)
        log = result.stdout + result.stderr
        (root / 'out' / f'sparc64-hardening-regexp-flags-{index}.log').write_bytes(log)
        passed = result.returncode == 0 and b'SNAPSHOT_REGEXP_FLAGS_OK' in log
        results.append(dict(command=command, snapshot=snapshot, exit_code=result.returncode,
                            passed=passed))
        print(json.dumps(results[-1]), flush=True)
        (root / 'out/sparc64-hardening-regexp-flags.json').write_text(json.dumps(
            dict(passed=all(item['passed'] for item in results) and len(results) == 10,
                 binary_sha256=digest, results=results), indent=2) + '\n')
        if not passed:
            print(log.decode(errors='replace'), flush=True)
            raise SystemExit(1)
assert hashlib.sha256(node.read_bytes()).hexdigest() == digest
print('SNAPSHOT_REGEXP_FLAG_MATRIX_OK', flush=True)
