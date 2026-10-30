#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Check real embedded cache use and restored state across fresh processes."""

import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
node = root / 'out/Release/node'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with node.open('rb') as stream:
    binary_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
results = []


def run(name, args, marker, phase=None):
    command = [str(node), *args]
    env = dict(os.environ)
    if phase:
        env['SPARC64_SNAPSHOT_PHASE'] = phase
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root, env=env, capture_output=True,
                                text=True, timeout=60)
        code, output = result.returncode, result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        code, output = 'timeout', 'Process exceeded 60 seconds\n'
    log = root / 'out' / f'sparc64-snapshots-state-{name}.log'
    log.write_text(output)
    passed = code == 0 and marker in output
    results.append(dict(name=name, command=command, phase=phase, exit=code,
                        marker=marker, passed=passed,
                        seconds=round(time.monotonic() - started, 2)))
    print(json.dumps(results[-1]), flush=True)


probe = '''
const assert = require('node:assert/strict');
const { internalBinding } = require('internal/test/binding');
for (const name of ['fs', 'http', 'https', 'crypto', 'stream', 'worker_threads'])
  require('node:' + name);
assert.equal(process.config.variables.node_use_node_snapshot, true);
assert.equal(process.config.variables.node_use_node_code_cache, true);
const expected = !process.execArgv.includes('--no-node-snapshot');
assert.equal(process.features.cached_builtins, expected);
const usage = internalBinding('builtins').getCacheUsage();
assert.equal(usage.compiledInSnapshot.length > 0, expected);
assert.equal(usage.compiledWithCache.size > 0, expected);
if (!expected) assert.ok(usage.compiledWithoutCache.size > 0);
console.log('EMBEDDED_CACHE_USAGE_OK', JSON.stringify({
  expected, inSnapshot: usage.compiledInSnapshot.length,
  withCache: usage.compiledWithCache.size, withoutCache: usage.compiledWithoutCache.size
}));
'''
run('embedded', ['--expose-internals', '-e', probe], 'EMBEDDED_CACHE_USAGE_OK')
run('fallback', ['--no-node-snapshot', '--expose-internals', '-e', probe],
    'EMBEDDED_CACHE_USAGE_OK')
flags = ['--allow-natives-syntax', '--expose-gc', '--verify-heap',
         '--trace-opt', '--trace-deopt']
blob = root / 'out/snapshot-state.blob'
blob.unlink(missing_ok=True)
run('build', [*flags, f'--snapshot-blob={blob}', '--build-snapshot',
              'tools/sparc64/snapshot-state.cjs'], 'SNAPSHOT_STATE_BUILT', 'build')
for index in range(3):
    run(f'restore-{index}', [*flags, f'--snapshot-blob={blob}'],
        'SNAPSHOT_STATE_TURBOFAN_OK', 'restore')
cache = root / 'out/sparc64-state-code.cache'
cache.unlink(missing_ok=True)
for mode in ['produce', 'consume', 'corrupt', 'source', 'version', 'flags']:
    run('cache-' + mode, [*flags, 'tools/sparc64/test-code-cache-state.cjs', mode,
                         str(cache)],
        'CODE_CACHE_PRODUCED' if mode == 'produce' else 'CODE_CACHE_STATE_OK ' + mode)
with node.open('rb') as stream:
    assert hashlib.file_digest(stream, 'sha256').hexdigest() == binary_hash
passed = all(item['passed'] for item in results)
(root / 'out/sparc64-snapshots-state.json').write_text(json.dumps(
    dict(passed=passed, binary_sha256=binary_hash, results=results), indent=2) + '\n')
raise SystemExit(not passed)
