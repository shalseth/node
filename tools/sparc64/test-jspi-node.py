#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run the unchanged Node JSPI conformance case in a fresh process."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/node')
parser.add_argument('--label', default='current')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
source = 'test/parallel/test-wasm.mjs'
assert (root / source).read_bytes() == subprocess.check_output(
    ['git', 'show', 'HEAD:' + source], cwd=root)
binary_hash = digest(args.binary)
command = [str(args.binary), source]
try:
    run = subprocess.run(command, cwd=root, capture_output=True, timeout=45)
    code, stdout, stderr = run.returncode, run.stdout, run.stderr
except subprocess.TimeoutExpired as error:
    code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
assert digest(args.binary) == binary_hash
prefix = root / ('out/sparc64-jspi-node-' + args.label)
prefix.with_suffix('.stdout').write_bytes(stdout)
prefix.with_suffix('.stderr').write_bytes(stderr)
result = dict(passed=code == 0, binary_sha256=binary_hash, command=command,
              exit_code=code, source=source, source_sha256=digest(root / source))
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not result['passed'])
