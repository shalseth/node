#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate the heap-verification build and record reproducible runtime evidence."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=['all', 'stress', 'core', 'applications', 'directed'],
                    default='all')
args = parser.parse_args()
config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
if not config['has_turbofan'] or not config['verify_heap']:
    raise SystemExit('Build with configure-turbofan-stress.sh first')
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
node = str(root / 'out/Release/node')
d8 = str(root / 'out/Release/d8')


def hashes():
    return {name: hashlib.file_digest((root / 'out/Release' / name).open('rb'),
                                     'sha256').hexdigest() for name in ['node', 'd8']}


def run(name, command, marker, timeout=60, cwd=root):
    log = root / 'out' / f'sparc64-turbofan-runtime-{name}.log'
    started = time.monotonic()
    with log.open('w') as stream:
        try:
            code = subprocess.run(command, cwd=cwd, stdout=stream,
                                  stderr=subprocess.STDOUT, timeout=timeout).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    passed = code == 0 and marker in log.read_text()
    result = dict(name=name, command=command, cwd=str(cwd), exit=code,
                  marker=marker, passed=passed,
                  seconds=round(time.monotonic() - started, 2))
    print(json.dumps(result), flush=True)
    return result


def group(name):
    before = hashes()
    results = []
    if name == 'stress':
        results.append(run('stress', ['python3', '-u', 'tools/sparc64/test-turbofan-stress.py'],
                           '"passed": true', timeout=2500))
    elif name == 'core':
        results.append(run('core', ['python3', '-u', 'tools/sparc64/test-node-core.py'],
                           '"passed": true', timeout=930))
    elif name == 'applications':
        results.append(run('apps', ['sh', 'tools/sparc64/test-apps.sh'],
                           'APPLICATION_SMOKE_OK', timeout=200))
        results.append(run('npm', [node, 'tools/sparc64/test-npm-workflow.cjs'],
                           'NPM_WORKFLOW_OK', timeout=100))
        apps = root / 'out/sparc64-app-smoke'
        command = ['env', f'SPARC64_SMOKE_CERT_DIR={apps}/certs', node,
                   '--allow-natives-syntax', '--expose-gc', '--verify-heap',
                   '--compact-on-every-full-gc', '--max-old-space-size=128',
                   '--trace-opt', '--trace-deopt', 'turbofan-soak.cjs']
        results.append(run('soak', command, 'TURBOFAN_APPLICATION_SOAK_OK',
                           timeout=250, cwd=apps))
        if results[-1]['passed']:
            (root / 'out/sparc64-turbofan-soak.json').write_bytes(
                (apps / 'turbofan-soak-result.json').read_bytes())
    else:
        for name_, flags in [
            ('gc-compaction', ['--compact-on-every-full-gc']),
            ('gc-incremental', ['--stress-incremental-marking']),
            ('gc-concurrent', ['--stress-concurrent-allocation']),
        ]:
            results.append(run(name_, [d8, '--allow-natives-syntax', '--expose-gc',
                '--verify-heap', '--trace-opt', '--trace-deopt', *flags,
                'tools/sparc64/test-turbofan-gc.js'], 'PASS optimized GC/deopt', timeout=90))
        results.append(run('node-tier', [node, '--allow-natives-syntax', '--expose-gc',
            '--verify-heap', '--trace-opt', '--trace-deopt',
            'tools/sparc64/test-node-turbofan.cjs'], 'PASS automatic TurboFan', timeout=40))
        results.append(run('serialization', [node, '--verify-heap',
            'tools/sparc64/test-serialization.cjs'], 'PASS BigInt serialization'))
        results.append(run('addon', ['sh', 'tools/sparc64/test-addon-gc.sh'], 'ADDON_GC_OK'))
        results.append(run('addon-optimized', [node, '--allow-natives-syntax', '--expose-gc',
            '--verify-heap', '--compact-on-every-full-gc', '--trace-opt', '--trace-deopt',
            'tools/sparc64/test-addon-turbofan.cjs', 'out/sparc64-addon-gc/addon-gc.node'],
            'PASS optimized native addon', timeout=90))
        results.append(run('osr', [d8, '--allow-natives-syntax', '--verify-heap',
            '--trace-opt', '--trace-osr', '--no-concurrent-recompilation',
            'tools/sparc64/test-turbofan-osr.js'], 'INTERPRETER TO TURBOFAN OSR PASS'))
        results.append(run('baseline', ['sh', 'tools/sparc64/test-sparkplug.sh'],
                           'PASS Sparkplug:', timeout=90))
    after = hashes()
    result = dict(group=name, passed=before == after and all(r['passed'] for r in results),
                  binary_sha256=before, config=config, results=results)
    (root / 'out' / f'sparc64-turbofan-runtime-{name}.json').write_text(
        json.dumps(result, indent=2) + '\n')
    return result['passed']


if args.group == 'all':
    with ThreadPoolExecutor(max_workers=4) as executor:
        passed = list(executor.map(group, ['stress', 'core', 'applications', 'directed']))
else:
    passed = [group(args.group)]
raise SystemExit(not all(passed))
