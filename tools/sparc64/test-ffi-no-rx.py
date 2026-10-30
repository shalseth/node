#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Exercise the upstream RX-denial assertions with a SPARC seccomp fixture."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
assert sys.byteorder == 'big'
digest = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
prefix = root / ('out/sparc64-node-features-ffi-no-rx-' + args.label)
prefix.mkdir(exist_ok=False)
(prefix / 'common').symlink_to(root / 'test/common', target_is_directory=True)
(prefix / 'ffi').mkdir()
(prefix / 'ffi/ffi-test-common.js').symlink_to(root / 'test/ffi/ffi-test-common.js')
sources = ['test/ffi/test-ffi-no-rx-memory.js',
           'test/ffi/fixture_library/deny_executable_memory.c',
           'test/ffi/fixture_library/ffi_test_library.c']
original = {}
for name in sources:
    original[name] = (root / name).read_bytes()
    assert original[name] == subprocess.check_output(['git', 'show', 'HEAD:' + name], cwd=root)
library = prefix / 'ffi_test_library.so'
test = original[sources[0]]
replacements = [
    (b"if (require('node:os').endianness() !== 'LE') {\n  common.skip('The seccomp fixture reads little-endian syscall arguments');\n}\n", b''),
    (b"'loong64', 's390x'].includes(process.arch)", b"'loong64', 's390x', 'sparc64'].includes(process.arch)"),
    (b"const { libraryPath, fixtureSymbols } = require('./ffi-test-common');",
     ("const { fixtureSymbols } = require('./ffi-test-common');\nconst libraryPath = " +
      json.dumps(str(library)) + ';').encode()),
]
for before, after in replacements:
    assert test.count(before) == 1
    test = test.replace(before, after, 1)
derived = prefix / 'ffi/test-no-rx.cjs'
derived.write_bytes(test)
fixture = prefix / 'deny_executable_memory.c'
word = b'offsetof(struct seccomp_data, args[2])'
assert original[sources[1]].count(word) == 1
fixture.write_bytes(original[sources[1]].replace(word, word + b' + 4', 1))
compile_command = ['gcc', '-O2', '-fPIC', '-shared', str(root / sources[2]),
                   str(fixture), '-o', str(library)]
with (prefix / 'build.log').open('w') as log:
    compile_code = subprocess.run(compile_command, cwd=root, stdout=log,
                                  stderr=subprocess.STDOUT).returncode
assert compile_code == 0
binary = root / 'out/Release/node'
binary_hash = digest(binary)
command = [str(binary), '--jitless', str(derived)]
with (prefix / 'stdout').open('w') as stdout, (prefix / 'stderr').open('w') as stderr:
    try:
        code = subprocess.run(command, cwd=root, stdout=stdout, stderr=stderr,
                              timeout=60).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
output = (prefix / 'stdout').read_text() + (prefix / 'stderr').read_text()
assert digest(binary) == binary_hash
assert all((root / name).read_bytes() == data for name, data in original.items())
report = dict(passed=code == 0 and 'skip' not in output.lower(),
              binary_sha256=binary_hash, command=command, exit_code=code,
              source_sha256={name: digest(root / name) for name in sources},
              derived_sha256={str(path.relative_to(root)): digest(path)
                              for path in [fixture, derived]},
              replacements=[dict(before=a.decode(), after=b.decode()) for a, b in replacements],
              seccomp_word_offset_correction=4, library_sha256=digest(library),
              compile_command=compile_command, original_unchanged=True)
prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
raise SystemExit(not report['passed'])
