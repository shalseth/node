#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run atomic acceptance and the preserved Wasm/Node/JIT regression gates."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--phase', choices=['prep', 'atomics', 'v8', 'node', 'conformance',
                                       'applications', 'all'], default='all')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
prefix = root / 'out/sparc64-wasm-atomics-validation'
records = []


def run(name, command, timeout=1200):
    started = time.monotonic()
    log = Path(str(prefix) + '-' + name + '.log')
    with log.open('w') as stream:
        try:
            result = subprocess.run(command, cwd=root, stdout=stream,
                                    stderr=subprocess.STDOUT, timeout=timeout)
            code = result.returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    record = dict(name=name, command=command, exit_code=code,
                  seconds=round(time.monotonic() - started, 3), log=str(log))
    print(json.dumps(record), flush=True)
    return record


def python(name, script, *arguments, timeout=1200):
    return run(name, ['python3', '-u', 'tools/sparc64/' + script + '.py', *arguments], timeout)


phases = ['prep', 'atomics', 'v8', 'node', 'conformance', 'applications'] if args.phase == 'all' else [args.phase]
for phase in phases:
    print('BEGIN_PHASE', phase, 'pid', os.getpid(), flush=True)
    if phase == 'prep':
        tasks = [python('config', 'write-test-config', '--allow-turbofan', '--allow-webassembly'),
                 python('wasm-inputs', 'prepare-wasm-suites')]
        for group in ['node-api', 'addons', 'js-native-api', 'sqlite']:
            tasks.append(run('build-addons-' + group, ['python3', '-u', 'tools/build_addons.py',
                'test/' + group, '--out-dir', 'out/Release', '--loglevel=error'], 900))
    elif phase == 'atomics':
        tasks = [python('atomics', 'test-wasm-atomics', timeout=900)]
    elif phase == 'v8':
        selections = [('baseline', 'test-wasm-baseline'), ('scalar-spec', 'test-wasm-upstream'),
                      ('jsapi', 'test-wasm-jsapi'), ('javascript', 'test-wasm-js-regression'),
                      ('regexp', 'test-regexp-upstream')]
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks = list(pool.map(lambda pair: python(*pair, timeout=1500), selections))
    elif phase == 'node':
        selections = [('integration', 'test-wasm-integration'), ('modules', 'test-wasm-node-modules'),
                      ('platform-wasm', 'test-wasm-platform'), ('platform', 'test-platform-assumptions'),
                      ('zip', 'test-zip-interop'), ('workloads', 'test-workloads'),
                      ('directed', 'test-turbofan-runtime', '--group', 'directed'),
                      ('cpp', 'test-cctest', '--profile', 'wasm')]
        with ThreadPoolExecutor(max_workers=4) as pool:
            tasks = list(pool.map(lambda selection: python(*selection, timeout=1200), selections))
        tasks.append(python('snapshot-state', 'test-snapshot-state'))
        tasks.append(python('snapshot-flags', 'test-snapshot-regexp-flags'))
        if (root / 'tools/sparc64/test-snapshot-array-buffer-keys.py').exists():
            tasks.append(python('snapshot-array-buffer-keys', 'test-snapshot-array-buffer-keys'))
    elif phase == 'conformance':
        tasks = [python('conformance-' + group, 'test-conformance', '--profile', 'wasm',
                         '--group', group, timeout=2600) for group in ['broad', 'addons', 'native']]
    else:
        tasks = [python('applications', 'test-turbofan-runtime', '--group', 'applications', timeout=900)]
    assert {name: digest(root / 'out/Release' / name) for name in current} == current
    records.extend(tasks)
    evidence = dict(passed=all(item['exit_code'] == 0 for item in records), pid=os.getpid(),
                    phase=phase, binary_sha256=current, results=records)
    Path(str(prefix) + '-' + args.phase + '.json').write_text(json.dumps(evidence, indent=2) + '\n')
    assert all(item['exit_code'] == 0 for item in tasks), 'See validation phase logs: ' + phase
    print('END_PHASE', phase, flush=True)
print('WASM_ATOMICS_VALIDATION_OK', flush=True)
