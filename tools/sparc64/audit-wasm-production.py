#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Gate all five production Wasm milestones and every inherited regression."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tarfile

from wasm_production_validation import digest, validate_sources

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', default='production-atomic-endian')
parser.add_argument('--features-only', action='store_true')
args = parser.parse_args()
changed = validate_sources(root)
current = {name: digest(root / 'out/Release' / name)
           for name in ['node', 'd8', 'inspector-test']}


def read(name):
    return json.loads((root / 'out' / name).read_text())


def original(source, value):
    data = subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
    assert (root / source).read_bytes() == data
    assert hashlib.sha256(data).hexdigest() == value


stages = ['prep', 'features', 'optimizer-regressions']
if not args.features_only:
    stages.append('regressions')
for stage in stages:
    data = read('sparc64-wasm-production-validation-' + stage + '-' + args.label + '.json')
    assert data['binary_sha256'] == current
    assert data['stage'] == stage and data['label'] == args.label
    failures = [item for item in data['results'] if not item['passed']]
    if stage == 'features':
        assert data['passed'] and len(failures) == 1
        assert data['controlled_fixture_failure'] == 'gc-extended-default'
        assert failures[0]['name'] == 'gc-extended-default'
        assert len(failures[0]['steps']) == 1 and failures[0]['steps'][0]['exit_code'] == 1
    else:
        assert data['passed'] and not failures
    assert all(item['passed'] and item['steps'] and
               all(step['exit_code'] == 0 for step in item['steps'])
               for item in data['results'] if item not in failures)

archive_hash = '571032b63659945521562c7a8f4646b1899af3c230fc6a209874d4f028350512'
for tier in ['liftoff', 'optimized', 'default']:
    prefix = 'sparc64-wasm-production-spec-production-' + tier + '-' + args.label
    data, raw = read(prefix + '.json'), read(prefix + '-raw.json')
    assert data['passed'] and data['binary_sha256'] == current['d8']
    assert data['tests'] == len(data['selected']) == len(data['source_sha256']) == 258
    assert not data['unexpected'] and raw['test_total'] == 258 and not raw['results']
    assert data['tier'] == tier and data['feature'] == 'production'
    assert data['archive_sha1'] == 'b6bfccaf9809d1e689417bc9f8e0efbacfea9f98'
    assert data['archive_sha256'] == archive_hash
    archive = root / ('out/wasm-upstream-inputs/wasm-spec-tests-' + data['archive_sha1'] + '.tar.gz')
    assert digest(archive) == archive_hash
    with tarfile.open(archive) as stream:
        selected = []
        for member in stream:
            if not member.isfile() or not member.name.endswith('.js'):
                continue
            relative = str(Path(member.name).relative_to('tests'))
            if relative.startswith('proposals/'):
                assert relative in data['excluded_experimental']
                continue
            source = root / 'deps/v8/test/wasm-spec-tests/tests' / relative
            assert source.read_bytes() == stream.extractfile(member).read()
            assert digest(source) == data['source_sha256'][relative]
            selected.append('wasm-spec-tests/' + relative.removesuffix('.js'))
        assert sorted(selected) == data['selected']
    assert len(data['excluded_experimental']) == 21
    original('deps/v8/test/wasm-spec-tests/wasm-spec-tests.status', data['status_sha256'])
    flags = next(item for item in data['command'] if item.startswith('--extra-flags='))
    assert flags == '--extra-flags=' + {
        'liftoff': '--liftoff-only', 'optimized': '--no-liftoff-only --no-liftoff',
        'default': ''}[tier] + ' --no-wasm-lazy-compilation'

suite_source = root / 'tools/sparc64/wasm-production-upstream-suites.json'
suites = json.loads(suite_source.read_text())['suites']
expected_counts = {'simd-abi': 10, 'simd-core': 18, 'gc-core': 22,
                   'gc-extended': 17, 'tiering-core': 4, 'deopt-all': 30, 'inlining-core': 17}
assert {key: len(value) for key, value in suites.items()} == expected_counts
deopts = sorted('wasm/deopt/' + path.stem for path in
                (root / 'deps/v8/test/mjsunit/wasm/deopt').glob('*.js'))
assert sorted(suites['deopt-all']) == deopts
simd = sorted('wasm/' + path.stem for path in
              (root / 'deps/v8/test/mjsunit/wasm').glob('*simd*.js')
              if path.stem != 'simd-fp16')
assert sorted(suites['simd-core']) == simd
upstream = []
for suite in suites:
    tiers = ['liftoff', 'optimized'] if suite in ['simd-core', 'gc-core'] else ['default']
    for tier in tiers:
        upstream.append((args.label + '-' + suite + '-' + tier, suite, tier, False))
