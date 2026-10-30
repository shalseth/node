#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run bounded production-feature acceptance or all inherited regression gates."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

from wasm_production_validation import validate_sources

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--stage', choices=['prep', 'features', 'regressions',
                                       'optimizer-regressions'], required=True)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
validate_sources(root)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
current = {name: digest(root / 'out/Release' / name)
           for name in ['node', 'd8', 'inspector-test']}
output = root / ('out/sparc64-wasm-production-validation-' + args.stage + '-' + args.label)
assert not output.exists(), 'Preserve previous validation before retrying'
output.mkdir(parents=True)


def script(name, *arguments):
    return ['python3', '-u', 'tools/sparc64/' + name + '.py', *arguments]


if args.stage == 'prep':
    tasks = [('config', [script('write-test-config', '--allow-turbofan', '--allow-webassembly')])]
    groups = ['node-api', 'addons', 'js-native-api', 'sqlite']
    if (root / 'out/sparc64-node-features-parent.json').exists():
        groups.append('ffi')
    for group in groups:
        tasks.append(('build-' + group, [['python3', '-u', 'tools/build_addons.py',
            'test/' + group, '--out-dir', 'out/Release', '--loglevel=error']]))
elif args.stage == 'features':
    tasks = []
    for tier in ['liftoff', 'optimized', 'default']:
        tasks.append(('spec-' + tier, [script('test-wasm-production-spec',
            '--feature', 'production', '--tier', tier, '--label', args.label)]))
    for suite in ['simd-abi', 'simd-core', 'gc-core', 'gc-extended', 'tiering-core', 'deopt-all', 'inlining-core']:
        tiers = ['liftoff', 'optimized'] if suite in ['simd-core', 'gc-core'] else ['default']
        for tier in tiers:
            tasks.append((suite + '-' + tier, [script('test-wasm-production-upstream',
                '--suite', suite, '--tier', tier, '--label', args.label + '-' + suite + '-' + tier)]))
    tasks.append(('lazy-abi', [script('test-wasm-production-upstream', '--suite', 'simd-abi',
        '--tier', 'default', '--lazy', '--label', args.label + '-lazy-abi')]))
    for tier in ['liftoff', 'optimized']:
        tasks.append(('application-' + tier, [script('test-wasm-simd-application',
            '--tier', tier, '--label', args.label + '-' + tier)]))
    tasks += [('inspector', [script('test-wasm-production-inspector', '--suite', 'all',
                '--timeout', '10', '--label', args.label)]),
              ('debug-simd', [script('test-wasm-debug-simd', '--label', args.label)]),
              ('mixed', [script('test-wasm-production-mixed', '--label', args.label)])]
    trace = script('test-wasm-production-upstream', '--tier', 'default',
        '--label', args.label + '-trace')
    for name in ['wasm/inlining', 'wasm/wasm-gc-inlining-nested', 'wasm/wasm-wrapper-inlining']:
        trace += ['--case', name]
    for flag in ['--trace-wasm-inlining', '--trace-turbo-inlining', '--wasm-inlining',
                 '--turbo-inline-js-wasm-calls']:
        trace += ['--extra-flag=' + flag]
    tasks.append(('inlining-proof', [trace, script('record-wasm-inlining-proof',
        '--input-label', args.label + '-trace', '--label', args.label)]))
elif args.stage == 'optimizer-regressions':
    tasks = []
    for name, report in [('scalar', 'sparc64-wasm-scalar-current.json'),
                         ('jspi', 'sparc64-jspi-current.json'),
                         ('atomics', 'sparc64-wasm-atomics-upstream.json')]:
        data = json.loads((root / 'out' / report).read_text())
        # These reports supply case names; the new run verifies fixture bytes
        # against HEAD and records the current binary independently.
        assert data['passed']
        command = script('test-wasm-production-upstream', '--tier', 'optimized',
                         '--label', args.label + '-' + name + '-optimized')
        for item in data['cases']:
            case = item['name'].removeprefix('upstream-')
            command += ['--case', case if '/' in case else 'wasm/' + case]
        tasks.append((name, [command]))
    tasks.append(('atomic-runtime', [script('test-wasm-atomics', '--tier', 'optimized',
        '--group', 'runtime', '--label', args.label + '-optimized')]))
else:
    tasks = [
        ('jspi', [script('test-jspi-upstream'), script('test-jspi-node'),
                  script('test-jspi-runtime'), script('test-jspi-termination'),
                  script('test-jspi-inspector')]),
        ('wasm', [script('test-wasm-baseline'), script('test-wasm-upstream'),
                  script('test-wasm-jsapi'), script('test-wasm-integration'),
                  script('test-wasm-platform'), script('test-wasm-node-modules')]),
        ('scalar', [script('test-wasm-scalar-acceptance')]),
        ('atomics', [script('test-wasm-atomics')]),
        ('javascript', [script('test-wasm-js-regression')]),
        ('node-broad', [script('test-conformance', '--profile', 'wasm', '--group', 'broad')]),
        ('node-addons', [script('test-conformance', '--profile', 'wasm', '--group', 'addons'),
                         script('test-conformance', '--profile', 'wasm', '--group', 'native'),
                         script('test-cctest', '--profile', 'wasm')]),
        ('runtime', [script('test-turbofan-runtime', '--group', 'directed'),
                     script('test-turbofan-runtime', '--group', 'applications')]),
        ('snapshot', [script('test-snapshot-state'), script('test-snapshot-array-buffer-keys'),
                      script('test-snapshot-regexp-flags')]),
        ('regexp', [script('test-regexp-upstream')]),
        ('workloads', [script('test-workloads')]),
        ('platform', [script('test-platform-assumptions'), script('test-zip-interop')]),
    ]


def one(task):
    name, commands = task
    steps = []
    for index, command in enumerate(commands):
        start = time.monotonic()
        with (output / (name + '-' + str(index) + '.log')).open('w') as log:
            try:
                code = subprocess.run(command, cwd=root, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=3600).returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        steps.append(dict(command=command, exit_code=code,
                          seconds=round(time.monotonic() - start, 3)))
        print(json.dumps(dict(task=name, **steps[-1])), flush=True)
        if code != 0:
            break
    return dict(name=name, passed=all(s['exit_code'] == 0 for s in steps), steps=steps)


with ThreadPoolExecutor(max_workers=1 if args.stage == 'prep' else 4) as pool:
    results = list(pool.map(one, tasks))
controlled_fixture_failure = None
if args.stage == 'features':
    failures = [item for item in results if not item['passed']]
    control = one(('instance-gc-control', [script('test-wasm-instance-gc-control',
                                                '--label', args.label)]))
    results.append(control)
    if (control['passed'] and len(failures) == 1 and
            failures[0]['name'] == 'gc-extended-default'):
        controlled_fixture_failure = 'gc-extended-default'
assert current == {name: digest(root / 'out/Release' / name) for name in current}
validate_sources(root)
result = dict(passed=all(item['passed'] or item['name'] == controlled_fixture_failure
                       for item in results), stage=args.stage,
              controlled_fixture_failure=controlled_fixture_failure,
              label=args.label, binary_sha256=current, results=results)
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not result['passed'])
