#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Validate calendar scheduling across DST boundaries and worker isolates."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
binary = root / 'out/Release/node'
binary_hash = digest(binary)
source = 'tools/sparc64/test-temporal-application.cjs'
source_hash = digest(root / source)
prefix = root / ('out/sparc64-node-features-temporal-application-' + args.label)
prefix.mkdir(exist_ok=False)
profiles = {'normal': [], 'compaction': ['--verify-heap', '--stress-compaction']}
results = []
for name, flags in profiles.items():
    command = [str(binary), *flags, source]
    with (prefix / (name + '.stdout')).open('w') as stdout, \
         (prefix / (name + '.stderr')).open('w') as stderr:
        try:
            code = subprocess.run(command, cwd=root, stdout=stdout, stderr=stderr,
                                  timeout=120).returncode
        except subprocess.TimeoutExpired:
            code = 'timeout'
    output = (prefix / (name + '.stdout')).read_text()
    try:
        value = json.loads(output)
    except ValueError:
        value = None
    expected = dict(passed=True, zone='Europe/Oslo', workers=4, checks=115)
    results.append(dict(profile=name, command=command, exit_code=code,
                        passed=code == 0 and value == expected, result=value))
assert digest(binary) == binary_hash and digest(root / source) == source_hash
report = dict(passed=all(item['passed'] for item in results),
              binary_sha256=binary_hash, source=source, source_sha256=source_hash,
              results=results)
prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
raise SystemExit(not report['passed'])
