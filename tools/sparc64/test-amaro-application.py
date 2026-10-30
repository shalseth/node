#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run a direct TypeScript HTTP/worker app with cold/warm caches and a negative control."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
binary = root / 'out/Release/node'
parent = root.with_name('node-sparc64-wasm-production')
checkpoint = Path((parent / 'out/sparc64-wasm-production-checkpoint-path.txt').read_text().strip())
manifest = json.loads((root / 'out/sparc64-node-features-parent.json').read_text())
assert manifest == json.loads((checkpoint / 'manifest.json').read_text())
baseline = checkpoint / 'node'
assert digest(baseline) == manifest['artifact_sha256']['node']
prefix = root / ('out/sparc64-node-features-typescript-application-' + args.label)
assert not prefix.exists(), 'Preserve previous evidence before retrying'
prefix.mkdir(parents=True)
cache = prefix / 'cache'
fixture = 'tools/sparc64/apps-typescript/main.ts'
sources = {str(p.relative_to(root)): digest(p) for p in
           sorted((root / 'tools/sparc64/apps-typescript').iterdir()) if p.is_file()}
current = digest(binary)
expected = dict(passed=True, httpRequests=24, workers=4, directTypeScript=True)
results = []
for name, exe in [('cold', binary), ('warm', binary), ('parent-negative', baseline)]:
    command = [str(exe), fixture]
    try:
        run = subprocess.run(command, cwd=root, capture_output=True, timeout=45,
                             env=dict(os.environ, NODE_COMPILE_CACHE=str(cache)))
        code, stdout, stderr = run.returncode, run.stdout, run.stderr
    except subprocess.TimeoutExpired as error:
        code, stdout, stderr = 'timeout', error.stdout or b'', error.stderr or b''
    (prefix / (name + '.stdout')).write_bytes(stdout)
    (prefix / (name + '.stderr')).write_bytes(stderr)
    markers = [json.loads(line.removeprefix('TYPESCRIPT_APPLICATION_OK '))
               for line in stdout.decode(errors='replace').splitlines()
               if line.startswith('TYPESCRIPT_APPLICATION_OK ')]
    passed = code == 0 and markers == [expected]
    if name == 'parent-negative':
        passed = code == 1 and not markers and (
            b'ERR_NO_TYPESCRIPT' in stderr or b'ERR_UNKNOWN_FILE_EXTENSION' in stderr)
    results.append(dict(name=name, command=command, exit_code=code,
                        binary_sha256=digest(exe), passed=passed, data=markers))
assert digest(binary) == current
assert sources == {source: digest(root / source) for source in sources}
cache_files = {str(p.relative_to(prefix)): digest(p) for p in cache.rglob('*') if p.is_file()}
assert cache_files
result = dict(passed=all(item['passed'] for item in results), binary_sha256=current,
              source_sha256=sources, cache_sha256=cache_files, results=results)
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not result['passed'])
