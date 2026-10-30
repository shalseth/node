#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run broad optimized Node conformance with explicit feature exclusions."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=['broad', 'addons', 'native'], default='broad')
parser.add_argument('--profile', choices=['hardening', 'wasm'], default='hardening')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = root / 'out/Release/node'
digest = hashlib.sha256(node.read_bytes()).hexdigest()
cores = {}
for cpu in sorted(os.sched_getaffinity(0)):
    topology = Path(f'/sys/devices/system/cpu/cpu{cpu}/topology/core_id')
    key = topology.read_text().strip()
    cores.setdefault(key, cpu)
affinity = list(cores.values())[:16]
os.sched_setaffinity(0, affinity)
excluded = json.loads((root / 'tools/sparc64/hardening-exclusions.json').read_text())
if args.profile == 'wasm':
    config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
    assert config['has_webassembly'] and config['has_turbofan']
    excluded = {name: reason for name, reason in excluded.items()
                if 'WebAssembly' not in reason}
    amaro = subprocess.check_output(
        [str(node), '-p', 'JSON.stringify(process.config.variables.node_use_amaro)'], cwd=root)
    if json.loads(amaro) is True:
        excluded = {name: reason for name, reason in excluded.items() if 'Amaro' not in reason}
    if args.group == 'broad':
        excluded.update(json.loads(
            (root / 'tools/sparc64/wasm-feature-exclusions.json').read_text()))
if args.group == 'broad':
    suites = ['parallel', 'sequential', 'async-hooks', 'es-module', 'module-hooks',
              'message', 'report', 'trace_events', 'test-runner', 'node-api']
elif args.group == 'addons':
    suites = ['addons', 'js-native-api']
else:
    suites = ['abort', 'pseudo-tty', 'sea', 'sqlite']
skip_patterns = ['parallel/test-wasm.mjs'
                 if args.profile == 'wasm' and name == 'parallel/test-wasm'
                 else name.rsplit('/', 1)[-1] for name in excluded]
command = ['python3', '-u', 'tools/test.py', '--shell=' + str(node), '--arch=sparc64',
           '-j8', '-t120', '-p', 'tap',
           '--skip-tests=' + ','.join(skip_patterns), *suites]
prefix = root / 'out' / ('sparc64-' + args.profile + '-conformance-' + args.group)
start = time.monotonic()
with prefix.with_suffix('.tap').open('w') as log:
    code = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=2400).returncode
text = prefix.with_suffix('.tap').read_text()
cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', text, re.M)
failures = [case[2] for case in cases if case[0] == 'not ok']
skipped = [case[2] for case in cases if '# SKIP' in case[3].upper()]
passed = code == 0 and bool(cases) and not failures
assert hashlib.sha256(node.read_bytes()).hexdigest() == digest
result = dict(passed=passed, binary_sha256=digest, command=command, exit_code=code,
              tests=len(cases), passes=len(cases)-len(skipped)-len(failures),
              upstream_skipped=skipped, failures=failures, excluded=excluded,
              affinity=affinity, seconds=round(time.monotonic()-start, 3))
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not passed)
