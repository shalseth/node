#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Check ordinary Wasm module cases independently of the JSPI feature exclusion."""

import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
names = ['es-module/test-wasm-simple', 'es-module/test-wasm-web-api',
         'es-module/test-wasm-memory-out-of-bound']
command = ['python3', '-u', 'tools/test.py', '--shell=' + str(node), '--arch=sparc64',
           '-j2', '-t60', '-p', 'tap', *names]
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
before = digest(node)
prefix = root / 'out/sparc64-wasm-node-modules'
with prefix.with_suffix('.tap').open('w') as log:
    code = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT,
                          timeout=180).returncode
cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', prefix.with_suffix('.tap').read_text(), re.M)
assert digest(node) == before
passed = code == 0 and len(cases) == len(names) and all(c[0] == 'ok' and '# SKIP' not in c[3].upper() for c in cases)
assert {c[2] for c in cases} == set(names)
item = dict(passed=passed, binary_sha256=before, command=command, exit_code=code,
            tests=len(cases), names=names,
            source_sha256={name: digest(root / 'test' / (name + '.js')) for name in names})
prefix.with_suffix('.json').write_text(json.dumps(item, indent=2) + '\n')
print(json.dumps(item), flush=True)
assert passed
print('WASM_NODE_MODULES_OK', flush=True)
