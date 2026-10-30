#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Fetch the exact Test262 revision pinned by V8 for Temporal acceptance."""
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[2]
deps = root / 'deps/v8/DEPS'
assert deps.read_bytes() == subprocess.check_output(['git', 'show', 'HEAD:deps/v8/DEPS'], cwd=root)
commit = re.search(r"'test/test262/data':\s*.*?@' \+ '([0-9a-f]{40})'", deps.read_text(), re.S).group(1)
cache = root / 'out/test262-inputs'
cache.mkdir(parents=True, exist_ok=True)
archive = cache / ('test262-' + commit + '.tar.gz')
url = 'https://codeload.github.com/tc39/test262/tar.gz/' + commit
assert shutil.disk_usage(root).free > 2 * 1024 ** 3
if not archive.exists():
    partial = archive.with_suffix('.partial')
    with urllib.request.urlopen(url, timeout=60) as source, partial.open('wb') as target:
        shutil.copyfileobj(source, target)
    partial.rename(archive)
digest = lambda p: hashlib.file_digest(p.open('rb'), 'sha256').hexdigest()
suite = root / 'deps/v8/test/test262'
destination = suite / 'data'
subprocess.run(['git', 'check-ignore', str(destination / 'test')], cwd=root, check=True,
               stdout=subprocess.DEVNULL)
if not destination.exists():
    staging = cache / 'staging'
    assert not staging.exists(), 'Inspect any incomplete Test262 extraction before retrying'
    staging.mkdir()
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
        assert all((m.isfile() or m.isdir()) and not Path(m.name).is_absolute() and
                   '..' not in Path(m.name).parts and
                   Path(m.name).parts[0] == 'test262-' + commit for m in members)
        size = sum(m.size for m in members)
        assert shutil.disk_usage(root).free > size + 512 * 1024 ** 2
        stream.extractall(staging, filter='data')
    (staging / ('test262-' + commit)).rename(destination)
    staging.rmdir()
selected = {}
for path in sorted((destination / 'test').rglob('*.js')):
    text = path.read_text()
    header = re.search(r'/\*---(.*?)---\*/', text, re.S)
    relative = str(path.relative_to(destination / 'test'))
    if (relative.startswith(('built-ins/Temporal/', 'intl402/Temporal/')) or
            (header and re.search(r'\bTemporal\b', header.group(1)))):
        selected[relative] = digest(path)
assert selected
record = dict(commit=commit, url=url, archive=str(archive.relative_to(root)),
              archive_sha256=digest(archive), deps_sha256=digest(deps),
              temporal_selected=selected, temporal_source_count=len(selected))
(root / 'out/sparc64-node-features-test262-inputs.json').write_text(json.dumps(record, indent=2) + '\n')
print('TEMPORAL_TEST262_INPUTS_READY', commit, len(selected), digest(archive), flush=True)
