#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Validate distinct SIMD debugger values before and after GC while paused."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--binary', type=Path, default=root / 'out/Release/inspector-test')
parser.add_argument('--label', default='current')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
fixture = 'tools/sparc64/test-wasm-debug-simd.js'
binary_hash = digest(args.binary)
prefix = root / ('out/sparc64-wasm-debug-simd-' + args.label)
assert not prefix.with_suffix('.json').exists(), 'Preserve previous evidence before retrying'
command = [str(args.binary), 'test/inspector/protocol-test.js', str(root / fixture),
           '--liftoff-only', '--no-wasm-lazy-compilation']
try:
    run = subprocess.run(command, cwd=root / 'deps/v8', capture_output=True, timeout=45)
    code, stdout, stderr = run.returncode, run.stdout, run.stderr
except subprocess.TimeoutExpired as error:
    code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
prefix.with_suffix('.stdout').write_bytes(stdout)
prefix.with_suffix('.stderr').write_bytes(stderr)
lines = [line.removeprefix('WASM_DEBUG_SIMD_OK ') for line in stdout.decode(errors='replace').splitlines()
         if line.startswith('WASM_DEBUG_SIMD_OK ')]
data = json.loads(lines[0]) if len(lines) == 1 else None
expected = dict(passed=True, vectors=20, checks=40, gcWhilePaused=True, checksum=942014689)
assert digest(args.binary) == binary_hash
result = dict(passed=code == 0 and data == expected, command=command, exit_code=code,
              binary_sha256=binary_hash, source_sha256={fixture: digest(root / fixture)}, data=data)
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not result['passed'])
