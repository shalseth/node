#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Gate JSPI acceptance, unchanged upstream fixtures and all preserved regressions."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-production', action='store_true')
parser.add_argument('--production-label', default='production-atomic-endian')
args = parser.parse_args()
if args.allow_production:
    from wasm_production_validation import validate_sources
    validate_sources(root)
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
read = lambda name: json.loads((root / 'out' / name).read_text())
original = lambda name: subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)
subprocess.run(['python3', 'tools/sparc64/audit-wasm-scalar.py', *(['--allow-production'] if args.allow_production else ['--allow-jspi'])], cwd=root, check=True)
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8', 'inspector-test']}
parent = root.with_name('node-sparc64-wasm-scalar')
checkpoint = Path((parent / 'out/sparc64-wasm-scalar-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == read('sparc64-jspi-parent.json') and manifest['profile'] == 'wasm-scalar'
subprocess.run(['python3', str(parent / 'tools/sparc64/verify-checkpoint.py'), str(checkpoint),
                '--source-root', str(parent)], check=True)
for name in ['node', 'd8', 'cctest', 'node_mksnapshot']:
    assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name]
for source, name in [('config.gypi', 'config.gypi'), ('out/Release/v8_build_config.json', 'v8_build_config.json')]:
    assert digest(parent / source) == manifest['artifact_sha256'][name]

# The child adds only JSPI builtins and the shared prompt handler-table guard.
changed = set()
for command in [['git', 'diff', '--name-only', 'HEAD'], ['git', 'ls-files', '--others', '--exclude-standard']]:
    changed.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
production = {name for name in changed | set(manifest['source_sha256']) if not name.startswith('tools/sparc64/')}
production_changes = {name for name in production if name not in manifest['source_sha256'] or
                      digest(root / name) != manifest['source_sha256'][name]}
if args.allow_production:
    validate_sources(root)
    from wasm_production_validation import PRODUCTION_SOURCES
    additional_sources = set()
    if (root / 'out/sparc64-node-features-parent.json').exists():
        from node_features_validation import FEATURE_SOURCES
        additional_sources = FEATURE_SOURCES
    assert production_changes == PRODUCTION_SOURCES | additional_sources | {
        'deps/v8/src/builtins/setup-builtins-internal.cc'}
else:
    assert production_changes == {'deps/v8/src/builtins/sparc64/builtins-sparc64.cc',
                                  'deps/v8/src/builtins/setup-builtins-internal.cc'}, production_changes
setup = 'deps/v8/src/builtins/setup-builtins-internal.cc'
before = original(setup)
old_guard = b'     V8_TARGET_ARCH_PPC64 || V8_TARGET_ARCH_S390X || V8_TARGET_ARCH_RISCV32)'
new_guard = old_guard[:-1] + b' || \\\n     V8_TARGET_ARCH_SPARC64)'
assert before.count(old_guard) == 1 and (root / setup).read_bytes() == before.replace(old_guard, new_guard, 1)

names = ['jspi', 'jspi-export', 'jspi-generic-wrapper', 'jspi-no-stress', 'jspi-notraps',
         *['regress/wasm/regress-' + suffix for suffix in ['336358915', '336852356', '342522151',
           '361123483', '361717714', '364667545', '365376497', '381917890', '392928805',
           '406043349', '407797300', '408254017', 'v8-14471']]]


def upstream(label, selected, binary_hash, baseline=False):
    data = read('sparc64-jspi-' + label + '.json')
    assert data['binary_sha256'] == binary_hash
    assert [item['name'] for item in data['cases']] == selected
    assert data['passed'] == (not baseline)
    for item in data['cases']:
        name = item['name']
        relative = (name if '/' in name else 'wasm/' + name) + '.js'
        source = 'deps/v8/test/mjsunit/' + relative
        fixture = original(source)
        assert (root / source).read_bytes() == fixture
        assert item['source_sha256'] == hashlib.sha256(fixture).hexdigest()
        flags = [flag for line in re.findall(r'^// Flags: (.*)$', fixture.decode(), re.M)
                 for flag in shlex.split(line)]
        assert item['command'][1:] == ['--liftoff-only', '--no-wasm-lazy-compilation',
                '--allow-natives-syntax', '--expose-gc', *flags,
                'test/mjsunit/mjsunit.js', 'test/mjsunit/' + relative]
        expected = (-5 if name != 'regress/wasm/regress-408254017' else 0) if baseline else 0
        assert item['exit_code'] == expected, (label, name)
        for suffix in ['stdout', 'stderr']:
            assert (root / 'out' / ('sparc64-jspi-' + label) / (name.replace('/', '__') + '.' + suffix)).is_file()
    return data


upstream('current', names, current['d8'])
upstream('baseline', names, manifest['artifact_sha256']['d8'], baseline=True)
for label, expected, binary_hash in [('current', 0, current['node']),
        ('baseline', -5, manifest['artifact_sha256']['node'])]:
    data = read('sparc64-jspi-node-' + label + '.json')
    assert data['passed'] == (expected == 0) and data['exit_code'] == expected
    assert data['binary_sha256'] == binary_hash
    source = data['source']
    assert source == 'test/parallel/test-wasm.mjs'
    assert (root / source).read_bytes() == original(source)
    assert digest(root / source) == data['source_sha256']
    assert data['command'][1:] == [source]
    for suffix in ['stdout', 'stderr']:
        assert (root / 'out' / ('sparc64-jspi-node-' + label + '.' + suffix)).is_file()
assert json.loads((root / 'tools/sparc64/wasm-feature-exclusions.json').read_text()) == {}

runtime = read('sparc64-jspi-runtime-all.json')
assert runtime['passed'] and runtime['binary_sha256'] == current['node'] and runtime['profile'] == 'all'
assert runtime['fixture'] == 'tools/sparc64/test-jspi-runtime.cjs'
assert digest(root / runtime['fixture']) == runtime['fixture_sha256']
expected = dict(passed=True, checks=151, suspensions=362, nativeFunctions=13, rejections=16,
                repeatedSuspensions=128, concurrentStacks=8, nestedDepth=8, stackReuse=64)
profiles = {'basic': [], 'compaction': ['--compact-on-every-full-gc'],
            'incremental': ['--stress-incremental-marking']}
assert [item['name'] for item in runtime['results']] == list(profiles)
for item in runtime['results']:
    assert item['passed'] and item['exit_code'] == 0 and item['data'] == expected
    assert item['command'][1:] == ['--liftoff-only', '--no-wasm-lazy-compilation',
        '--allow-natives-syntax', '--expose-gc', '--verify-heap', *profiles[item['name']], runtime['fixture']]
    stdout = (root / 'out/sparc64-jspi-runtime' / (item['name'] + '.stdout')).read_text()
    markers = [json.loads(line.removeprefix('JSPI_RUNTIME_OK ')) for line in stdout.splitlines()
               if line.startswith('JSPI_RUNTIME_OK ')]
    assert markers == [expected]
    assert (root / 'out/sparc64-jspi-runtime' / (item['name'] + '.stderr')).is_file()
termination = read('sparc64-jspi-termination.json')
assert termination['passed'] and termination['binary_sha256'] == current['d8']
assert termination['rounds'] == 20 and termination['cases_per_round'] == 2
assert [item['label'] for item in termination['results']] == [f'termination-{i:02d}' for i in range(20)]
for item in termination['results']:
    assert item['passed'] and item['exit_code'] == 0
    upstream(item['label'], ['jspi', 'regress/wasm/regress-361717714'], current['d8'])
    for suffix in ['stdout', 'stderr']:
        assert (root / 'out/sparc64-jspi-termination' / (item['label'] + '.' + suffix)).is_file()

build = read(('sparc64-wasm-production-inspector-build-' + args.production_label + '.json')
             if args.allow_production else 'sparc64-jspi-inspector-build.json')
assert build['passed'] and build['binary_sha256'] == {name: current[name] for name in ['node', 'd8']}
assert build['inspector_sha256'] == current['inspector-test'] and build['compile_exit_codes'] == [0] * 5
assert build['link_exit_code'] == 0 and len(build['source_sha256']) == 5
for name, value in build['source_sha256'].items():
    assert digest(root / name) == value and (root / name).read_bytes() == original(name)
inspector = read('sparc64-jspi-inspector.json')
assert inspector['passed'] and inspector['exit_code'] == 0 and inspector['binary_sha256'] == current
for name, value in inspector['source_sha256'].items():
    assert digest(root / name) == value and (root / name).read_bytes() == original(name)
raw = read('sparc64-jspi-inspector-raw.json')
assert raw['test_total'] == 1 and not raw['results']
assert len(raw['slowest_tests']) == 1
case = raw['slowest_tests'][0]
assert case['name'] == 'inspector/debugger/wasm-jspi-async-stack'
assert case['expected'] == ['PASS'] and case['exit_code'] == 0
assert case['target_name'] == 'inspector-test' and case['variant'] == 'default'
assert case['flags'] == ['test/inspector/protocol-test.js',
    'test/inspector/debugger/wasm-jspi-async-stack.js', '--random-seed=42', '--nohard-abort',
    '--liftoff-only', '--no-wasm-lazy-compilation', '--wasm-staging', '--async-stack-traces',
    '--wasm-stack-switching-stack-size=100']
assert '--run-skipped' in inspector['command'] and inspector['command'][-1] == case['name']
for name in ['deps/v8/test/mjsunit/mjsunit.js', 'deps/v8/test/mjsunit/wasm/wasm-module-builder.js']:
    assert (root / name).read_bytes() == original(name)
result = dict(passed=True, binary_sha256=current, upstream_cases=18, node_jspi=True,
              runtime_profiles=3, runtime_per_profile=expected, termination_rounds=20,
              termination_executions=40, inspector_cases=1, parent_checkpoint=str(checkpoint),
              production_changes=sorted(production_changes))
(root / 'out/sparc64-jspi-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('JSPI_AUDIT_OK', flush=True)
