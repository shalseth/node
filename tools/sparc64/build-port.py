#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Build a clean committed SPARC64 checkout and record its inputs and outputs."""
import argparse
import ast
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--jobs', type=int, default=96)
parser.add_argument('--prefix', default='/usr')
args = parser.parse_args()
assert args.jobs > 0
assert not subprocess.check_output(['git', 'status', '--porcelain'], cwd=root)
assert not (root / 'out/Release').exists() and not (root / 'config.gypi').exists(), 'Use a fresh worktree'
assert shutil.disk_usage(root).free > 15 * 1024**3
sha = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()


def inventory():
    result = {}
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=root).decode().split('\0')
    for name in sorted(filter(None, names)):
        path = root / name
        result[name] = {'symlink': os.readlink(path)} if path.is_symlink() else {'sha256': sha(path)}
    return result


stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%SZ')
output = root / 'out/sparc64-builds' / stamp
output.mkdir(parents=True, exist_ok=False)
head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
before = inventory()
record = dict(passed=False, git_commit=head, source_inventory=before, results=[])
env = dict(os.environ, CCACHE_DISABLE='1', CARGO_NET_OFFLINE='true')


def save():
    (output / 'build.json').write_text(json.dumps(record, indent=2) + '\n')


save()
commands = [
    ('configure', ['sh', 'tools/sparc64/configure-node-features.sh', 'all',
                   '--prefix=' + args.prefix, '--use-prefix-to-find-headers']),
    ('make', ['make', '-j' + str(args.jobs)]),
    ('test-metadata', ['python3', 'tools/sparc64/write-test-config.py',
                       '--allow-turbofan', '--allow-webassembly']),
    ('runtime', [str(root / 'out/Release/node'), '-e',
        "const a=require('node:assert/strict');a.equal(process.arch,'sparc64');"
        "a.equal(require('node:os').endianness(),'BE');a.equal(process.features.typescript,'strip');"
        "a.equal(typeof Temporal,'object');a.equal(typeof WebAssembly.Suspending,'function');"
        "a.equal(process.config.variables.node_use_ffi,true);"
        "console.log(JSON.stringify({versions:process.versions,features:process.features}))"]),
]
for name, command in commands:
    started = time.monotonic()
    with (output / (name + '.log')).open('w') as log:
        code = subprocess.run(command, cwd=root, env=env, stdout=log, stderr=subprocess.STDOUT).returncode
    result = dict(name=name, command=command, exit_code=code, seconds=time.monotonic()-started)
    record['results'].append(result)
    save()
    print(json.dumps(result), flush=True)
    if code:
        raise SystemExit(code)
assert inventory() == before, 'Tracked sources changed during the build'
assert subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip() == head
config = ast.literal_eval((root / 'config.gypi').read_text())['variables']
assert config['target_arch'] == 'sparc64'
assert config['v8_enable_verify_heap'] == config['v8_enable_temporal_support'] == 1
assert config['node_use_amaro'] == config['node_use_ffi'] == 'true'
assert config['v8_enable_maglev'] == 0
record.update(passed=True, configuration=config, binary_sha256={
    name: sha(root / 'out/Release' / name) for name in ['node', 'd8', 'cctest', 'node_mksnapshot']})
save()
print('SPARC64_BUILD_OK ' + str(output / 'build.json'), flush=True)
