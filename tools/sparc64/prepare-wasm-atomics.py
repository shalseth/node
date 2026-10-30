#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Pin the official threads tests and translate their unchanged scalar assertions."""

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[2]
output = root / 'out/wasm-threads-spec'
output.mkdir(parents=True, exist_ok=True)
commit = 'cc535ada1aa21cfaa3cabf3ac73b89acef78a0a0'
expected = '8cc7e0b408c6ae84865497aec07fc3afd85f5021143111681e9abc3ead963290'
url = 'https://codeload.github.com/WebAssembly/threads/tar.gz/' + commit
archive = output / 'threads.tar.gz'
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
if not archive.exists():
    temporary = archive.with_suffix('.partial')
    with urllib.request.urlopen(url, timeout=30) as source, temporary.open('wb') as target:
        shutil.copyfileobj(source, target)
    assert digest(temporary) == expected
    temporary.rename(archive)
assert digest(archive) == expected
sources = {}
with tarfile.open(archive) as stream:
    for entry in stream:
        prefix = 'threads-' + commit + '/test/core/threads/'
        if not entry.isfile() or not entry.name.startswith(prefix):
            continue
        relative = Path('sources/threads') / entry.name.removeprefix(prefix)
        assert '..' not in relative.parts
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(stream.extractfile(entry).read())
        sources[str(relative)] = digest(target)
(output / 'source-manifest.json').write_text(json.dumps(dict(
    repository='https://github.com/WebAssembly/threads', commit=commit, url=url,
    archive_sha256=expected, sources=sources), indent=2) + '\n')
translator = os.environ.get('WAST2JSON', 'wast2json')
command = [translator, '--enable-threads', 'sources/threads/atomic.wast', '-o', 'atomic.json']
subprocess.run(command, cwd=output, check=True)
files = {str(p.relative_to(output)): digest(p) for p in sorted(output.glob('atomic.*'))}
(output / 'translation-manifest.json').write_text(json.dumps(dict(
    command=command, translator_version=subprocess.check_output([translator, '--version'], text=True).strip(),
    translator_sha256=digest(Path(shutil.which(translator) or translator)),
    source_sha256=digest(output / 'sources/threads/atomic.wast'), files=files), indent=2) + '\n')
print('WASM_ATOMIC_SPEC_INPUTS_OK', flush=True)
