#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Check the bundled Amaro Wasm parser in both native compilation tiers."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/node')
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
fixture = 'tools/sparc64/test-amaro-parser-native.cjs'
sources = ['deps/amaro/dist/index.js', 'deps/amaro/package.json']
for source in sources:
    assert (root / source).read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
before = {source: digest(root / source) for source in [fixture, *sources]}
binary_hash = digest(args.binary)
output = root / ('out/sparc64-node-features-amaro-parser-' + args.label)
assert not output.exists(), 'Preserve previous evidence before retrying'
output.mkdir(parents=True)
results = []
for tier, flags in [('liftoff', ['--liftoff-only']), ('optimized', ['--no-liftoff'])]:
    command = [str(args.binary), *flags, '--allow-natives-syntax', '--expose-gc',
               '--verify-heap', fixture]
    start = time.monotonic()
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=120,
                             env=dict(os.environ, AMARO_WASM_TIER=tier))
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (output / (tier + '.stdout')).write_bytes(stdout)
    (output / (tier + '.stderr')).write_bytes(stderr)
    markers = [json.loads(line.removeprefix('AMARO_NATIVE_PARSER_OK '))
               for line in stdout.decode(errors='replace').splitlines()
               if line.startswith('AMARO_NATIVE_PARSER_OK ')]
    data = markers[0] if len(markers) == 1 else None
    passed = (code == 0 and data is not None and data['passed'] and data['tier'] == tier and
              data['checks'] == 195 and data['calls'] == 99 and data['largeSourceBytes'] > 65536 and
              data['liftoff'] == (tier == 'liftoff') and data['optimized'] == (tier == 'optimized'))
    item = dict(tier=tier, passed=passed, command=command, exit_code=code, data=data,
                seconds=round(time.monotonic() - start, 3))
    results.append(item)
    print(json.dumps(item), flush=True)
assert digest(args.binary) == binary_hash
assert before == {source: digest(root / source) for source in before}
passed = all(item['passed'] for item in results)
if passed:
    assert len({item['data']['moduleSha256'] for item in results}) == 1
result = dict(passed=passed, binary_sha256=binary_hash, source_sha256=before, results=results)
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
raise SystemExit(not passed)
