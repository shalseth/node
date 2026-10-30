#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Finish native validation after a specific live build, without restarting it."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import json
from pathlib import Path
import subprocess
import time

from node_features_validation import digest, validate_sources

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build-pid', type=int, required=True)
parser.add_argument('--label', default='math-isolated-full')
parser.add_argument('--feature-label', default='features-isolated')
parser.add_argument('--regression-label', default='production-atomic-endian')
args = parser.parse_args()
output = root / ('out/sparc64-node-features-completion-' + args.label)
output.mkdir(exist_ok=False)
record = dict(passed=False, started_utc=datetime.now(timezone.utc).isoformat(),
              build_pid=args.build_pid, results=[])


def save():
    output.with_suffix('.json').write_text(json.dumps(record, indent=2) + '\n')


def identity():
    proc = Path('/proc') / str(args.build_pid)
    try:
        if b'build-node-features.py' not in (proc / 'cmdline').read_bytes():
            return None
        return (proc / 'stat').read_text().rsplit(')', 1)[1].split()[19]
    except (FileNotFoundError, ProcessLookupError):
        return None


def step(name, command):
    start = time.monotonic()
    print('START', name, flush=True)
    with (output / (name + '.log')).open('w') as log:
        code = subprocess.run(command, cwd=root, stdout=log,
                              stderr=subprocess.STDOUT).returncode
    result = dict(name=name, command=command, exit_code=code,
                  seconds=round(time.monotonic() - start, 3))
    print(json.dumps(result), flush=True)
    return result


def script(name, *flags):
    return ['python3', '-u', 'tools/sparc64/' + name + '.py', *flags]


save()
original_identity = identity()
record['build_process_start_time'] = original_identity
save()
while original_identity is not None and identity() == original_identity:
    time.sleep(10)
record['source_changes'] = validate_sources(root)
record['binary_sha256'] = {name: digest(root / 'out/Release' / name)
                           for name in ['node', 'd8', 'inspector-test']}
for name, command in [
    ('prepare-inputs', script('prepare-node-features-acceptance')),
    ('build-inspector', script('build-wasm-production-inspector', '--label', args.regression_label)),
    ('prepare-regressions', script('validate-wasm-production', '--stage', 'prep',
                                   '--label', args.regression_label)),
]:
    result = step(name, command)
    record['results'].append(result)
    save()
    if result['exit_code'] != 0:
        raise SystemExit(1)
record['binary_sha256']['inspector-test'] = digest(root / 'out/Release/inspector-test')
save()


def retained():
    # Optimizer selections read retained case-name reports before the fresh
    # regression producers replace them; each test runs the current binary.
    results = []
    for stage in ['optimizer-regressions', 'regressions']:
        result = step(stage, script('validate-wasm-production', '--stage', stage,
                                     '--label', args.regression_label))
        results.append(result)
        if result['exit_code'] != 0:
            break
    return results


with ThreadPoolExecutor(max_workers=3) as pool:
    jobs = [pool.submit(step, 'node-features', script('validate-node-features',
                                                    '--label', args.feature_label)),
            pool.submit(step, 'wasm-features', script('validate-wasm-production',
                '--stage', 'features', '--label', args.regression_label)),
            pool.submit(retained)]
    for job in jobs:
        result = job.result()
        record['results'].extend(result if isinstance(result, list) else [result])
        save()
if any(result['exit_code'] != 0 for result in record['results']):
    raise SystemExit(1)
result = step('full-audit', script('audit-node-features', '--label', args.feature_label,
                                  '--production-label', args.regression_label))
record['results'].append(result)
save()
if result['exit_code'] != 0:
    raise SystemExit(1)
assert record['source_changes'] == validate_sources(root)
assert record['binary_sha256'] == {name: digest(root / 'out/Release' / name)
                                    for name in record['binary_sha256']}
record.update(passed=True, finished_utc=datetime.now(timezone.utc).isoformat())
save()
print('NODE_FEATURES_VALIDATION_COMPLETE_CHECKPOINT_PENDING', flush=True)
