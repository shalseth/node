#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Require current scalar atomic evidence, threaded workload and preserved baseline."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-jspi', action='store_true')
parser.add_argument('--allow-production', action='store_true')
args = parser.parse_args()
if args.allow_production:
    args.allow_jspi = True
    from wasm_production_validation import validate_sources
    validate_sources(Path(__file__).resolve().parents[2])
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
subprocess.run(['python3', 'tools/sparc64/audit-wasm.py',
                *(['--allow-production'] if args.allow_production else ['--allow-jspi'] if args.allow_jspi else [])], cwd=root, check=True)
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
results = {}
for group in ['matrix', 'spec', 'upstream', 'threads']:
    evidence = json.loads((root / f'out/sparc64-wasm-atomics-{group}.json').read_text())
    assert evidence['passed'] and evidence['binary_sha256'] == current
    for item in evidence['cases']:
        assert item['exit_code'] == 0
        for source, value in item['source_sha256'].items():
            assert digest(root / source) == value, source
        if group == 'upstream':
            fixture = 'deps/v8/test/mjsunit/wasm/' + item['name'].removeprefix('upstream-') + '.js'
            assert (root / fixture).read_bytes() == subprocess.check_output(
                ['git', 'show', 'HEAD:' + fixture], cwd=root)
        if group in ['matrix', 'spec']:
            raw = root / 'out/sparc64-wasm-atomics' / (item['name'] + '.stdout')
            assert json.loads(raw.read_text().splitlines()[-1]) == item['data']
    results[group] = evidence
matrix = results['matrix']['cases'][0]['data']
assert matrix['passed'] and matrix['checks'] >= 198392 and matrix['nativeFunctions'] == 504
spec = results['spec']['cases'][0]['data']
assert spec['passed'] and spec['commands'] == 372 and spec['nativeFunctions'] == 73
assert spec['counts'] == dict(module=5, action=65, assert_return=154, assert_trap=55, assert_invalid=93)
inputs = root / 'out/wasm-threads-spec'
manifest = json.loads((inputs / 'source-manifest.json').read_text())
assert manifest['commit'] == 'cc535ada1aa21cfaa3cabf3ac73b89acef78a0a0'
assert digest(inputs / 'threads.tar.gz') == manifest['archive_sha256'] == '8cc7e0b408c6ae84865497aec07fc3afd85f5021143111681e9abc3ead963290'
with tarfile.open(inputs / 'threads.tar.gz') as stream:
    for source, value in manifest['sources'].items():
        assert digest(inputs / source) == value
        member = 'threads-' + manifest['commit'] + '/test/core/' + source.removeprefix('sources/')
        assert (inputs / source).read_bytes() == stream.extractfile(member).read()
translation = json.loads((inputs / 'translation-manifest.json').read_text())
assert digest(inputs / 'sources/threads/atomic.wast') == translation['source_sha256']
assert all(digest(inputs / path) == value for path, value in translation['files'].items())
names = {case['name'].removeprefix('upstream-') for case in results['upstream']['cases']}
assert names == {'atomic-alignment-checks', 'atomics-non-shared',
    'compare-exchange-stress', 'compare-exchange64-stress', 'shared-memory',
    'grow-shared-memory', 'grow-shared-memory-resizable-buffer',
    'shared-memory-worker-simple-gc', 'shared-memory-worker-stress',
    'shared-memory-worker-gc', 'shared-memory-worker-gc-stress',
    'shared-memory-worker-explicit-gc-stress', 'shared-memory-gc-stress',
    'shared-arraybuffer-worker-simple-gc'}
assert len(results['upstream']['cases']) == 14
negative = root / 'out/sparc64-wasm-atomics-attempts/initial-growth-interrupt'
initial = json.loads((negative / 'sparc64-wasm-atomics-upstream.json').read_text())
assert not initial['passed']
assert {item['name'] for item in initial['cases'] if item['exit_code'] != 0} == {
    'upstream-grow-shared-memory', 'upstream-grow-shared-memory-resizable-buffer'}
assert all(item['exit_code'] in (0, 'timeout') for item in initial['cases'])
for item in initial['cases']:
    for source, value in item['source_sha256'].items():
        assert digest(root / source) == value
before = (negative / 'liftoff-before-interrupt-fix.h').read_bytes()
now = (root / 'deps/v8/src/wasm/baseline/sparc64/liftoff-assembler-sparc64-inl.h').read_bytes()
def stack_check(source):
    start = source.index(b'void LiftoffAssembler::StackCheck(')
    end = source.index(b'\n}', start) + 2
    return source[start:end]
assert stack_check(before) == stack_check(now).replace(
    b'IsolateData::jslimit_offset()', b'IsolateData::real_jslimit_offset()', 1)
assert initial['binary_sha256'] != current
threads = results['threads']['cases'][0]['data']
assert threads['passed'] and threads['serial'] == threads['threaded'] == 1024
assert threads['bytes'] == 16777216 and len(threads['workers']) == 4
assert sum(threads['workers']) == 1024 and all(value > 0 for value in threads['workers'])
assert threads['nativeFunctions'] >= 55 and threads['waitChecks'] >= 9
assert threads['growthPages'] == 321
assert threads['tearingIterations'] >= 10003
assert threads['laneIterations'] >= 10003 and threads['orderingRounds'] >= 10000
assert {(item['width'], item['notifier'], item['result']) for item in threads['waitEvidence']} == {
    (4, 'wasm', 0), (4, 'js', 0), (8, 'wasm', 0), (8, 'js', 0)}
workload = root / 'out/wasm-atomics-workload'
workload_manifest = json.loads((workload / 'manifest.json').read_text())
for name, value in workload_manifest['source_sha256'].items():
    assert digest(root / name) == value
for name, value in workload_manifest['files'].items():
    assert digest(workload / name) == value
assert threads['module_sha256'] == workload_manifest['files']['threaded-hash.wasm']
assert threads['expected_sha256'] == workload_manifest['files']['expected.bin']
oracle = json.loads((workload / 'target-oracle.json').read_text())
assert oracle['passed'] and oracle['machine'] == 'sparc64' and oracle['checks'] == 1024
assert oracle['source_sha256'] == workload_manifest['source_sha256']
assert digest(workload / 'target-native-hash.so') == oracle['native_sha256']
assert digest(workload / 'target-expected.bin') == oracle['expected_sha256'] == threads['expected_sha256']
parent = root.with_name('node-sparc64-wasm')
checkpoint = Path((parent / 'out/sparc64-wasm-checkpoint-path.txt').read_text().strip())
parent_manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert parent_manifest == json.loads((root / 'out/sparc64-wasm-atomics-parent.json').read_text())
subprocess.run(['python3', str(parent / 'tools/sparc64/verify-checkpoint.py'),
                str(checkpoint), '--source-root', str(parent)], check=True)
for name in ['node', 'd8', 'cctest', 'node_mksnapshot']:
    assert digest(parent / 'out/Release' / name) == parent_manifest['artifact_sha256'][name]
for source, artifact in [('config.gypi', 'config.gypi'),
                          ('out/Release/v8_build_config.json', 'v8_build_config.json')]:
    assert digest(parent / source) == parent_manifest['artifact_sha256'][artifact]
result = dict(passed=True, binary_sha256=current, matrix_checks=matrix['checks'],
              matrix_functions=matrix['nativeFunctions'], official_spec_commands=spec['commands'],
              upstream_cases=len(names), threaded=threads, parent_checkpoint=str(checkpoint))
(root / 'out/sparc64-wasm-atomics-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('WASM_ATOMICS_AUDIT_OK', flush=True)
