#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate scalar Wasm atomics, unchanged upstream cases and Node workers."""

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import resource
import re
import shlex
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--group', choices=['matrix', 'spec', 'upstream', 'threads',
                                       'runtime', 'all'], default='all')
parser.add_argument('--case', action='append')
parser.add_argument('--tier', choices=['liftoff', 'optimized'], default='liftoff')
parser.add_argument('--label')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
binaries = {name: digest(root / 'out/Release' / name) for name in ['node', 'd8']}
prefix = 'sparc64-wasm-atomics' + ('-' + args.label if args.label else '')
output = root / 'out' / prefix
if args.label:
    assert not output.exists(), 'Preserve previous evidence before retrying'
output.mkdir(parents=True, exist_ok=True)
flags = [*(['--liftoff-only'] if args.tier == 'liftoff' else
           ['--no-liftoff-only', '--no-liftoff']),
         '--no-wasm-lazy-compilation', '--allow-natives-syntax']
thread_result = 'out/' + prefix + '-threads-result.json' if args.label else 'out/sparc64-wasm-threads-result.json'


def run(name, command, timeout, cwd=root, sources=()):
    before = {source: digest(root / source) for source in sources}
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=cwd, capture_output=True, timeout=timeout,
                                env=dict(os.environ, WASM_ATOMICS_TIER=args.tier,
                                         WASM_ATOMICS_RESULT=thread_result))
        stdout, stderr, code = result.stdout, result.stderr, result.returncode
    except subprocess.TimeoutExpired as error:
        stdout, stderr, code = error.stdout or b'', error.stderr or b'', 'timeout'
    (output / (name + '.stdout')).write_bytes(stdout)
    (output / (name + '.stderr')).write_bytes(stderr)
    assert all(digest(root / source) == value for source, value in before.items())
    result = dict(name=name, command=command, cwd=str(cwd), exit_code=code,
                  seconds=round(time.monotonic() - started, 3), source_sha256=before)
    if code == 0 and name in ['matrix', 'spec']:
        result['data'] = json.loads(stdout.decode().splitlines()[-1])
        assert result['data']['passed']
    if code == 0 and name == 'threads':
        assert b'WASM_THREADS_OK' in stdout
        result['data'] = json.loads((root / thread_result).read_text())
        assert result['data']['passed']
    print(json.dumps(result), flush=True)
    return result


def group(name):
    if name == 'matrix':
        fixture = 'tools/sparc64/test-wasm-atomics.js'
        results = [run('matrix', [str(root / 'out/Release/d8'), *flags,
                                 '--expose-gc', '../../' + fixture, '--', args.tier], 60,
                       root / 'deps/v8', [fixture])]
    elif name == 'spec':
        fixture = 'tools/sparc64/run-wasm-atomic-spec.js'
        inputs = root / 'out/wasm-threads-spec'
        manifest = json.loads((inputs / 'source-manifest.json').read_text())
        assert digest(inputs / 'threads.tar.gz') == manifest['archive_sha256']
        assert all(digest(inputs / source) == value for source, value in manifest['sources'].items())
        generated = json.loads((inputs / 'translation-manifest.json').read_text())
        assert all(digest(inputs / path) == value for path, value in generated['files'].items())
        results = [run('spec', [str(root / 'out/Release/d8'), *flags, fixture,
                               '--', 'out/wasm-threads-spec/atomic.json', args.tier], 60, sources=[fixture])]
    elif name == 'threads':
        subprocess.run(['python3', 'tools/sparc64/verify-wasm-threaded-oracle.py'], cwd=root, check=True)
        fixture = 'tools/sparc64/test-wasm-threads.cjs'
        results = [run('threads', [str(root / 'out/Release/node'), *flags,
                                   '--expose-gc', '--verify-heap', fixture], 90,
                       sources=[fixture, 'tools/sparc64/wasm-threaded-hash.c',
                                'deps/zstd/lib/common/xxhash.h'])]
    else:
        names = args.case or ['atomic-alignment-checks', 'atomics-non-shared',
            'compare-exchange-stress', 'compare-exchange64-stress',
            'shared-memory', 'grow-shared-memory', 'grow-shared-memory-resizable-buffer',
            'shared-memory-worker-simple-gc', 'shared-memory-worker-stress',
            'shared-memory-worker-gc', 'shared-memory-worker-gc-stress',
            'shared-memory-worker-explicit-gc-stress', 'shared-memory-gc-stress',
            'shared-arraybuffer-worker-simple-gc']
        def one(case):
            fixture = 'deps/v8/test/mjsunit/wasm/' + case + '.js'
            original = subprocess.check_output(['git', 'show', 'HEAD:' + fixture], cwd=root)
            assert (root / fixture).read_bytes() == original, fixture
            upstream_flags = [flag for line in re.findall(r'^// Flags: (.*)$', original.decode(), re.M)
                              for flag in shlex.split(line)]
            return run('upstream-' + case, [str(root / 'out/Release/d8'), *flags, *upstream_flags,
                '--expose-gc', 'test/mjsunit/mjsunit.js', 'test/mjsunit/wasm/' + case + '.js'],
                120, root / 'deps/v8', [fixture, 'deps/v8/test/mjsunit/mjsunit.js',
                                       'deps/v8/test/mjsunit/wasm/wasm-module-builder.js'])
        with ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(one, names))
    evidence = dict(passed=all(item['exit_code'] == 0 for item in results),
                    binary_sha256=binaries, group=name, tier=args.tier, cases=results)
    (root / 'out' / (prefix + '-' + name + '.json')).write_text(
        json.dumps(evidence, indent=2) + '\n')
    assert all(digest(root / 'out/Release' / key) == value for key, value in binaries.items())
    assert evidence['passed'], 'See scalar atomic validation logs'


selected = (['matrix', 'spec', 'upstream', 'threads'] if args.group == 'all' else
            ['matrix', 'spec', 'threads'] if args.group == 'runtime' else [args.group])
for name in selected:
    group(name)
print('WASM_ATOMICS_OK', flush=True)
