#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Require current native Wasm/application evidence and preserved JS JIT behavior."""

import argparse
import ast
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-jspi', action='store_true')
parser.add_argument('--allow-production', action='store_true')
args = parser.parse_args()
if args.allow_production:
    args.allow_jspi = True
    from wasm_production_validation import validate_sources
    validate_sources(Path(__file__).resolve().parents[2])


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read(name):
    return json.loads((root / 'out' / name).read_text())


current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
config = read('Release/v8_build_config.json')
assert all(config[name] for name in ['has_turbofan', 'has_webassembly', 'verify_heap'])
makefile = (root / 'out/tools/v8_gypfiles/v8_base_without_compiler.target.mk').read_text()
assert all('-D' + name in makefile for name in
           ['V8_ENABLE_SPARKPLUG', 'V8_ENABLE_TURBOFAN', 'V8_ENABLE_WEBASSEMBLY'])
variables = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert all(variables[name] == 'true' for name in ['node_use_node_snapshot', 'node_use_node_code_cache'])
build = read('sparc64-wasm-build.json')
assert build and all(item['exit_code'] == 0 for item in build)
assert build[-1]['binary_sha256'] == current
changed = set()
for command in [['git', 'diff', '--name-only', 'HEAD'],
                ['git', 'ls-files', '--others', '--exclude-standard']]:
    changed.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
assert {name for name in changed if (root / name).is_file() and
        not name.startswith('tools/sparc64/')} == set(build[-1]['source_sha256'])
for name, value in build[-1]['source_sha256'].items():
    assert digest(root / name) == value, name

baseline = read('sparc64-wasm-baseline.json')
assert baseline['passed'] and baseline['binary_sha256'] == current['d8']
assert baseline['fixture_sha256'] == digest(root / 'tools/sparc64/test-wasm-baseline.js')
assert {item['name'] for item in baseline['cases']} == {
    'scalar', 'integer', 'float', 'control', 'calls', 'tables', 'stack', 'reentry', 'memory'}
assert sum(item['data']['checks'] for item in baseline['cases']) >= 1010
assert sum(item['data']['nativeFunctions'] for item in baseline['cases']) >= 33
for item in baseline['cases']:
    assert item['exit_code'] == 0 and item['data']['passed']
    raw = json.loads((root / 'out/sparc64-wasm-baseline' /
                      (item['name'] + '.stdout')).read_text().splitlines()[-1])
    assert raw == item['data']

spec = read('sparc64-wasm-spec.json')
assert spec['passed'] and spec['binary_sha256'] == current['d8']
assert len(spec['selected']) == 73 and len(spec['tests']) == 73
assert {item['name'] for item in spec['tests']} == set(spec['selected'])
assert all(item['exit_code'] == 0 for item in spec['tests'])
for name, value in spec['source_sha256'].items():
    assert digest(root / 'deps/v8/test/wasm-spec-tests/tests' / (name + '.js')) == value
jsapi = read('sparc64-wasm-jsapi.json')
assert jsapi['passed'] and jsapi['binary_sha256'] == current['d8']
assert len(jsapi['selected']) == 77 and jsapi['tests'] >= 72 and not jsapi['unexpected']
for name, value in jsapi['source_sha256'].items():
    assert digest(root / 'deps/v8/test/wasm-js' / name) == value
for suite, item in [('wasm-spec-tests', spec), ('wasm-js', jsapi)]:
    sha1 = (root / 'deps/v8/test' / suite / 'tests.tar.gz.sha1').read_text().strip()
    archive = root / 'out/wasm-upstream-inputs' / f'{suite}-{sha1}.tar.gz'
    assert item['archive_sha1'] == sha1
    assert hashlib.sha1(archive.read_bytes()).hexdigest() == sha1
    assert digest(archive) == item['archive_sha256']

integration = read('sparc64-wasm-integration.json')
assert integration['passed'] and integration['binary_sha256'] == current['node']
assert {step['name'] for step in integration['steps']} == {'parser', 'apis', 'upstream'}
for step in integration['steps']:
    assert step['exit_code'] == 0
    if step['name'] != 'upstream':
        assert read('sparc64-wasm-integration/' + step['name'] + '.stdout') == step['data']
    if step['name'] == 'parser':
        assert step['data']['checks'] >= 194 and step['data']['nativeFunctions'] == 2
    elif step['name'] == 'apis':
        assert step['data']['fetch'] and step['data']['websocket'] and step['data']['echoes'] == 3
    else:
        assert step['data']['tests'] >= 7 and not step['data']['failures']
