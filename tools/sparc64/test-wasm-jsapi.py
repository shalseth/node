#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run pinned, unmodified baseline Wasm JS API tests with V8's WPT harness."""

import hashlib
import json
from pathlib import Path
import resource
import subprocess
import tarfile
import time

root = Path(__file__).resolve().parents[2]
v8 = root / 'deps/v8'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
sha1 = (v8 / 'test/wasm-js/tests.tar.gz.sha1').read_text().strip()
archive = root / f'out/wasm-upstream-inputs/wasm-js-{sha1}.tar.gz'
assert hashlib.sha1(archive.read_bytes()).hexdigest() == sha1
source_hashes = {}
with tarfile.open(archive) as stream:
    for member in stream.getmembers():
        if not member.isfile():
            continue
        name = Path(member.name)
        assert name.parts[0] == 'tests' and '..' not in name.parts
        path = v8 / 'test/wasm-js' / name
        assert path.read_bytes() == stream.extractfile(member).read(), str(name)
        source_hashes[str(name)] = digest(path)

selected, outside_scope = [], {}
core = {'constructor', 'global', 'instance', 'memory', 'module', 'table'}
for path in sorted((v8 / 'test/wasm-js/tests').rglob('*.any.js')):
    relative = path.relative_to(v8 / 'test/wasm-js/tests')
    parts = relative.parts[1:] if relative.parts[0] == 'wpt' else relative.parts
    name = str(relative).removesuffix('.any.js')
    if parts[0] in core or parts[0] in {'interface.any.js', 'prototypes.any.js'}:
        selected.append('wasm-js/' + name)
    else:
        outside_scope[name] = ('Resource-limit stress' if parts[0] == 'limits.any.js'
                               else 'Additional Wasm proposal/API: ' + parts[0])
assert len(selected) >= 70
prefix = root / 'out/sparc64-wasm-jsapi'
binary = root / 'out/Release/d8'
before = digest(binary)
command = ['python3', '-u', 'tools/run-tests.py', '--outdir=../../out/Release',
           '--variants=default', '--no-presubmit', '-j8', '-t60', '--progress=verbose',
           '--random-seed=42', '--rerun-failures-count=0',
           '--extra-flags=--liftoff-only --no-wasm-lazy-compilation',
           '--json-test-results=' + str(prefix) + '-raw.json', *selected]
started = time.monotonic()
with prefix.with_suffix('.log').open('w') as log:
    code = subprocess.run(command, cwd=v8, stdout=log, stderr=subprocess.STDOUT,
                          timeout=600).returncode
raw = json.loads(Path(str(prefix) + '-raw.json').read_text())
passed = code == 0 and not raw['results'] and raw['test_total'] >= len(selected) - 5
assert digest(binary) == before
result = dict(passed=passed, binary_sha256=before, archive_sha1=sha1,
              archive_sha256=digest(archive), source_sha256=source_hashes,
              command=command, exit_code=code, selected=selected, outside_scope=outside_scope,
              tests=raw['test_total'], unexpected=raw['results'],
              seconds=round(time.monotonic() - started, 3))
prefix.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps({k: result[k] for k in ['passed', 'exit_code', 'tests', 'seconds']}), flush=True)
assert passed, 'See out/sparc64-wasm-jsapi.log and -raw.json'
print('WASM_JSAPI_OK', flush=True)
