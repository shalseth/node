#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Rerun the preserved ordinary-JavaScript test selection with Wasm enabled."""

import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--build', type=Path, default=root / 'out/Release')
parser.add_argument('--label')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
parent = root.with_name('node-sparc64-regexp')
checkpoint = Path((parent / 'out/sparc64-regexp-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
log_name = 'validation/sparc64-regexp-upstream-full.log'
log = checkpoint / log_name
assert digest(log) == manifest['artifact_sha256'][log_name]
selected = re.findall(r'^(mjsunit/\S+) default: PASS$', log.read_text(), re.M)
assert len(selected) >= 6500 and len(selected) == len(set(selected))
config = json.loads((args.build / 'v8_build_config.json').read_text())
assert config['has_webassembly'] and config['has_turbofan'] and config['verify_heap']
source_hashes = {}
for name in selected:
    candidates = ['deps/v8/test/' + name + suffix for suffix in ['.js', '.mjs']]
    matches = [source for source in candidates if (root / source).is_file()]
    assert len(matches) == 1, name
    source = matches[0]
    assert (root / source).read_bytes() == (parent / source).read_bytes(), source
    source_hashes[source] = digest(root / source)
binary = args.build / 'd8'
before = digest(binary)
prefix = root / ('out/sparc64-wasm-js-regression' + ('-' + args.label if args.label else ''))
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=' + str(args.build),
           '--variants=default', '--no-presubmit', '-j16', '-t60', '--progress=verbose',
           '--random-seed=42', '--rerun-failures-count=0', '--exit-after-n-failures=10',
           '--json-test-results=' + str(prefix) + '-raw.json', *selected]
started = time.monotonic()
with prefix.with_suffix('.log').open('w') as stream:
    code = subprocess.run(command, cwd=root / 'deps/v8', stdout=stream,
                          stderr=subprocess.STDOUT, timeout=1200).returncode
raw = json.loads(Path(str(prefix) + '-raw.json').read_text())
assert digest(binary) == before
passed = code == 0 and not raw['results'] and raw['test_total'] == len(selected)
result = dict(passed=passed, binary_sha256=before, command=command, exit_code=code,
              parent_checkpoint=str(checkpoint), parent_selection_sha256=digest(log),
              selected=selected, source_sha256=source_hashes, tests=raw['test_total'],
              unexpected=raw['results'], seconds=round(time.monotonic() - started, 3))
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: result[k] for k in ['passed', 'tests', 'exit_code', 'seconds']}), flush=True)
assert passed, 'See ordinary JavaScript regression logs'
print('WASM_JS_REGRESSION_OK', flush=True)
