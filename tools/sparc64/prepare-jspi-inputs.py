#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Seed inherited immutable inputs and negative controls from the scalar checkpoint."""
import hashlib
import json
from pathlib import Path
import shutil

root = Path(__file__).resolve().parents[2]
parent = root.with_name('node-sparc64-wasm-scalar')
checkpoint = Path((parent / 'out/sparc64-wasm-scalar-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == json.loads((root / 'out/sparc64-jspi-parent.json').read_text())
assert manifest['profile'] == 'wasm-scalar'
assert shutil.disk_usage(root).free > 2 * 1024 ** 3
directories = {'wasm-upstream-inputs', 'wasm-threads-spec', 'wasm-atomics-workload',
               'wasm-scalar-application', 'sparc64-wasm-atomics-attempts',
               'sparc64-snapshot-before-fix', 'sparc64-snapshot-array-buffer-keys-baseline',
               'sparc64-wasm-scalar-baseline', 'sparc64-wasm-scalar-baseline-exnref'}
files = {'sparc64-snapshot-array-buffer-keys-baseline.json',
         'sparc64-wasm-scalar-baseline.json', 'sparc64-wasm-scalar-baseline-exnref.json',
         'sparc64-wasm-scalar-spec-baseline.json', 'sparc64-wasm-scalar-spec-baseline.log',
         'sparc64-wasm-scalar-spec-baseline-raw.json', 'scalar-ref-baseline-gdb.log'}
copied = {}
for name, expected in manifest['artifact_sha256'].items():
    parts = Path(name).parts
    if len(parts) < 2 or parts[0] != 'validation':
        continue
    relative = Path(*parts[1:])
    if relative.parts[0] not in directories and str(relative) not in files:
        continue
    source = checkpoint / name
    with source.open('rb') as stream:
        assert hashlib.file_digest(stream, 'sha256').hexdigest() == expected
    destination = root / 'out' / relative
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied[str(relative)] = expected
for name in ['sparc64-wasm-parent.json', 'sparc64-wasm-atomics-parent.json',
             'sparc64-wasm-scalar-parent.json']:
    artifact = {'sparc64-wasm-parent.json': 'hardening-parent-manifest.json',
                'sparc64-wasm-atomics-parent.json': 'wasm-parent-manifest.json',
                'sparc64-wasm-scalar-parent.json': 'wasm-atomics-parent-manifest.json'}[name]
    source, destination = checkpoint / artifact, root / 'out' / name
    assert hashlib.sha256(source.read_bytes()).hexdigest() == manifest['artifact_sha256'][artifact]
    if destination.exists():
        assert destination.read_bytes() == source.read_bytes()
    else:
        shutil.copy2(source, destination)
(root / 'out/sparc64-jspi-inputs.json').write_text(json.dumps(dict(
    parent_checkpoint=str(checkpoint), copied=copied), indent=2) + '\n')
print('JSPI_INPUTS_READY', len(copied), flush=True)
