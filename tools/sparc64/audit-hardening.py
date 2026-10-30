#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Audit hardening fixes, conformance, workloads and preserved parent evidence."""

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
assert config['has_turbofan'] and config['verify_heap'] and not config['has_webassembly']
assert '-DV8_ENABLE_SPARKPLUG' in (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert all(variables[key] == 'true' for key in ['node_use_node_snapshot', 'node_use_node_code_cache'])
build = read('sparc64-hardening-build.json')
assert len(build) == 2 and all(item['exit_code'] == 0 for item in build)
assert build[-1]['binary_sha256'] == current
for name, value in build[-1]['source_sha256'].items():
    assert digest(root / name) == value, name

excluded = json.loads((root / 'tools/sparc64/hardening-exclusions.json').read_text())
assert len(excluded) == 45
conformance = []
for group, minimum in [('broad', 5700), ('addons', 130), ('native', 90)]:
    item = read('sparc64-hardening-conformance-' + group + '.json')
    assert item['passed'] and item['binary_sha256'] == current['node']
    assert item['tests'] >= minimum and item['exit_code'] == 0 and not item['failures']
    assert item['excluded'] == excluded and len(item['affinity']) == 16
    tap = (root / 'out' / ('sparc64-hardening-conformance-' + group + '.tap')).read_text()
    cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', tap, re.M)
    assert len(cases) == item['tests'] and all(case[0] == 'ok' for case in cases)
    assert int(re.search(r'^1\.\.(\d+)$', tap, re.M)[1]) == len(cases)
    conformance.append(item)
cctest = read('sparc64-hardening-cctest-run.json')
assert cctest['passed'] and cctest['node_sha256'] == current['node']
assert cctest['binary_sha256'] == digest(root / 'out/Release/cctest')
assert cctest['tests'] >= 280 and cctest['failures'] == 0
raw_cctest = read('sparc64-hardening-cctest.json')
assert raw_cctest['tests'] == cctest['tests'] and raw_cctest['failures'] == 0

flags = read('sparc64-hardening-regexp-flags.json')
assert flags['passed'] and flags['binary_sha256'] == current['node']
assert len(flags['results']) == 10 and all(item['passed'] for item in flags['results'])
inspector = read('sparc64-hardening-inspector-strings.json')
assert {item['name'] for item in inspector} == {'fixed', 'parent', 'protocol'}
assert all(item['passed'] for item in inspector)
assert next(item for item in inspector if item['name'] == 'parent')['exit_code'] == -10
assert next(item for item in inspector if item['name'] == 'protocol')['binary_sha256'] == current['node']
assert 'INSPECTOR_UTF16_ROUNDTRIP_OK 256' in (root / 'out/sparc64-inspector-strings/protocol.log').read_text()
platform = read('sparc64-hardening-platform.json')
assert platform['passed'] and platform['binary_sha256'] == current['node']
assert platform['native_noatime'] == platform['node_noatime'] == 2097152
assert platform['permission_test_exit'] == 0
assert platform['permission_test_source_sha256'] == digest(root / 'test/parallel' / platform['permission_test'])
zip_test = read('sparc64-hardening-zip.json')
assert zip_test['passed'] and zip_test['binary_sha256'] == current['node']
assert not zip_test['reference_zipinfo_preserves_unicode']
assert all(item['unicode_ok'] for item in zip_test['results'] if item['tool'] == 'bsdtar')

workloads = read('sparc64-hardening-workloads.json')
assert workloads['passed'] and workloads['binary_sha256'] == current['node']
assert workloads['versions'] == {'typescript': '6.0.3', 'eslint': '10.13.0'}
assert workloads['expected']['source_files'] == 65 and workloads['expected']['checksum'] == 2207808
assert len(workloads['results']) == 10 and all(item['passed'] for item in workloads['results'])
for name in ['typescript-clean', 'eslint']:
    assert next(item for item in workloads['results'] if item['name'] == name)['optimizing_compilations'] > 0
assert workloads['lockfile_sha256'] == digest(root / 'tools/sparc64/workloads/package-lock.json')

state = read('sparc64-snapshots-state.json')
assert state['passed'] and state['binary_sha256'] == current['node'] and len(state['results']) == 12
regexp = read('sparc64-regexp-differential.json')
assert regexp['passed'] and regexp['binary_sha256'] == current['d8']
assert regexp['fixture_sha256'] == digest(root / 'tools/sparc64/test-regexp-vectors.js')
assert len(regexp['profiles']) == 3
for item in regexp['profiles']:
    assert item['checks'] == 70568 and item['exit_code'] == 0
    assert item['native'] == (0 if item['name'] == 'interpreter' else 70568)
upstream = read('sparc64-regexp-upstream.json')
assert len(upstream) == 3 and all(item['passed'] and item['binary_sha256'] == current['d8'] for item in upstream)
interrupt = read('sparc64-regexp-interrupt.json')
assert interrupt['passed'] and interrupt['binary_sha256'] == current['node']
for group in ['directed', 'applications']:
    evidence = read('sparc64-turbofan-runtime-' + group + '.json')
    assert evidence['passed'] and evidence['binary_sha256'] == current
    for item in evidence['results']:
        assert item['passed'] and item['exit'] == 0
        assert item['marker'] in (root / 'out' / ('sparc64-turbofan-runtime-' + item['name'] + '.log')).read_text()
soak = read('sparc64-turbofan-soak.json')
assert soak['passed'] and soak['milliseconds'] >= 120000 and soak['requests'] >= 6000
assert soak['handlerOptimized'] > 0 and soak['optimized'] > 0
assert len(soak['workers']) == 2 and all(item['optimized'] > 0 for item in soak['workers'])

parent = root.with_name('node-sparc64-regexp')
checkpoint = Path((parent / 'out/sparc64-regexp-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == read('sparc64-hardening-parent.json') and manifest['profile'] == 'regexp'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=parent, text=True).strip() == manifest['base_head']
actual = set()
for command in [['git', 'diff', '--name-only', '--diff-filter=ACMRT', 'HEAD'],
                ['git', 'ls-files', '--others', '--exclude-standard']]:
    actual.update(subprocess.check_output(command, cwd=parent, text=True).splitlines())
assert actual == set(manifest['source_sha256'])
for name, value in manifest['source_sha256'].items():
    assert digest(parent / name) == value, name
for name in ['node', 'd8']:
    assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name], name
for line in (checkpoint / 'SHA256SUMS').read_text().splitlines():
    value, name = line.split('  ', 1)
    assert digest(checkpoint / name) == value, name
assert shutil.disk_usage(root).free > 1024 ** 3
result = dict(passed=True, binary_sha256=current, conformance=conformance,
              cctest=cctest, workloads=workloads, parent_sources=len(actual),
              parent_checkpoint=str(checkpoint))
(root / 'out/sparc64-hardening-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('HARDENING_EVIDENCE_AUDIT_OK', json.dumps(current), flush=True)
