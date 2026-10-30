#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Seed only immutable inherited inputs and negative controls for production audits."""
import json
from pathlib import Path
import shutil
import subprocess
from wasm_production_validation import digest, PARENT_MANIFEST_SHA256
root = Path(__file__).resolve().parents[2]
parent = root.with_name('node-sparc64-jspi')
checkpoint = Path((parent / 'out/sparc64-jspi-checkpoint-path.txt').read_text().strip())
assert digest(checkpoint / 'manifest.json') == PARENT_MANIFEST_SHA256
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest == json.loads((root / 'out/sparc64-wasm-production-parent.json').read_text())
assert shutil.disk_usage(root).free > 2 * 1024 ** 3
copied = {}
for name, expected in manifest['artifact_sha256'].items():
    if name == 'wasm-scalar-parent-manifest.json':
        relative = Path('sparc64-jspi-parent.json')
    elif name.startswith('validation/sparc64-jspi-baseline/') or name in {
        'validation/sparc64-jspi-baseline.json',
        'validation/sparc64-jspi-node-baseline.json',
        'validation/sparc64-jspi-node-baseline.stdout',
        'validation/sparc64-jspi-node-baseline.stderr',
    }:
        relative = Path(name.removeprefix('validation/'))
    else:
        continue
    source, destination = checkpoint / name, root / 'out' / relative
    assert digest(source) == expected
    if destination.exists():
        assert digest(destination) == expected, str(relative)
    else:
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
    copied[str(relative)] = expected
assert 'sparc64-jspi-parent.json' in copied
subprocess.run(['python3', 'tools/sparc64/prepare-jspi-inputs.py'], cwd=root, check=True)
(root / 'out/sparc64-wasm-production-regression-inputs.json').write_text(
    json.dumps(dict(parent_checkpoint=checkpoint.name, copied=copied), indent=2) + '\n')
print('WASM_PRODUCTION_REGRESSION_INPUTS_READY', len(copied), flush=True)
