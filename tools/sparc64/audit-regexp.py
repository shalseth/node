#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Audit native RegExp evidence and the preserved snapshot/TurboFan parent."""

import ast
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(name):
    return json.loads((root / 'out' / name).read_text())


current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
config = read('Release/v8_build_config.json')
assert config['has_turbofan'] and config['verify_heap']
assert not config['has_webassembly']
variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert '-DV8_ENABLE_SPARKPLUG' in (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
assert all(variables[key] == 'true' for key in ['node_use_node_snapshot', 'node_use_node_code_cache'])
build = read('sparc64-regexp-build.json')
assert len(build) == 2 and all(item['exit_code'] == 0 for item in build)
assert build[-1]['binary_sha256'] == current
for name, value in build[-1]['source_sha256'].items():
    assert digest(root / name) == value, name

upstream = read('sparc64-regexp-upstream.json')
assert {item['profile'] for item in upstream} == {'tier-up', 'native', 'native-gc', 'full'}
for item in upstream:
    assert item['passed'] and item['binary_sha256'] == current['d8']
    assert item['exit_code'] == 0 and item['unexpected'] == 0
    raw = read('sparc64-regexp-upstream-' + item['profile'] + '.json')
    assert raw['test_total'] == item['tests'] and not raw['results']
    assert item['tests'] >= (6500 if item['profile'] == 'full' else 100)

differential = read('sparc64-regexp-differential.json')
assert differential['passed'] and differential['binary_sha256'] == current['d8']
assert differential['fixture_sha256'] == digest(root / 'tools/sparc64/test-regexp-vectors.js')
assert len(differential['profiles']) == 3
for item in differential['profiles']:
    assert item['exit_code'] == 0 and item['checks'] > 50000
    assert (item['native'] == 0) if item['name'] == 'interpreter' else (item['native'] > 50000)

interrupt = read('sparc64-regexp-interrupt.json')
assert interrupt['passed'] and interrupt['binary_sha256'] == current['node']
assert len(interrupt['results']) == 3 and all(item['passed'] for item in interrupt['results'])
assert 'REGEXP_INTERRUPT_GC_NESTING_OK' in (root / 'out/sparc64-regexp-interrupt-1.log').read_text()
stack = read('sparc64-regexp-stack-growth.json')
assert stack['passed'] and stack['binary_sha256'] == current['d8']
assert stack['growth_calls'] > 0 and stack['exit_code'] == 0
assembler = (root / 'out/sparc64-regexp-assembler.log').read_text()
assert re.search(r'\[  PASSED  \] 101 tests\.', assembler)
assert 'NativeLabelOffsetAcrossBufferGrowth' in assembler

for group in ['core', 'directed', 'applications']:
    evidence = read('sparc64-turbofan-runtime-' + group + '.json')
    assert evidence['passed'] and evidence['binary_sha256'] == current
    assert evidence['results'] and all(item['passed'] for item in evidence['results'])
    for item in evidence['results']:
        assert item['exit'] == 0
        log = root / 'out' / ('sparc64-turbofan-runtime-' + item['name'] + '.log')
        assert item['marker'] in log.read_text()
core = read('sparc64-turbofan-node-core.json')
assert core['passed'] and core['tests'] >= 990 and not core['failures']
soak = read('sparc64-turbofan-soak.json')
assert soak['passed'] and soak['milliseconds'] >= 120000 and soak['requests'] >= 6000
assert soak['handlerOptimized'] > 0 and soak['optimized'] > 0
assert len(soak['workers']) == 2 and all(item['optimized'] > 0 for item in soak['workers'])
state = read('sparc64-snapshots-state.json')
assert state['passed'] and state['binary_sha256'] == current['node']
assert len(state['results']) == 12 and all(item['passed'] for item in state['results'])
benchmark = read('sparc64-regexp-benchmark.json')
assert benchmark['passed'] and benchmark['binary_sha256'] == current['node']
assert len(benchmark['samples']) == 10 and len(benchmark['summary']) == 5

parent = root.with_name('node-sparc64-snapshots')
checkpoint = Path((parent / 'out/sparc64-snapshots-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == read('sparc64-regexp-parent.json')
assert manifest['profile'] == 'snapshots'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=parent, text=True).strip() == manifest['base_head']
parent_files = set()
for command in [['git', 'diff', '--name-only', '--diff-filter=ACMRT', 'HEAD'],
                ['git', 'ls-files', '--others', '--exclude-standard']]:
    parent_files.update(subprocess.check_output(command, cwd=parent, text=True).splitlines())
assert parent_files == set(manifest['source_sha256'])
for name, value in manifest['source_sha256'].items():
    assert digest(parent / name) == value, name
for name in ['node', 'd8']:
    assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name], name
for line in (checkpoint / 'SHA256SUMS').read_text().splitlines():
    value, name = line.split('  ', 1)
    assert digest(checkpoint / name) == value, name
assert shutil.disk_usage(root).free > 1024 ** 3
result = dict(passed=True, binary_sha256=current,
              parent_checkpoint=str(checkpoint), parent_sources=len(manifest['source_sha256']),
              upstream=upstream, differential=differential['profiles'],
              benchmark=benchmark['summary'])
(root / 'out/sparc64-regexp-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('REGEXP_EVIDENCE_AUDIT_OK', json.dumps(current), flush=True)
