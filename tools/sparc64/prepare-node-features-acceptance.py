#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Expose the authenticated feature build to the retained regression drivers."""
import json
from pathlib import Path
import subprocess

from node_features_validation import digest, validate_sources

root = Path(__file__).resolve().parents[2]
validate_sources(root)
path = root / 'out/sparc64-node-features-build-all.json'
build = json.loads(path.read_text())
record = [dict(item, source_sha256=build['source_sha256'],
               binary_sha256={name: build['binary_sha256'][name] for name in ['node', 'd8']},
               node_features_build_sha256=digest(path)) for item in build['results']]
target = root / 'out/sparc64-wasm-build.json'
if target.exists():
    assert json.loads(target.read_text()) == record, 'Preserve earlier regression evidence first'
else:
    target.write_text(json.dumps(record, indent=2) + '\n')
for script, flags in [
    ('prepare-node-features-regression-inputs.py', []),
    ('prepare-wasm-production-inputs.py', []),
    ('write-test-config.py', ['--allow-turbofan', '--allow-webassembly']),
]:
    subprocess.run(['python3', 'tools/sparc64/' + script, *flags], cwd=root, check=True)
print('NODE_FEATURES_ACCEPTANCE_INPUTS_READY', flush=True)