upstream.append((args.label + '-lazy-abi', 'simd-abi', 'default', True))
for label, suite, tier, lazy in upstream:
    data = read('sparc64-wasm-production-upstream-' + label + '.json')
    assert data['passed'] == (suite != 'gc-extended')
    assert data['binary_sha256'] == current['d8']
    assert [item['name'] for item in data['cases']] == suites[suite]
    assert data['tier'] == tier and data['lazy'] == lazy and not data['extra_flags']
    assert data['suite_source_sha256'] == digest(suite_source)
    for item in data['cases']:
        if item['name'] == 'wasm/instance-gc':
            assert not item['passed'] and item['exit_code'] == 1
        else:
            assert item['passed'] and item['exit_code'] == 0
        original('deps/v8/test/mjsunit/' + item['name'] + '.js', item['source_sha256'])
        for suffix in ['stdout', 'stderr']:
            assert (root / 'out' / ('sparc64-wasm-production-upstream-' + label) /
                    (item['name'].replace('/', '__') + '.' + suffix)).is_file()
    for source, value in data['support_sha256'].items():
        original(source, value)

control = read('sparc64-wasm-production-instance-gc-control-' + args.label + '.json')
assert control['passed'] and control['binary_sha256'] == current['d8']
assert not control['original_test_passed'] and control['original_exit_code'] == 1
assert control['correction_from'] == (
    '  var view = new Int32Array(instance.exports.memory.buffer);\n  view[0] = val;')
assert control['correction_to'] == (
    '  var view = new DataView(instance.exports.memory.buffer);\n  view.setInt32(0, val, true);')
original(control['original_source'], control['original_sha256'])
original_bytes = (root / control['original_source']).read_bytes()
assert original_bytes.count(control['correction_from'].encode()) == 1
derived = root / control['derived_source']
assert derived.read_bytes() == original_bytes.replace(
    control['correction_from'].encode(), control['correction_to'].encode(), 1)
assert digest(derived) == control['derived_sha256']
assert control['host_endian_probe'] == dict(hostBytes=[0, 0, 0, 99],
                                          wasmLittleEndianValue=1660944384)
negative_stdout = (root / 'out' / ('sparc64-wasm-production-upstream-' + args.label +
                   '-gc-extended-default') / 'wasm__instance-gc.stdout')
assert digest(negative_stdout) == control['original_stdout_sha256']
assert 'Failure: expected <99> found <1660944384>' in negative_stdout.read_text()
assert [item['tier'] for item in control['results']] == ['liftoff', 'optimized', 'default']
assert all(item['passed'] and item['exit_code'] == 0 for item in control['results'])

for group, report, count in [('scalar', 'sparc64-wasm-scalar-current.json', 15),
                            ('jspi', 'sparc64-jspi-current.json', 18),
                            ('atomics', 'sparc64-wasm-atomics-upstream.json', 14)]:
    inherited = read(report)
    data = read('sparc64-wasm-production-upstream-' + args.label + '-' + group + '-optimized.json')
    assert data['passed'] and data['binary_sha256'] == current['d8']
    assert data['tier'] == 'optimized' and len(data['cases']) == count
    names = [item['name'].removeprefix('upstream-') for item in inherited['cases']]
    assert [item['name'] for item in data['cases']] == [
        name if '/' in name else 'wasm/' + name for name in names]
    for item in data['cases']:
        assert item['passed'] and item['exit_code'] == 0
        original('deps/v8/test/mjsunit/' + item['name'] + '.js', item['source_sha256'])

atomic_runtime = {}
for group in ['matrix', 'spec', 'threads']:
    data = read('sparc64-wasm-atomics-' + args.label + '-optimized-' + group + '.json')
    assert data['passed'] and data['tier'] == 'optimized'
    assert data['binary_sha256'] == {key: current[key] for key in ['node', 'd8']}
    assert len(data['cases']) == 1
    item = data['cases'][0]
    assert item['exit_code'] == 0 and item['data']['passed']
    assert '--no-liftoff-only' in item['command'] and '--no-liftoff' in item['command']
    for source, value in item['source_sha256'].items():
        assert digest(root / source) == value
    atomic_runtime[group] = item['data']
assert atomic_runtime['matrix']['checks'] >= 198392
assert atomic_runtime['matrix']['nativeFunctions'] == 504
assert atomic_runtime['spec']['commands'] == 372 and atomic_runtime['spec']['nativeFunctions'] == 73
assert atomic_runtime['threads']['serial'] == atomic_runtime['threads']['threaded'] == 1024
assert atomic_runtime['threads']['bytes'] == 16777216 and atomic_runtime['threads']['growthPages'] == 321
assert atomic_runtime['threads']['nativeFunctions'] >= 55

