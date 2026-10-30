#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run unchanged upstream Wasm cases against a recorded native compilation tier."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import re
import resource
import shlex
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/d8')
parser.add_argument('--tier', choices=['liftoff', 'optimized', 'tiered', 'upstream', 'default'], default='liftoff')
parser.add_argument('--label', default='current')
parser.add_argument('--lazy', action='store_true')
parser.add_argument('--case', action='append')
parser.add_argument('--extra-flag', action='append', default=[])
parser.add_argument('--suite', choices=['simd-abi', 'simd-core', 'gc-core', 'gc-extended', 'tiering-core', 'inlining-core', 'deopt-all'])
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
suite_source = root / 'tools/sparc64/wasm-production-upstream-suites.json'
suites = json.loads(suite_source.read_text())['suites']
if args.case and args.suite:
    parser.error('--case and --suite are mutually exclusive')
suite = args.suite or ('custom' if args.case else 'simd-abi')
names = args.case or suites[suite]
tier_flags = {'liftoff': ['--liftoff-only'],
              'optimized': ['--no-liftoff-only', '--no-liftoff'],
              'tiered': ['--no-liftoff-only', '--liftoff'],
              'upstream': ['--no-liftoff-only'], 'default': []}[args.tier]
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
binary_hash = digest(args.binary)
output = root / ('out/sparc64-wasm-production-upstream-' + args.label)
assert not output.exists(), 'Preserve previous evidence before retrying'
output.mkdir(parents=True)


def one(name):
    source = 'deps/v8/test/mjsunit/' + name + '.js'
    fixture = subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
    assert (root / source).read_bytes() == fixture
    flags = [flag for line in re.findall(r'^// Flags: (.*)$', fixture.decode(), re.M)
             for flag in shlex.split(line)]
    command = [str(args.binary), *tier_flags,
               '--wasm-lazy-compilation' if args.lazy else '--no-wasm-lazy-compilation',
               '--allow-natives-syntax', '--expose-gc',
               *args.extra_flag, *flags, 'test/mjsunit/mjsunit.js', 'test/mjsunit/' + name + '.js']
    start = time.monotonic()
    try:
        run = subprocess.run(command, cwd=root / 'deps/v8', capture_output=True, timeout=120)
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    leaf = name.replace('/', '__')
    (output / (leaf + '.stdout')).write_bytes(stdout)
    (output / (leaf + '.stderr')).write_bytes(stderr)
    item = dict(name=name, command=command, exit_code=code, passed=code == 0,
                seconds=round(time.monotonic() - start, 3), source_sha256=digest(root / source))
    print(json.dumps(item), flush=True)
    return item


with ThreadPoolExecutor(max_workers=4) as pool:
    cases = list(pool.map(one, names))
assert digest(args.binary) == binary_hash
support = ['deps/v8/test/mjsunit/mjsunit.js', 'deps/v8/test/mjsunit/wasm/wasm-module-builder.js',
           'deps/v8/test/mjsunit/value-helper.js', 'deps/v8/test/mjsunit/wasm/exceptions-utils.js']
for source in support:
    assert (root / source).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
result = dict(passed=bool(cases) and all(item['passed'] for item in cases),
              binary_sha256=binary_hash, tier=args.tier, lazy=args.lazy, cases=cases,
              suite=suite, suite_source_sha256=digest(suite_source), extra_flags=args.extra_flag,
              support_sha256={source: digest(root / source) for source in support})
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
