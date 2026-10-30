#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Compare native regexp results with the interpreter in fresh processes."""

import argparse
import gzip
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', default=str(root / 'out/Release/d8'))
args = parser.parse_args()
binary = Path(args.binary).resolve()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
fixture = root / 'tools/sparc64/test-regexp-vectors.js'
output = root / 'out/sparc64-regexp-differential'
output.mkdir(parents=True, exist_ok=True)
evidence = {'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'fixture_sha256': hashlib.sha256(fixture.read_bytes()).hexdigest(),
            'profiles': [], 'passed': False}
values = []
for name, flags in [('interpreter', ['--regexp-interpret-all']),
                    ('native', ['--no-regexp-tier-up']),
                    ('tier-up', [])]:
    command = [str(binary), '--allow-natives-syntax', *flags, str(fixture)]
    start = time.monotonic()
    result = subprocess.run(command, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=180)
    with gzip.open(output / f'{name}.json.gz', 'wb') as stream:
        stream.write(result.stdout)
    (output / f'{name}.stderr').write_bytes(result.stderr)
    data = json.loads(result.stdout) if result.returncode == 0 else {}
    item = dict(name=name, command=command, exit_code=result.returncode,
                seconds=round(time.monotonic() - start, 3),
                checks=data.get('checks', 0), native=data.get('native', 0))
    evidence['profiles'].append(item)
    (root / 'out/sparc64-regexp-differential.json').write_text(json.dumps(evidence, indent=2) + '\n')
    print(json.dumps(item), flush=True)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
    assert item['checks'] > 50000
    assert (item['native'] == 0) if name == 'interpreter' else (item['native'] > 50000)
    values.append(data['results'])
    if len(values) > 1:
        for index, (expected, actual) in enumerate(zip(values[0], values[-1], strict=True)):
            assert actual == expected, (name, index, expected, actual)
evidence['passed'] = True
assert hashlib.sha256(binary.read_bytes()).hexdigest() == evidence['binary_sha256']
(root / 'out/sparc64-regexp-differential.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('REGEXP_DIFFERENTIAL_OK', flush=True)
