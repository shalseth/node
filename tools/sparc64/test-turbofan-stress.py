#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run bounded upstream GC, deoptimization and concurrent compiler coverage."""

import argparse
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--initial', action='store_true',
                    help='Run before the heap-verification build is available')
args = parser.parse_args()
config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
if not config['has_turbofan'] or (not args.initial and not config['verify_heap']):
    raise SystemExit('Requires TurboFan and, unless --initial, heap verification')
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
patterns = [
    'compiler/deopt-simple-*', 'compiler/deopt-followed-by-gc',
    'compiler/escape-analysis*', 'compiler/osr-try-catch',
    'compiler/fast-api-deopt', 'compiler/fast-api-calls-exception',
    'compiler/fast-api-calls-nested', 'compiler/fast-api-calls-8args',
    'compiler/regress-v8-6077', 'compiler/constructor-inlining*',
    'baseline/test-osr', 'regress/regress-deopt-gc',
    'regress/regress-deopt-gcb',
]
profiles = [
    ('compaction', 'default', '--compact-on-every-full-gc --stress-compaction'),
    ('incremental', 'stress_incremental_marking', ''),
    ('allocation', 'stress_concurrent_allocation', ''),
    ('inlining', 'stress_concurrent_inlining', ''),
]
results = []
phase = 'initial' if args.initial else 'verified'
for name, variant, flags in profiles:
    prefix = root / 'out' / f'sparc64-turbofan-stress-{phase}-{name}'
    if not args.initial:
        flags += ' --verify-heap'
    command = ['python3', '-u', 'tools/run-tests.py',
               '--outdir=../../out/Release', f'--variants={variant}',
               '--no-presubmit', '-j8', '-t30', '--progress=verbose',
               '--random-seed=42',
               '--exit-after-n-failures=10', '--rerun-failures-count=0',
               f'--extra-flags={flags.strip()}',
               f'--json-test-results={prefix}.json',
               *('mjsunit/' + pattern for pattern in patterns)]
    started = time.monotonic()
    with prefix.with_suffix('.log').open('w') as log:
        try:
            code = subprocess.run(command, cwd=root / 'deps/v8', stdout=log,
                                  stderr=subprocess.STDOUT, timeout=600).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    data = json.loads(prefix.with_suffix('.json').read_text()) if prefix.with_suffix('.json').exists() else {}
    count = data.get('test_total', 0)
    failures = data.get('results', [])
    passed = code == 0 and count >= 40 and not failures
    results.append(dict(profile=name, command=command, exit=code,
                        tests=count, unexpected=len(failures), passed=passed,
                        seconds=round(time.monotonic() - started, 2)))
    print(json.dumps(results[-1]), flush=True)
output = root / 'out' / f'sparc64-turbofan-stress-{phase}.json'
output.write_text(json.dumps(results, indent=2) + '\n')
raise SystemExit(not all(result['passed'] for result in results))
