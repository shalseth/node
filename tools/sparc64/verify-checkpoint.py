#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Verify every checkpoint artifact and source-overlay member without extracting."""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tarfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('checkpoint', type=Path)
parser.add_argument('--source-root', type=Path, help='Also verify the current source overlay')
args = parser.parse_args()
checkpoint = args.checkpoint.resolve()


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


recorded = {}
for line in (checkpoint / 'SHA256SUMS').read_text().splitlines():
    value, name = line.split('  ', 1)
    path = Path(name)
    assert not path.is_absolute() and '..' not in path.parts
    assert len(value) == 64 and name not in recorded
    assert digest(checkpoint / name) == value, name
    recorded[name] = value
actual = {str(path.relative_to(checkpoint)) for path in checkpoint.rglob('*') if path.is_file()}
assert actual == set(recorded) | {'SHA256SUMS'}
manifest = json.loads((checkpoint / 'manifest.json').read_text())
assert manifest['base_head'] == (checkpoint / 'BASE_HEAD').read_text().strip()
assert manifest['source_file_count'] == len(manifest['source_sha256'])
assert set(recorded) == set(manifest['artifact_sha256']) | {'manifest.json'}
for name, value in manifest['artifact_sha256'].items():
    assert recorded[name] == value, name
with tarfile.open(checkpoint / 'source-overlay.tar.gz') as stream:
    members = stream.getmembers()
    assert len(members) == len(manifest['source_sha256'])
    assert {member.name for member in members} == set(manifest['source_sha256'])
    for member in members:
        assert member.isfile() and not Path(member.name).is_absolute()
        assert '..' not in Path(member.name).parts
        with stream.extractfile(member) as source:
            value = hashlib.file_digest(source, 'sha256').hexdigest()
        assert value == manifest['source_sha256'][member.name], member.name
if args.source_root:
    root = args.source_root.resolve()
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    assert head == manifest['base_head']
    changed = set()
    for command in [['git', 'diff', '--name-only', '--diff-filter=ACMRT', 'HEAD'],
                    ['git', 'ls-files', '--others', '--exclude-standard']]:
        changed.update(subprocess.check_output(command, cwd=root, text=True).splitlines())
    assert {name for name in changed if (root / name).is_file()} == set(manifest['source_sha256'])
    for name, value in manifest['source_sha256'].items():
        assert digest(root / name) == value, name
    assert subprocess.check_output(['git', 'diff', '--binary', 'HEAD'], cwd=root) == (
        checkpoint / 'tracked.patch').read_bytes()
print('CHECKPOINT_VERIFIED', json.dumps(dict(profile=manifest['profile'],
      base_head=manifest['base_head'], sources=manifest['source_file_count'],
      artifacts=len(recorded), checkpoint=str(checkpoint))), flush=True)
