#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Fetch the exact Wasm test data pinned by the vendored V8 DEPS hooks."""

import hashlib
import json
from pathlib import Path
import shutil
import tarfile
import urllib.request

root = Path(__file__).resolve().parents[2]
cache = root / 'out/wasm-upstream-inputs'
cache.mkdir(parents=True, exist_ok=True)
records = []
for suite in ['wasm-spec-tests', 'wasm-js']:
    directory = root / 'deps/v8/test' / suite
    expected = (directory / 'tests.tar.gz.sha1').read_text().strip()
    assert len(expected) == 40 and all(c in '0123456789abcdef' for c in expected)
    archive = cache / f'{suite}-{expected}.tar.gz'
    url = 'https://storage.googleapis.com/v8-wasm-spec-tests/' + expected
    if not archive.exists():
        assert shutil.disk_usage(root).free > 1024 ** 3
        temporary = archive.with_suffix('.partial')
        with urllib.request.urlopen(url, timeout=30) as source, temporary.open('wb') as destination:
            shutil.copyfileobj(source, destination)
        assert hashlib.sha1(temporary.read_bytes()).hexdigest() == expected
        temporary.rename(archive)
    assert hashlib.sha1(archive.read_bytes()).hexdigest() == expected
    with tarfile.open(archive) as stream:
        members = stream.getmembers()
        assert all(not m.issym() and not m.islnk() and
                   not Path(m.name).is_absolute() and '..' not in Path(m.name).parts and
                   Path(m.name).parts[0] == 'tests' for m in members)
        size = sum(m.size for m in members)
        assert shutil.disk_usage(root).free > size + 512 * 1024 ** 2
        stream.extractall(directory, filter='data')
    record = dict(suite=suite, url=url, sha1=expected,
                  sha256=hashlib.sha256(archive.read_bytes()).hexdigest(),
                  archive=str(archive.relative_to(root)), files=sum(m.isfile() for m in members),
                  unpacked_bytes=size)
    records.append(record)
    print(json.dumps(record), flush=True)
(cache / 'manifest.json').write_text(json.dumps(records, indent=2) + '\n')
print('WASM_SUITE_INPUTS_OK', flush=True)