for tier in ['liftoff', 'optimized']:
    data = read('sparc64-wasm-simd-application-' + args.label + '-' + tier + '.json')
    assert data['passed'] and data['tier'] == tier and data['binary_sha256'] == current['node']
    assert [item['name'] for item in data['results']] == ['matrix', 'undici']
    for item in data['results']:
        assert item['passed'] and item['exit_code'] == 0
        assert digest(root / item['fixture']) == item['fixture_sha256']
    assert data['results'][0]['data'] == dict(passed=True, tier=tier, operations=59,
        nativeFunctions=59, invocations=11328, checks=1812480, seed=131160559)
    assert data['results'][1]['data'] == dict(passed=True, tier=tier, checks=1320,
        nativeFunctions=2, simd=True,
        moduleSha256='18403ee56fab07f89a845028bad358bb3ebcda213f8e2ec0989a991e9c33da69')
    original(data['source'], data['source_sha256'])

debug = read('sparc64-wasm-production-inspector-all-' + args.label + '.json')
assert debug['passed'] and debug['binary_sha256'] == current['inspector-test']
assert debug['tests'] == len(debug['selected']) == 46 and not debug['unexpected']
for source, value in debug['source_sha256'].items():
    original(source, value)
vectors = read('sparc64-wasm-debug-simd-' + args.label + '.json')
assert vectors['passed'] and vectors['binary_sha256'] == current['inspector-test']
assert vectors['data'] == dict(passed=True, vectors=20, checks=40,
                               gcWhilePaused=True, checksum=942014689)
for source, value in vectors['source_sha256'].items():
    assert digest(root / source) == value

mixed = read('sparc64-wasm-production-mixed-' + args.label + '.json')
assert mixed['passed'] and mixed['binary_sha256'] == current['node']
assert mixed['fixture_sha256'] == digest(root / mixed['fixture'])
assert [item['name'] for item in mixed['results']] == ['basic', 'compaction', 'incremental']
for item in mixed['results']:
    assert item['passed'] and item['exit_code'] == 0
    assert item['data'] == [dict(passed=True, calls=34, checks=102, suspensions=102,
        vectors=20, concurrentStacks=4, liftoff=True, optimized=True, wasmGC=True)]
    assert not any(flag in item['command'] for flag in ['--liftoff-only', '--no-liftoff'])
proof = read('sparc64-wasm-inlining-proof-' + args.label + '.json')
assert proof['passed'] and proof['binary_sha256'] == current['d8']
assert proof['wasm_bodies_inlined_into_js'] >= 4 and proof['wasm_direct_calls_inlined'] > 0
assert digest(root / proof['upstream_report']) == proof['upstream_report_sha256']
for source, value in proof['trace_sha256'].items():
    assert digest(root / source) == value
for source, value in proof['compiler_source_sha256'].items():
    original(source, value)

flags = (root / 'deps/v8/src/flags/flag-definitions.h').read_text()
assert 'SPARC64_WASM_BASELINE_ONLY' not in flags
assert re.search(r'DEFINE_BOOL\(liftoff_only, false,', flags)
assert re.search(r'DEFINE_BOOL\(wasm_inlining, true,', flags)
assert re.search(r'DEFINE_BOOL\(\s*turbo_inline_js_wasm_calls, true,', flags)
if args.features_only:
    result = dict(passed=True, label=args.label, binary_sha256=current,
                  spec_cases_per_tier=258, upstream_suite_cases=expected_counts,
                  inspector_cases=46, mixed_gc_profiles=3,
                  upstream_fixture_limitation='instance-gc host-endian write; '
                  'original failure retained, explicit-LE copy passes three modes')
    (root / 'out/sparc64-wasm-production-feature-audit.json').write_text(
        json.dumps(result, indent=2) + '\n')
    print('WASM_PRODUCTION_FEATURE_AUDIT_OK', flush=True)
    raise SystemExit(0)
subprocess.run(['python3', 'tools/sparc64/audit-jspi.py', '--allow-production',
                '--production-label', args.label], cwd=root, check=True)
parent = root.with_name('node-sparc64-jspi')
checkpoint = Path((parent / 'out/sparc64-jspi-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
subprocess.run(['python3', str(parent / 'tools/sparc64/verify-checkpoint.py'),
                str(checkpoint), '--source-root', str(parent)], check=True)
for name in ['node', 'd8', 'cctest', 'node_mksnapshot', 'inspector-test']:
    assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name]
for source, artifact in [('config.gypi', 'config.gypi'),
                         ('out/Release/v8_build_config.json', 'v8_build_config.json')]:
    assert digest(parent / source) == manifest['artifact_sha256'][artifact]
result = dict(passed=True, label=args.label, binary_sha256=current,
              production_source_changes=changed, spec_cases_per_tier=258,
              upstream_suite_cases=expected_counts, inspector_cases=46,
              mixed_gc_profiles=3, inherited_audit='sparc64-jspi-audit.json',
              parent_checkpoint=checkpoint.name,
              upstream_fixture_limitation='instance-gc assumes host little endian; '
              'unchanged failure retained, separate corrected copy passes all three modes')
(root / 'out/sparc64-wasm-production-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('WASM_PRODUCTION_AUDIT_OK', flush=True)
