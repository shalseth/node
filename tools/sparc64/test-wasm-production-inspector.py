#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run unchanged Wasm inspector tests and their upstream expected output."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=root / 'out/Release')
parser.add_argument('--suite', choices=['core', 'all'], default='core')
parser.add_argument('--label', default='current')
parser.add_argument('--timeout', type=int, default=120)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
if args.suite == 'core':
    names = ['wasm-set-breakpoint', 'wasm-stepping', 'wasm-scope-info',
             'wasm-inspect-many-registers', 'wasm-gc-breakpoints',
             'wasm-gc-in-debug-break', 'wasm-evaluate-on-call-frame',
             'wasm-jspi-async-stack']
else:
    names = sorted(p.stem for p in (root / 'deps/v8/test/inspector/debugger').glob('wasm-*.js'))
    assert len(names) == 46, len(names)
selected = ['inspector/debugger/' + name for name in names]
fixtures = ['deps/v8/test/inspector/debugger/' + name + suffix
            for name in names for suffix in ['.js', '-expected.txt']]
fixtures += ['deps/v8/test/inspector/wasm-inspector-test.js',
             'deps/v8/test/inspector/protocol-test.js',
             'deps/v8/test/inspector/inspector.status',
             'deps/v8/test/mjsunit/wasm/wasm-module-builder.js']
for name in fixtures:
    assert (root / name).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)
binary = args.build / 'inspector-test'
binary_hash = digest(binary)
prefix = root / ('out/sparc64-wasm-production-inspector-' + args.suite + '-' + args.label)
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
raw = Path(str(prefix) + '-raw.json')
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=' + str(args.build),
           '--variants=default', '--no-presubmit', '--run-skipped', '-j4', '-t' + str(args.timeout),
           '--progress=verbose', '--random-seed=42', '--rerun-failures-count=0',
           '--exit-after-n-failures=0',
           '--extra-flags=--liftoff-only --no-wasm-lazy-compilation',
           '--json-test-results=' + str(raw), *selected]
with prefix.with_suffix('.log').open('w') as log:
    try:
        code = subprocess.run(command, cwd=root / 'deps/v8', stdout=log,
                              stderr=subprocess.STDOUT, timeout=1800).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
data = json.loads(raw.read_text()) if raw.exists() else None
assert digest(binary) == binary_hash
for name in fixtures:
    assert (root / name).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)
result = dict(passed=code == 0 and data is not None and data['test_total'] == len(selected) and not data['results'],
              command=command, exit_code=code, binary_sha256=binary_hash,
              suite=args.suite, selected=selected, tests=data['test_total'] if data else 0,
              source_sha256={name: digest(root / name) for name in fixtures},
              unexpected=data['results'] if data else None)
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: result[key] for key in ['passed', 'suite', 'exit_code', 'tests']}), flush=True)
raise SystemExit(not result['passed'])
