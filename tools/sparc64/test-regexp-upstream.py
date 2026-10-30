#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run upstream regexp suites using default and forced-native compilation."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
v8 = root / 'deps/v8'
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--full', action='store_true', help='Also run all default mjsunit cases')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = sorted('mjsunit/' + str(p.relative_to(v8 / 'test/mjsunit')).removesuffix('.js')
               for p in (v8 / 'test/mjsunit').rglob('*.js')
               if 'regexp' in p.name or 'reg-exp' in p.name)
cases += ['mjsunit/regress/regress-1255368', 'mjsunit/regress/regress-996234',
          'mjsunit/third_party/regexp-pcre/regexp-pcre']
cases = sorted(set(cases))
native_excluded = [name for name in cases
                   if '--regexp-interpret-all' in (v8 / 'test' / (name + '.js')).read_text()]
native_cases = [name for name in cases if name not in native_excluded]
binary = root / 'out/Release/d8'
digest = hashlib.sha256(binary.read_bytes()).hexdigest()
profiles = [('tier-up', 'default', [], cases),
            ('native', 'stress_regexp_jit', [], native_cases),
            ('native-gc', 'stress_regexp_jit', ['--verify-heap',
                           '--stress-compaction', '--compact-on-every-full-gc'], native_cases)]
if args.full:
    profiles.append(('full', 'default', [], ['mjsunit']))
results = []
for name, variant, flags, selected in profiles:
    prefix = root / 'out' / ('sparc64-regexp-upstream-' + name)
    command = ['python3', '-u', 'tools/run-tests.py', '--outdir=../../out/Release',
               '--variants=' + variant, '--no-presubmit', '-j16', '-t60',
               '--progress=verbose', '--random-seed=42',
               '--exit-after-n-failures=10', '--rerun-failures-count=0',
               '--extra-flags=' + ' '.join(flags),
               f'--json-test-results={prefix}.json', *selected]
    start = time.monotonic()
    with prefix.with_suffix('.log').open('w') as log:
        code = subprocess.run(command, cwd=v8, stdout=log, stderr=subprocess.STDOUT,
                              timeout=1200).returncode
    data = json.loads(prefix.with_suffix('.json').read_text()) if prefix.with_suffix('.json').exists() else {}
    count = data.get('test_total', 0)
    failures = data.get('results', [])
    passed = code == 0 and not failures and count >= (6000 if name == 'full' else 100)
    item = dict(profile=name, excluded_interpreter_only=(native_excluded if variant != 'default' else []), command=command, exit_code=code, tests=count,
                unexpected=len(failures), passed=passed,
                binary_sha256=digest, seconds=round(time.monotonic() - start, 3))
    results.append(item)
    (root / 'out/sparc64-regexp-upstream.json').write_text(json.dumps(results, indent=2) + '\n')
    print(json.dumps(item), flush=True)
    if not passed:
        raise SystemExit('Upstream regexp validation failed: ' + name)
assert hashlib.sha256(binary.read_bytes()).hexdigest() == digest
print('REGEXP_UPSTREAM_OK', flush=True)
