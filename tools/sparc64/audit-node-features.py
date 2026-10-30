#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Gate Amaro, Temporal, FFI and the complete retained regression chain."""
import argparse
import json
from pathlib import Path
import subprocess

from node_features_validation import digest, validate_sources, PARENT_MANIFEST_SHA256

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', default='features-isolated')
parser.add_argument('--production-label', default='production-atomic-endian')
parser.add_argument('--features-only', action='store_true')
args = parser.parse_args()
changed = validate_sources(root)
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
evidence = {}


def read(name, binary='node'):
    path = root / 'out' / name
    data = json.loads(path.read_text())
    assert data['passed'], name
    assert data['binary_sha256'] == (current if binary is None else current[binary]), name
    evidence[name] = digest(path)
    return data


def original(name, expected):
    path = root / name
    assert digest(path) == expected
    assert path.read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)


label = args.label
summary = read('sparc64-node-features-validation-' + label + '.json', None)
assert {item['group']: len(item['results']) for item in summary['results']} == {
    'amaro': 3, 'temporal': 3, 'ffi': 5}
assert all(item['passed'] and all(step['exit_code'] == 0 for step in item['results'])
           for item in summary['results'])
assert summary['source_changes'] == changed
heap_proof_path = root / 'out/sparc64-node-features-heap-configuration-proof.json'
heap_proof = json.loads(heap_proof_path.read_text())
assert heap_proof['passed'] and heap_proof['heap_verification_persisted']
assert heap_proof['defines_before'] == heap_proof['defines_after']
assert 'VERIFY_HEAP' in heap_proof['defines_after']
assert digest(root / 'config.gypi') == heap_proof['configuration_sha256']
evidence[heap_proof_path.name] = digest(heap_proof_path)
for feature, count in [('amaro', 39), ('temporal', 3), ('ffi', 31)]:
    data = read('sparc64-node-features-upstream-' + feature + '-' + label + '.json')
    assert data['feature'] == feature and data['tests'] == count
    assert not data['uncovered_feature_disabled_skips']
    assert data['feature_probe'][feature] is True
    for name, value in data['source_sha256'].items():
        original(name, value)
    if feature != 'ffi':
        assert data['exit_code'] == 0 and not data['failures'] and not data['upstream_skipped']
    else:
        assert data['failures'] == data['controlled_upstream_failures'] == ['ffi/test-ffi-dynamic-library']
        assert {item['name'] for item in data['upstream_skipped']} == {
            'parallel/test-ffi-missing-build', 'ffi/test-ffi-no-rx-memory', 'ffi/test-ffi-shared-buffer'}
        assert set(data['big_endian_controls']) == {
            'ffi/test-ffi-no-rx-memory', 'ffi/test-ffi-shared-buffer', 'ffi/test-ffi-dynamic-library'}
        for control in data['big_endian_controls'].values():
            assert digest(root / control['report']) == control['sha256']

parser_proof = read('sparc64-node-features-amaro-parser-' + label + '.json')
assert [item['tier'] for item in parser_proof['results']] == ['liftoff', 'optimized']
for item in parser_proof['results']:
    assert item['passed'] and item['exit_code'] == 0
    assert item['data']['checks'] == 195 and item['data']['calls'] == 99
    assert item['data']['moduleSha256'] == '79c79a0be6c92c7d8a1968cb1a7bed88737755d38d2b47ddfe4da5e51796a4e9'
for name, value in parser_proof['source_sha256'].items():
    assert digest(root / name) == value
typescript = read('sparc64-node-features-typescript-application-' + label + '.json')
assert [item['name'] for item in typescript['results']] == ['cold', 'warm', 'parent-negative']
assert all(item['passed'] for item in typescript['results'])
for name, value in typescript['source_sha256'].items():
    assert digest(root / name) == value
for name, value in typescript['cache_sha256'].items():
    assert digest(root / 'out' / ('sparc64-node-features-typescript-application-' + label) / name) == value
temporal = read('sparc64-node-features-temporal-application-' + label + '.json')
assert digest(root / temporal['source']) == temporal['source_sha256']
assert [item['profile'] for item in temporal['results']] == ['normal', 'compaction']
assert all(item['passed'] and item['result']['checks'] == 115 and
           item['result']['workers'] == 4 for item in temporal['results'])
temporal = read('sparc64-node-features-temporal-test262-' + label + '.json', 'd8')
assert temporal['tests'] == temporal['expected_tests'] == 13056
assert len(temporal['source_sha256']) == len(temporal['selected']) == 6534
assert temporal['upstream_expectations_unchanged'] and not temporal['unexpected']
raw = json.loads((root / 'out' / ('sparc64-node-features-temporal-test262-' + label + '-raw.json')).read_text())
assert raw['test_total'] == 13056 and not raw['results']
for name, value in temporal['source_sha256'].items():
    assert digest(root / 'deps/v8/test/test262/data/test' / name) == value
for name, value in temporal['support_sha256'].items():
    original('deps/v8/test/test262/' + name, value)
cargo_path = root / 'out/sparc64-node-features-temporal-cargo.json'
cargo = json.loads(cargo_path.read_text())
assert cargo['passed'] and cargo['elf_machine'] == 'SPARCV9' and cargo['byte_order'] == 'big'
assert digest(root / cargo['library']) == cargo['library_sha256']
for name, value in cargo['source_sha256'].items():
    original(name, value)
