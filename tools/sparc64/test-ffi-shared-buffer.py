#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run the upstream shared-buffer body separately from its big-endian skip."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
parser.add_argument('--expect-disabled', action='store_true')
parser.add_argument('--suite', choices=['shared-buffer', 'dynamic-library'], default='shared-buffer')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda path: hashlib.file_digest(path.open('rb'), 'sha256').hexdigest()
relative = 'test/ffi/test-ffi-' + args.suite + '.js'
source = root / relative
original = source.read_bytes()
assert original == subprocess.check_output(['git', 'show', 'HEAD:' + relative], cwd=root)
guard = b"if (endianness() === 'BE') {\n  common.skip('shared-buffer FFI is disabled on big-endian hosts');\n}\n"
replacement = b''
if args.suite == 'dynamic-library':
    guard = b"endianness() === 'BE' ? 0 : 2"
    replacement = b'2'
assert original.count(guard) == 1
prefix = root / ('out/sparc64-node-features-ffi-' + args.suite + '-' + args.label)
prefix.mkdir(exist_ok=False)
(prefix / 'common').symlink_to(root / 'test/common', target_is_directory=True)
(prefix / 'ffi').mkdir()
(prefix / 'ffi/ffi-test-common.js').symlink_to(root / 'test/ffi/ffi-test-common.js')
derived = prefix / ('ffi/test-' + args.suite + '.cjs')
derived.write_bytes(original.replace(guard, replacement, 1))
binary = root / 'out/Release/node'
binary_hash = digest(binary)
command = [str(binary), '--test-reporter=tap', '--expose-internals', '--verify-heap', str(derived)]
with (prefix / 'stdout').open('w') as stdout, (prefix / 'stderr').open('w') as stderr:
    try:
        code = subprocess.run(command, cwd=root, stdout=stdout, stderr=stderr,
                              timeout=120).returncode
    except subprocess.TimeoutExpired:
        code = 'timeout'
output = (prefix / 'stdout').read_text()
counts = {key: int(value) for key, value in re.findall(
    r'^# (tests|pass|fail|skipped) (\d+)$', output, re.M)}
success = code == 0 and counts.get('tests', 0) > 0 and \
    counts.get('tests') == counts.get('pass') and counts.get('skipped') == 0
assert digest(binary) == binary_hash and source.read_bytes() == original
report = dict(passed=success, expected_disabled=args.expect_disabled,
              binary_sha256=binary_hash, command=command, exit_code=code, counts=counts,
              source=relative, source_sha256=digest(source),
              derived_source=str(derived.relative_to(root)), derived_sha256=digest(derived),
              correction_from=guard.decode(), correction_to=replacement.decode(),
              original_unchanged=True)
prefix.with_suffix('.json').write_text(json.dumps(report, indent=2) + '\n')
print(json.dumps(report), flush=True)
raise SystemExit(not (code == 1 and counts.get('fail', 0) > 0
                     if args.expect_disabled else success))
