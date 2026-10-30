#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run unmodified, hash-verified official scalar Wasm specification tests."""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import tarfile
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--case', action='append', help='Select a specification case; repeatable')
parser.add_argument('--jobs', type=int, default=8)
args = parser.parse_args()
assert 1 <= args.jobs <= 16
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
names = '''address align binary binary-leb128 block br br_if br_table call call_indirect
const conversions custom data elem endianness exports f32 f32_bitwise f32_cmp f64
f64_bitwise f64_cmp fac float_exprs float_literals float_memory float_misc forward
func func_ptrs global i32 i64 if imports instance int_exprs int_literals labels
left-to-right linking load local_get local_init local_set local_tee loop memory
memory_grow memory_redundancy memory_size memory_trap names nop return select
skip-stack-guard-page stack start store switch token traps type unreachable
unreached-invalid unreached-valid unwind utf8-custom-section-id utf8-import-field
utf8-import-module utf8-invalid-encoding'''.split()
selected = args.case or names
assert all(name in names for name in selected)
binary = root / 'out/Release/d8'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
output = root / 'out/sparc64-wasm-spec'
output.mkdir(parents=True, exist_ok=True)
sha1 = (root / 'deps/v8/test/wasm-spec-tests/tests.tar.gz.sha1').read_text().strip()
archive = root / f'out/wasm-upstream-inputs/wasm-spec-tests-{sha1}.tar.gz'
assert hashlib.sha1(archive.read_bytes()).hexdigest() == sha1
sources = {}
with tarfile.open(archive) as stream:
    for name in selected:
        path = root / f'deps/v8/test/wasm-spec-tests/tests/{name}.js'
        original = stream.extractfile(f'tests/{name}.js').read()
        assert path.read_bytes() == original, f'Upstream test changed: {name}'
        sources[name] = digest(path)
evidence = dict(passed=False, binary_sha256=digest(binary), archive_sha1=sha1,
                archive_sha256=digest(archive), source_sha256=sources,
                selected=selected, tests=[])

def run(name):
    fixture = root / f'deps/v8/test/wasm-spec-tests/tests/{name}.js'
    command = [str(binary), '--liftoff-only', '--no-wasm-lazy-compilation', str(fixture)]
    started = time.monotonic()
    try:
        result = subprocess.run(command, capture_output=True, timeout=60)
        stdout, stderr = result.stdout, result.stderr
        item = dict(name=name, command=command, exit_code=result.returncode,
                    seconds=round(time.monotonic() - started, 3))
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or b'', error.stderr or b''
        item = dict(name=name, command=command, timeout=True, seconds=60)
    (output / f'{name}.stdout').write_bytes(stdout)
    (output / f'{name}.stderr').write_bytes(stderr)
    return item

with ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for future in as_completed([pool.submit(run, name) for name in selected]):
        item = future.result()
        evidence['tests'].append(item)
        output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
        print(json.dumps(item), flush=True)
assert digest(binary) == evidence['binary_sha256']
evidence['passed'] = all(item.get('exit_code') == 0 for item in evidence['tests'])
assert len(evidence['tests']) == len(selected)
output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
assert evidence['passed'], [item['name'] for item in evidence['tests'] if item.get('exit_code') != 0]
print('WASM_SPEC_OK', flush=True)