evidence[cargo_path.name] = digest(cargo_path)
isolation_path = root / 'out/Release/obj/gen/release/libnode_crates.a.sparc64.a.json'
isolation = json.loads(isolation_path.read_text())
assert isolation['passed'] and isolation['raw_sha256'] == cargo['library_sha256']
assert {'ceil', 'ceilf', 'floor', 'floorf', 'trunc', 'truncf'} <= set(isolation['localized_math_symbols'])
assert digest(root / 'out/Release/obj/gen/release/libnode_crates.a.sparc64.a') == isolation['isolated_sha256']
assert digest(root / 'deps/crates/isolate_libm.py') == isolation['script_sha256']
assert digest(Path(isolation['platform_libm'])) == isolation['platform_libm_sha256']
evidence[str(isolation_path.relative_to(root / 'out'))] = digest(isolation_path)

for suite, before, after in [
    ('shared-buffer', "if (endianness() === 'BE') {\n  common.skip('shared-buffer FFI is disabled on big-endian hosts');\n}\n", ''),
    ('dynamic-library', "endianness() === 'BE' ? 0 : 2", '2'),
]:
    data = read('sparc64-node-features-ffi-' + suite + '-' + label + '.json')
    original(data['source'], data['source_sha256'])
    assert data['correction_from'] == before and data['correction_to'] == after
    source = (root / data['source']).read_bytes()
    assert source.count(before.encode()) == 1
    assert (root / data['derived_source']).read_bytes() == source.replace(before.encode(), after.encode(), 1)
    assert digest(root / data['derived_source']) == data['derived_sha256']
    assert data['counts']['tests'] == data['counts']['pass'] > 0
    assert data['counts']['fail'] == data['counts']['skipped'] == 0
    if suite == 'shared-buffer':
        assert data['counts']['tests'] == 31
no_rx = read('sparc64-node-features-ffi-no-rx-' + label + '.json')
for name, value in no_rx['source_sha256'].items():
    original(name, value)
for name, value in no_rx['derived_sha256'].items():
    assert digest(root / name) == value
original_c = (root / 'test/ffi/fixture_library/deny_executable_memory.c').read_bytes()
derived_c = root / 'out' / ('sparc64-node-features-ffi-no-rx-' + label) / 'deny_executable_memory.c'
assert derived_c.read_bytes() == original_c.replace(
    b'offsetof(struct seccomp_data, args[2])', b'offsetof(struct seccomp_data, args[2]) + 4', 1)
test_body = (root / 'test/ffi/test-ffi-no-rx-memory.js').read_bytes()
for edit in no_rx['replacements']:
    assert test_body.count(edit['before'].encode()) == 1
    test_body = test_body.replace(edit['before'].encode(), edit['after'].encode(), 1)
derived_js = root / 'out' / ('sparc64-node-features-ffi-no-rx-' + label) / 'ffi/test-no-rx.cjs'
assert derived_js.read_bytes() == test_body and no_rx['exit_code'] == 0
assert digest(derived_c.parent / 'ffi_test_library.so') == no_rx['library_sha256']
control_path = root / 'out/sparc64-node-features-ffi-shared-buffer-disabled-control-tap.json'
control = json.loads(control_path.read_text())
assert control['expected_disabled'] and not control['passed'] and control['exit_code'] == 1
assert control['counts']['fail'] > 0
assert digest(root / 'out/node-features-attempts/all-first-built/node') == control['binary_sha256']
original(control['source'], control['source_sha256'])
assert digest(root / control['derived_source']) == control['derived_sha256']
evidence[control_path.name] = digest(control_path)
fast = read('sparc64-node-features-ffi-fast-native-' + label + '.json')
for name, value in fast['source_sha256'].items():
    assert digest(root / name) == value
assert len(fast['fast_api_graphs']) >= 2
for name, value in fast['graph_sha256'].items():
    assert digest(root / name) == value and 'FastApiCall' in (root / name).read_text()
stdout = root / 'out' / ('sparc64-node-features-ffi-fast-native-' + label) / 'stdout'
markers = [json.loads(line.removeprefix('FFI_FAST_NATIVE_OK ')) for line in stdout.read_text().splitlines()
           if line.startswith('FFI_FAST_NATIVE_OK ')]
assert markers == [dict(passed=True, calls=128, eightArguments=True, publicWrapper=True,
                        nativeJitReturnAddress=True, gc=True, closedLibrary=True)]
if not args.features_only:
    subprocess.run(['python3', 'tools/sparc64/audit-wasm-production.py',
                    '--label', args.production_label], cwd=root, check=True)
    parent = root.with_name('node-sparc64-wasm-production')
    checkpoint = parent / 'out/checkpoints/node-wasm-production-20261029T234057.721541Z'
    assert digest(checkpoint / 'manifest.json') == PARENT_MANIFEST_SHA256
    subprocess.run(['python3', 'tools/sparc64/verify-checkpoint.py', str(checkpoint),
                    '--source-root', str(parent)], cwd=root, check=True)
    manifest = json.loads((checkpoint / 'manifest.json').read_text())
    for name in ['node', 'd8', 'cctest', 'node_mksnapshot', 'inspector-test']:
        assert digest(parent / 'out/Release' / name) == manifest['artifact_sha256'][name]
    for name, artifact in [('config.gypi', 'config.gypi'),
                           ('out/Release/v8_build_config.json', 'v8_build_config.json')]:
        assert digest(parent / name) == manifest['artifact_sha256'][artifact]
report = dict(passed=True, label=label, binary_sha256=current, source_changes=changed,
              parent_manifest_sha256=PARENT_MANIFEST_SHA256, evidence_sha256=evidence,
              inherited_regressions_verified=not args.features_only,
              fixture_limitations=['Upstream FFI BE skip guards and disabled-path arity expectation; '
                                   'unchanged outcomes retained, separately derived controls pass'])
target = 'sparc64-node-features-' + ('feature-audit' if args.features_only else 'audit') + '.json'
(root / 'out' / target).write_text(json.dumps(report, indent=2) + '\n')
print('NODE_FEATURES_AUDIT_OK', flush=True)
