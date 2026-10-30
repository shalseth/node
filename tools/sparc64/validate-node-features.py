#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run all three Node feature gates against one authenticated native build."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import json
from pathlib import Path
import resource
import subprocess
import time

from node_features_validation import digest, validate_sources

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
changed = validate_sources(root)
current = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
output = root / ('out/sparc64-node-features-validation-' + args.label)
output.mkdir(exist_ok=False)
groups = {
    'amaro': [('test-amaro-parser-native', []), ('test-amaro-application', []),
              ('test-node-feature-upstream', ['--feature', 'amaro'])],
    'temporal': [('test-temporal-application', []),
                 ('test-node-feature-upstream', ['--feature', 'temporal']),
                 ('test-temporal-test262', [])],
    'ffi': [('test-ffi-shared-buffer', []),
            ('test-ffi-shared-buffer', ['--suite', 'dynamic-library']),
            ('test-ffi-no-rx', []), ('test-ffi-fast-native', []),
            ('test-node-feature-upstream', ['--feature', 'ffi',
                '--big-endian-controls-label', args.label])],
}


def run_group(item):
    group, tasks = item
    results = []
    for index, (script, flags) in enumerate(tasks):
        command = ['python3', '-u', 'tools/sparc64/' + script + '.py',
                   '--label', args.label, *flags]
        start = time.monotonic()
        with (output / (group + '-' + str(index) + '.log')).open('w') as log:
            try:
                code = subprocess.run(command, cwd=root, stdout=log,
                                      stderr=subprocess.STDOUT, timeout=2400).returncode
            except subprocess.TimeoutExpired:
                code = 'timeout'
        results.append(dict(command=command, exit_code=code,
                            seconds=round(time.monotonic() - start, 3)))
        if code != 0:
            break
    return dict(group=group, passed=len(results) == len(tasks) and
                all(step['exit_code'] == 0 for step in results), results=results)


with ThreadPoolExecutor(max_workers=3) as pool:
    results = list(pool.map(run_group, groups.items()))
assert current == {name: digest(root / 'out/Release' / name) for name in current}
assert validate_sources(root) == changed
report = dict(passed=all(item['passed'] for item in results), label=args.label,
              binary_sha256=current, source_changes=changed, results=results)
output.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
raise SystemExit(not report['passed'])