platform = read('sparc64-wasm-platform.json')
assert platform['passed'] and platform['negative_control_reproduced']
assert len(platform['signature_controls']) == 4
assert all(item['exit_code'] == (0 if item['expected_success'] else 1)
           for item in platform['signature_controls'])
assert platform['signature_header_sha256'] == digest(root / 'deps/v8/src/codegen/signature.h')

regression = read('sparc64-wasm-js-regression.json')
assert regression['passed'] and regression['binary_sha256'] == current['d8']
assert regression['tests'] == len(regression['selected']) == 6544 and not regression['unexpected']
for name, value in regression['source_sha256'].items():
    assert digest(root / name) == value, name
expected_exclusions = {name: reason for name, reason in json.loads(
    (root / 'tools/sparc64/hardening-exclusions.json').read_text()).items()
    if 'WebAssembly' not in reason}
amaro_enabled = json.loads(subprocess.check_output(
    [str(root / 'out/Release/node'), '-p',
     'JSON.stringify(process.config.variables.node_use_amaro)'], cwd=root)) is True
if amaro_enabled:
    expected_exclusions = {name: reason for name, reason in expected_exclusions.items()
                           if 'Amaro' not in reason}
feature_exclusions = json.loads(
    (root / 'tools/sparc64/wasm-feature-exclusions.json').read_text())
assert set(feature_exclusions) == (set() if args.allow_jspi else {'parallel/test-wasm'})
if args.allow_jspi:
    jspi = read('sparc64-jspi-node-current.json')
    assert jspi['passed'] and jspi['exit_code'] == 0 and jspi['binary_sha256'] == current['node']
    assert jspi['source'] == 'test/parallel/test-wasm.mjs'
    assert jspi['source_sha256'] == digest(root / jspi['source'])
    assert (root / jspi['source']).read_bytes() == subprocess.check_output(
        ['git', 'show', 'HEAD:' + jspi['source']], cwd=root)
jspi_test = (root / 'test/parallel/test-wasm.mjs').read_text()
assert 'WebAssembly.Suspending' in jspi_test and 'WebAssembly.promising' in jspi_test
modules = read('sparc64-wasm-node-modules.json')
assert modules['passed'] and modules['binary_sha256'] == current['node']
assert modules['exit_code'] == 0 and modules['tests'] == 3
module_tap = (root / 'out/sparc64-wasm-node-modules.tap').read_text()
module_cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', module_tap, re.M)
assert len(module_cases) == 3 and all(c[0] == 'ok' and '# SKIP' not in c[3].upper() for c in module_cases)
assert {c[2] for c in module_cases} == set(modules['names'])
for name, value in modules['source_sha256'].items():
    assert digest(root / 'test' / (name + '.js')) == value
node_case_counts = {}
for group, minimum in [('broad', 5700), ('addons', 130), ('native', 90)]:
    item = read('sparc64-wasm-conformance-' + group + '.json')
    assert item['passed'] and item['binary_sha256'] == current['node']
    assert item['tests'] >= minimum and not item['failures']
    assert item['excluded'] == (expected_exclusions | feature_exclusions
                                if group == 'broad' else expected_exclusions)
    tap = (root / 'out' / ('sparc64-wasm-conformance-' + group + '.tap')).read_text()
    cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', tap, re.M)
    assert len(cases) == item['tests'] and all(c[0] == 'ok' for c in cases)
    assert int(re.search(r'^1\.\.(\d+)$', tap, re.M)[1]) == len(cases)
    names = Counter(c[2] for c in cases)
    if group == 'broad':
        for name in modules['names']:
            names[name] = max(names[name], 1)
        assert sum(names.values()) == 5831 + int(args.allow_jspi) + int(amaro_enabled)
        initial_path = root / 'out/sparc64-wasm-conformance-broad-initial.json'
        if initial_path.exists():
            initial = read(initial_path.name)
            assert initial['binary_sha256'] == current['node']
            assert initial['tests'] == 5832 and initial['failures'] == ['parallel/test-wasm']
            initial_tap = initial_path.with_suffix('.tap').read_text()
            initial_names = Counter(re.findall(r'^(?:not ok|ok) \d+ ([^\s]+)', initial_tap, re.M))
            assert initial_names.pop('parallel/test-wasm') == 1
            assert names == initial_names
    node_case_counts[group] = sum(names.values())
