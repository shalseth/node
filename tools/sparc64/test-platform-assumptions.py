#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Verify architecture constants and run permission tests without root bypass."""

import hashlib
import json
import os
from pathlib import Path
import pwd
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
node = root / 'out/Release/node'
output = root / 'out/sparc64-hardening-platform'
output.mkdir(exist_ok=True)
source = output / 'noatime.c'
source.write_text('#define _GNU_SOURCE\n#include <fcntl.h>\n#include <stdio.h>\nint main(void) { printf("%d\\n", O_NOATIME); }\n')
subprocess.run(['gcc', str(source), '-o', str(output / 'noatime')], check=True, timeout=30)
native = int(subprocess.check_output([str(output / 'noatime')], text=True))
actual = int(subprocess.check_output([str(node), '-p', 'require("fs").constants.O_NOATIME'], text=True))
assert native == actual
account = pwd.getpwnam('nobody')
fixture = 'test-fs-rmSync-special-char-additional-error.js'
with tempfile.TemporaryDirectory(prefix='sparc64-permissions-') as name:
    staged = Path(name)
    staged.chmod(0o755)
    (staged / 'test/parallel').mkdir(parents=True)
    shutil.copytree(root / 'test/common', staged / 'test/common')
    shutil.copy2(root / 'test/parallel' / fixture, staged / 'test/parallel' / fixture)
    shutil.copy2(node, staged / 'node')
    temporary = staged / 'temporary'
    temporary.mkdir()
    os.chown(temporary, account.pw_uid, account.pw_gid)
    env = dict(os.environ, NODE_TEST_DIR=str(temporary))
    command = ['runuser', '-u', 'nobody', '--', str(staged / 'node'),
               str(staged / 'test/parallel' / fixture)]
    result = subprocess.run(command, cwd=staged, env=env, capture_output=True, timeout=30)
    (output / 'permissions.log').write_bytes(result.stdout + result.stderr)
    assert result.returncode == 0, result.stderr.decode(errors='replace')
evidence = dict(passed=True, binary_sha256=hashlib.sha256(node.read_bytes()).hexdigest(),
                node_noatime=actual, native_noatime=native, permission_test=fixture,
                permission_test_exit=result.returncode,
                permission_test_source_sha256=hashlib.sha256((root / 'test/parallel' / fixture).read_bytes()).hexdigest())
(root / 'out/sparc64-hardening-platform.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('PLATFORM_ASSUMPTIONS_OK', json.dumps(evidence), flush=True)
