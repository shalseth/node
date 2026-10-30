#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Prove a native match actually grows the backtracking stack."""

import hashlib
import json
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[2]
binary = root / 'out/Release/d8'
digest = hashlib.sha256(binary.read_bytes()).hexdigest()
command = ['gdb', '-q', '-batch', '-x', 'tools/sparc64/test-regexp-stack.gdb',
           '--args', str(binary), '--allow-natives-syntax', '--no-regexp-tier-up',
           'tools/sparc64/test-regexp-stack.js']
result = subprocess.run(command, cwd=root, stdout=subprocess.PIPE,
                        stderr=subprocess.STDOUT, text=True, timeout=90)
(root / 'out/sparc64-regexp-stack-growth.log').write_text(result.stdout)
match = re.search(r'REGEXP_STACK_GROWTH_CALLS=(\d+)', result.stdout)
calls = int(match[1]) if match else 0
passed = (result.returncode == 0 and calls > 0 and
          'REGEXP_STACK_GROWTH_OK' in result.stdout and
          'REGEXP_STACK_PROCESS_EXIT=0' in result.stdout)
evidence = dict(passed=passed, binary_sha256=digest, command=command,
                exit_code=result.returncode, growth_calls=calls)
(root / 'out/sparc64-regexp-stack-growth.json').write_text(json.dumps(evidence, indent=2) + '\n')
assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
print(json.dumps(evidence), flush=True)
raise SystemExit(not passed)
