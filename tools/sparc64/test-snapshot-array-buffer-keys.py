#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Check snapshot detach-key identity, protection and view updates after GC."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/node')
parser.add_argument('--label', choices=['baseline', 'current'], default='current')
args = parser.parse_args()
node = args.binary.resolve()
digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
binary_hash = digest(node)
fixture = Path('tools/sparc64/snapshot-array-buffer-keys.cjs')
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
output = root / 'out' / ('sparc64-snapshot-array-buffer-keys-' + args.label)
output.mkdir(parents=True, exist_ok=True)
results = []
for tracking in [False, True]:
    for verify in [False, True]:
        name = f'track-{int(tracking)}-verify-{int(verify)}'
        blob = output / (name + '.blob')
        blob.unlink(missing_ok=True)
        flags = ['--allow-natives-syntax', '--expose-gc',
                 '--track-array-buffer-views' if tracking else '--no-track-array-buffer-views']
        if verify:
            flags += ['--verify-heap', '--compact-on-every-full-gc']
        for phase in ['build', 'restore-0', 'restore-1']:
            command = [str(node), *flags, '--snapshot-blob=' + str(blob)]
            if phase == 'build':
                command += ['--build-snapshot', str(fixture)]
            try:
                run = subprocess.run(command, cwd=root, capture_output=True, timeout=60)
                code, log = run.returncode, run.stdout + run.stderr
            except subprocess.TimeoutExpired as error:
                code = 'timeout'
                log = (error.stdout or b'') + (error.stderr or b'')
            (output / (name + '-' + phase + '.log')).write_bytes(log)
            marker = b'SNAPSHOT_ARRAY_BUFFER_KEYS_BUILT 27' if phase == 'build' else b'SNAPSHOT_ARRAY_BUFFER_KEYS_OK 27'
            item = dict(tracking=tracking, verify_heap=verify, phase=phase,
                        command=command, exit_code=code, passed=code == 0 and marker in log)
            results.append(item)
            print(json.dumps(item), flush=True)
            if phase == 'build' and not item['passed']:
                break
assert digest(node) == binary_hash
passed = len(results) == 12 and all(item['passed'] for item in results)
output.with_suffix('.json').write_text(json.dumps(dict(
    passed=passed, binary_sha256=binary_hash, fixture=str(fixture),
    fixture_sha256=digest(root / fixture), cases=27, results=results), indent=2) + '\n')
raise SystemExit(not passed)
