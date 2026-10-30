#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Exercise GC, nested matching and termination inside native regexp code."""

import hashlib
import json
import os
from pathlib import Path
import resource
import shlex
import subprocess
import time

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
output = root / 'out/sparc64-regexp-interrupt.node'
digest = hashlib.sha256(node.read_bytes()).hexdigest()
commands = [
    [*shlex.split(os.environ.get('CXX', 'g++')), '-std=gnu++20', '-O2',
     '-mcpu=v9', '-fPIC', '-shared', '-pthread', '-I' + str(root / 'src'),
     '-I' + str(root / 'deps/v8/include'),
     '-DNODE_GYP_MODULE_NAME=sparc64_regexp_interrupt',
     'tools/sparc64/addon-regexp-interrupt.cc', '-o', str(output)],
    [str(node), '--allow-natives-syntax', '--expose-gc', '--verify-heap',
     '--compact-on-every-full-gc', '--stress-compaction',
     '--no-enable-experimental-regexp-engine-on-excessive-backtracks',
     'tools/sparc64/test-regexp-interrupt.cjs'],
]
commands.append([*commands[1][:-1], '--no-node-snapshot', '--no-regexp-tier-up', commands[1][-1]])
results = []
for index, command in enumerate(commands):
    start = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, timeout=60)
        code, log = result.returncode, result.stdout
    except subprocess.TimeoutExpired as error:
        code, log = 'timeout', error.stdout or b''
    (root / 'out' / f'sparc64-regexp-interrupt-{index}.log').write_bytes(log)
    passed = code == 0 and (index == 0 or b'REGEXP_INTERRUPT_GC_NESTING_OK' in log)
    results.append(dict(command=command, exit_code=code, passed=passed,
                        seconds=round(time.monotonic() - start, 3)))
    print(json.dumps(results[-1]), flush=True)
    evidence = dict(binary_sha256=digest, results=results,
                    passed=len(results) == len(commands) and all(item['passed'] for item in results))
    (root / 'out/sparc64-regexp-interrupt.json').write_text(json.dumps(evidence, indent=2) + '\n')
    if not passed:
        print(log.decode(errors='replace'), flush=True)
        raise SystemExit(1)
assert hashlib.sha256(node.read_bytes()).hexdigest() == digest
print('REGEXP_INTERRUPT_OK', flush=True)
