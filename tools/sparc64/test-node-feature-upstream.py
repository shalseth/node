#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run relevant unchanged Node feature suites with the feature actually enabled."""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--feature', choices=['amaro', 'temporal', 'ffi'], required=True)
parser.add_argument('--label', required=True)
parser.add_argument('--big-endian-controls-label')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary = root / 'out/Release/node'
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
binary_hash = digest(binary)
probe = 'JSON.stringify({amaro:process.config.variables.node_use_amaro,' \
        'ffi:process.config.variables.node_use_ffi,temporal:typeof Temporal==="object",' \
        'typescript:process.features.typescript})'
features = json.loads(subprocess.check_output([str(binary), '-p', probe], cwd=root))
assert features[args.feature] is True, features
suites = ['parallel', 'sequential', 'es-module', 'module-hooks', 'message',
          'pseudo-tty', 'test-runner', 'report', 'async-hooks', 'trace_events', 'ffi']
patterns = {
    'amaro': r'typescript|hasAmaro|node_use_amaro|stripTypeScriptTypes|strip-types',
    'temporal': r'hasTemporal|v8_enable_temporal_support|\bTemporal\.',
    'ffi': r'hasFFI|node_use_ffi|node:ffi',
}
selected = {}
for suite in suites:
    for path in sorted((root / 'test' / suite).glob('test-*')):
        if not path.is_file() or path.suffix not in ['.js', '.mjs']:
            continue
        if (args.feature == 'ffi' and suite == 'ffi') or re.search(
                patterns[args.feature], path.read_text() + path.name):
            source = str(path.relative_to(root))
            assert path.read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
            selected[source] = digest(path)
assert selected
names = [str(Path(source).relative_to('test').with_suffix('')) for source in selected]
prefix = root / ('out/sparc64-node-features-upstream-' + args.feature + '-' + args.label)
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
command = ['python3', '-u', 'tools/test.py', '--shell=' + str(binary),
           '--arch=sparc64', '-j8', '-t120', '-p', 'tap', *names]
start = time.monotonic()
with prefix.with_suffix('.tap').open('w') as log:
    try:
        code = subprocess.run(command, cwd=root, stdout=log,
                              stderr=subprocess.STDOUT, timeout=1200).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
tap = prefix.with_suffix('.tap').read_text()
cases = re.findall(r'^(not ok|ok) (\d+) ([^\s]+)([^\n]*)', tap, re.M)
seen = Counter(item[2] for item in cases)
failures = [item[2] for item in cases if item[0] != 'ok']
skipped = [dict(name=item[2], detail=item[3]) for item in cases if '# SKIP' in item[3].upper()]
feature_skips = [item for item in skipped if re.search(
    r'requires? (?:amaro|ffi|temporal)|(?:amaro|ffi|temporal) (?:is )?(?:not enabled|disabled)',
    item['detail'], re.I)]
controls = {}
controlled_names = set()
if args.feature == 'ffi' and args.big_endian_controls_label:
    for test, control in [('ffi/test-ffi-shared-buffer', 'shared-buffer'),
                          ('ffi/test-ffi-no-rx-memory', 'no-rx'),
                          ('ffi/test-ffi-dynamic-library', 'dynamic-library')]:
        report = root / ('out/sparc64-node-features-ffi-' + control + '-' +
                         args.big_endian_controls_label + '.json')
        data = json.loads(report.read_text())
        assert data['passed'] and data['binary_sha256'] == binary_hash
        assert data['original_unchanged']
        controls[test] = dict(report=str(report.relative_to(root)), sha256=digest(report))
        controlled_names.add(test)
uncovered_feature_skips = [item for item in feature_skips if item['name'] not in controlled_names]
controlled_failures = []
if failures == ['ffi/test-ffi-dynamic-library'] and 'ffi/test-ffi-dynamic-library' in controls:
    assert code == 1 and tap.count("Test failure: '") == 1
    assert "Test failure: 'dlopen resolves functions from definitions'" in tap
    assert '2 !== 0' in tap and 'actual: 2' in tap and 'expected: 0' in tap
    controlled_failures = list(failures)
assert digest(binary) == binary_hash
assert selected == {source: digest(root / source) for source in selected}
result = dict(passed=(code == 0 or bool(controlled_failures)) and bool(cases) and
              failures == controlled_failures and not uncovered_feature_skips and
              set(names).issubset(seen), binary_sha256=binary_hash, feature=args.feature,
              feature_probe=features, command=command, exit_code=code, selected=names,
              source_sha256=selected, tests=len(cases), failures=failures, upstream_skipped=skipped,
              feature_disabled_skips=feature_skips, seconds=round(time.monotonic() - start, 3))
result['big_endian_controls'] = controls
result['uncovered_feature_disabled_skips'] = uncovered_feature_skips
result['controlled_upstream_failures'] = controlled_failures
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({key: result[key] for key in ['passed', 'feature', 'tests', 'failures',
                                            'upstream_skipped', 'seconds']}), flush=True)
raise SystemExit(not result['passed'])
