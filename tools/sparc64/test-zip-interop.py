#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Validate Node ZIP bytes with independent readers and isolate Info-ZIP output."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import zipfile

root = Path(__file__).resolve().parents[2]
node = root / 'out/Release/node'
output = root / 'out/sparc64-hardening-zip'
output.mkdir(exist_ok=True)
command = [str(node), 'tools/sparc64/test-zip-interop.cjs']
result = subprocess.run(command, cwd=root, capture_output=True, timeout=30)
assert result.returncode == 0 and b'NODE_ZIP_ARCHIVE_WRITTEN' in result.stdout
expected = {'hello.txt': b'hello', 'unicode-名前.txt': '漢字😀'.encode(),
            'raw.bin': bytes([0, 1, 254, 255])}
with zipfile.ZipFile(output / 'node.zip') as archive:
    assert archive.testzip() is None
    assert set(archive.namelist()) == set(expected)
    for name, data in expected.items():
        assert archive.read(name) == data
with zipfile.ZipFile(output / 'reference.zip', 'w', compression=zipfile.ZIP_DEFLATED) as archive:
    for name, data in expected.items():
        archive.writestr(name, data)
results = []
for source in ['node', 'reference']:
    for locale in ['C.utf8', 'en_US.utf8']:
        env = dict(os.environ, LANG=locale, LC_ALL=locale)
        for tool in ['zipinfo', 'bsdtar']:
            command = [tool, *(['-tf'] if tool == 'bsdtar' else []), str(output / (source + '.zip'))]
            process = subprocess.run(command, env=env, capture_output=True, timeout=20)
            log = process.stdout + process.stderr
            (output / f'{source}-{locale}-{tool}.log').write_bytes(log)
            unicode_ok = all(name.encode() in process.stdout for name in expected)
            assert process.returncode == 0
            if tool == 'bsdtar':
                assert unicode_ok
            results.append(dict(source=source, locale=locale, tool=tool,
                                exit_code=process.returncode, unicode_ok=unicode_ok))
extracted = output / 'extracted'
extracted.mkdir(exist_ok=True)
subprocess.run(['bsdtar', '-xf', str(output / 'node.zip'), '-C', str(extracted)], check=True, timeout=20)
for name, data in expected.items():
    assert (extracted / name).read_bytes() == data
reference_ok = all(item['unicode_ok'] for item in results
                   if item['tool'] == 'zipinfo' and item['source'] == 'reference')
node_ok = all(item['unicode_ok'] for item in results
              if item['tool'] == 'zipinfo' and item['source'] == 'node')
assert node_ok == reference_ok
result = dict(passed=True, binary_sha256=hashlib.sha256(node.read_bytes()).hexdigest(),
              reference_zipinfo_preserves_unicode=reference_ok, results=results)
(root / 'out/sparc64-hardening-zip.json').write_text(json.dumps(result, indent=2) + '\n')
print('ZIP_INDEPENDENT_INTEROP_OK', json.dumps(result), flush=True)