cctest = read('sparc64-wasm-cctest-run.json')
assert cctest['passed'] and not cctest['excluded'] and cctest['tests'] >= 281
assert cctest['node_sha256'] == current['node']
assert cctest['binary_sha256'] == digest(root / 'out/Release/cctest')
for group in ['directed', 'applications']:
    item = read('sparc64-turbofan-runtime-' + group + '.json')
    assert item['passed'] and item['binary_sha256'] == current
    assert all(step['passed'] and step['exit'] == 0 for step in item['results'])
soak = read('sparc64-turbofan-soak.json')
assert soak['passed'] and soak['milliseconds'] >= 120000 and soak['requests'] >= 6000
assert soak['handlerOptimized'] > 0 and soak['optimized'] > 0
assert len(soak['workers']) == 2 and all(item['optimized'] > 0 for item in soak['workers'])
assert len(soak['samples']) == 8
state = read('sparc64-snapshots-state.json')
assert state['passed'] and state['binary_sha256'] == current['node'] and len(state['results']) == 12
regexp = read('sparc64-regexp-upstream.json')
assert len(regexp) == 3 and all(item['passed'] and item['binary_sha256'] == current['d8'] for item in regexp)
workloads = read('sparc64-hardening-workloads.json')
assert workloads['passed'] and workloads['binary_sha256'] == current['node']
assert len(workloads['results']) == 10 and all(item['passed'] for item in workloads['results'])
for name in ['typescript-clean', 'eslint']:
    assert next(item for item in workloads['results'] if item['name'] == name)['optimizing_compilations'] > 0
flags = read('sparc64-hardening-regexp-flags.json')
assert flags['passed'] and flags['binary_sha256'] == current['node']
assert len(flags['results']) == 10 and all(item['passed'] for item in flags['results'])
platform_control = read('sparc64-hardening-platform.json')
assert platform_control['passed'] and platform_control['binary_sha256'] == current['node']
assert platform_control['native_noatime'] == platform_control['node_noatime'] == 2097152
assert platform_control['permission_test_exit'] == 0
zip_control = read('sparc64-hardening-zip.json')
assert zip_control['passed'] and zip_control['binary_sha256'] == current['node']
assert not zip_control['reference_zipinfo_preserves_unicode']

parent = root.with_name('node-sparc64-hardening')
checkpoint = Path((parent / 'out/sparc64-hardening-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == read('sparc64-wasm-parent.json') and manifest['profile'] == 'hardening'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=parent, text=True).strip() == manifest['base_head']
actual = set()
for command in [['git', 'diff', '--name-only', '--diff-filter=ACMRT', 'HEAD'],
                ['git', 'ls-files', '--others', '--exclude-standard']]:
    actual.update(subprocess.check_output(command, cwd=parent, text=True).splitlines())
assert actual == set(manifest['source_sha256'])
for name, value in manifest['source_sha256'].items():
    assert digest(parent / name) == value, name
for name in ['node', 'd8', 'cctest', 'node_mksnapshot']:
    assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name], name
assert digest(parent / 'config.gypi') == manifest['artifact_sha256']['config.gypi']
assert digest(parent / 'out/Release/v8_build_config.json') == manifest['artifact_sha256']['v8_build_config.json']
for line in (checkpoint / 'SHA256SUMS').read_text().splitlines():
    value, name = line.split('  ', 1)
    assert digest(checkpoint / name) == value, name
assert shutil.disk_usage(root).free > 1024 ** 3
result = dict(passed=True, binary_sha256=current, parent_checkpoint=str(checkpoint),
              parent_sources=len(actual),
              baseline_checks=sum(item['data']['checks'] for item in baseline['cases']),
              scalar_spec_cases=len(spec['tests']),
              jsapi_cases=jsapi['tests'], js_regression_cases=regression['tests'],
              node_case_counts=node_case_counts,
              node_case_total=sum(node_case_counts.values()))
(root / 'out/sparc64-wasm-audit.json').write_text(json.dumps(result, indent=2) + '\n')
print('WASM_EVIDENCE_AUDIT_OK', json.dumps(current), flush=True)
