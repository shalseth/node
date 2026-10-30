#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Verify an optimized function and deoptimization beyond the short-branch range."""

import json
from pathlib import Path
import re
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
command = [str(root / 'out/Release/d8'), '--allow-natives-syntax',
           '--no-concurrent-recompilation', '--max-optimized-bytecode-size=1000000',
           '--trace-opt', '--trace-deopt', '--print-opt-code',
           '--print-opt-code-filter=large', 'tools/sparc64/test-backend-large.js']
log = root / 'out/sparc64-turbofan-backend-large.log'
with log.open('w') as stream:
    result = subprocess.run(command, cwd=root, stdout=stream,
                            stderr=subprocess.STDOUT, timeout=90)
output = log.read_text()
match = re.search(r'Instructions \(size = (\d+)\)', output)
size = int(match[1]) if match else 0
deoptimized = bool(re.search(r'bailout \(kind: deopt-eager[^\n]*JSFunction large',
                            output))
passed = (result.returncode == 0 and size > 1 << 20 and deoptimized and
          'LARGE OPTIMIZED FUNCTION AND EAGER DEOPT PASS' in output)
summary = {'pass': passed, 'exit': result.returncode, 'machine_code_bytes': size,
           'eager_deoptimization': deoptimized, 'command': command}
(root / 'out/sparc64-turbofan-backend-large.json').write_text(
    json.dumps(summary, indent=2) + '\n')
print(f'{"PASS" if passed else "FAIL"} large optimized function: '
      f'{size} machine-code bytes, eager deoptimization={deoptimized}', flush=True)
raise SystemExit(not passed)
