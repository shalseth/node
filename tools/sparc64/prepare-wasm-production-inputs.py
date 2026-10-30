#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Seed immutable upstream archives from the verified JSPI parent checkpoint."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parent = root.with_name('node-sparc64-jspi')
checkpoint = Path((parent / 'out/sparc64-jspi-checkpoint-path.txt').read_text().strip())
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == json.loads((root / 'out/sparc64-wasm-production-parent.json').read_text())
assert manifest['profile'] == 'jspi'
assert shutil.disk_usage(root).free > 1024 ** 3
records = {}
for name, value in manifest['artifact_sha256'].items():
    if not name.startswith('validation/wasm-upstream-inputs/'):
        continue
    source = checkpoint / name
    assert hashlib.sha256(source.read_bytes()).hexdigest() == value
    destination = root / 'out' / name.removeprefix('validation/')
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        assert hashlib.sha256(destination.read_bytes()).hexdigest() == value
    else:
        shutil.copy2(source, destination)
    records[name] = value
assert len(records) >= 3
(root / 'out/sparc64-wasm-production-inputs.json').write_text(json.dumps(records, indent=2) + '\n')
subprocess.run(['python3', 'tools/sparc64/prepare-wasm-suites.py'], cwd=root, check=True)
print('WASM_PRODUCTION_INPUTS_OK', flush=True)
