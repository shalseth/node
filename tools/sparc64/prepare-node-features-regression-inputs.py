#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Seed authenticated inherited inputs without importing passing test reports."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parent = root.with_name('node-sparc64-wasm-production')
checkpoint = parent / 'out/checkpoints/node-wasm-production-20261029T234057.721541Z'
digest = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
expected_manifest = '3df269cf8395ac0673cdeadd8c2813222715a03b44e33f58cc36913cd62c269f'
assert digest(checkpoint / 'manifest.json') == expected_manifest
assert digest(root / 'out/sparc64-node-features-parent.json') == expected_manifest
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest['profile'] == 'wasm-production'
copied = {}
for name in ['validation/sparc64-wasm-production-parent.json']:
    source = checkpoint / name
    expected = manifest['artifact_sha256'][name]
    assert digest(source) == expected
    destination = root / 'out' / Path(name).name
    if destination.exists():
        assert digest(destination) == expected
    else:
        shutil.copy2(source, destination)
    copied[str(destination.relative_to(root))] = expected
subprocess.run(['python3', 'tools/sparc64/prepare-wasm-production-regression-inputs.py'],
               cwd=root, check=True)
(root / 'out/sparc64-node-features-regression-inputs.json').write_text(
    json.dumps(dict(parent_checkpoint=checkpoint.name,
                    parent_manifest_sha256=expected_manifest, copied=copied), indent=2) + '\n')
print('NODE_FEATURES_REGRESSION_INPUTS_READY', flush=True)
