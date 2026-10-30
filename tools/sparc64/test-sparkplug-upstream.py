#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run the upstream baseline cases that do not require an optimizing tier."""

from pathlib import Path
import re
import resource
import shlex
import subprocess
import time

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = [
    'test-baseline.js',
    'test-baseline-module.mjs',
    'cross-realm.js',
    'loadic-transition-elements.js',
    'batch-compilation.js',
    'flush-baseline-code.js',
    'flush-only-baseline-code.js',
    'verify-bytecode-offsets.js',
]
failures = []
for name in cases:
    path = root / 'deps/v8/test/mjsunit/baseline' / name
    flags = []
    for line in re.findall(r'^// Flags: (.*)$', path.read_text(), re.M):
        flags.extend(shlex.split(line))
    command = [str(root / 'out/Release/d8'), *flags,
               str(root / 'deps/v8/test/mjsunit/mjsunit.js')]
    if path.suffix == '.mjs':
        command.append('--module')
    command.append(str(path))
    started = time.monotonic()
    try:
        result = subprocess.run(command, cwd=root, capture_output=True,
                                text=True, timeout=15)
        passed = result.returncode == 0
        output = result.stdout + result.stderr
    except subprocess.TimeoutExpired:
        passed = False
        output = 'Timed out after 15 seconds'
    print(f'{"PASS" if passed else "FAIL"} {name} '
          f'{time.monotonic() - started:.3f}s', flush=True)
    if not passed:
        failures.append(name)
        print(shlex.join(command), flush=True)
        print(output, flush=True)
print(f'{len(cases) - len(failures)}/{len(cases)} upstream baseline cases passed',
      flush=True)
raise SystemExit(bool(failures))
