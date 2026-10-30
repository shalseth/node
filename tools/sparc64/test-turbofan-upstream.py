#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run bounded upstream deoptimization and OSR cases with compilation proof."""

import json
from pathlib import Path
import re
import resource
import shlex
import subprocess

root = Path(__file__).resolve().parents[2]
v8 = root / 'deps/v8'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = [
    'compiler/deopt-simple-eager',
    'compiler/deopt-simple-lazy',
    'compiler/deopt-followed-by-gc',
    'compiler/deopt-simple-try-catch',
    'baseline/test-osr',
]
results = []
for name in cases:
    source = v8 / 'test/mjsunit' / (name + '.js')
    flags = []
    for line in re.findall(r'^// Flags: (.*)$', source.read_text(), re.M):
        flags.extend(shlex.split(line))
    command = [str(root / 'out/Release/d8'), '--no-concurrent-recompilation',
               '--trace-opt', '--trace-deopt', *flags,
               'test/mjsunit/mjsunit.js', str(source)]
    try:
        result = subprocess.run(command, cwd=v8, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, timeout=15)
        output = result.stdout
        compilations = output.count('completed compiling')
        passed = result.returncode == 0 and compilations > 0
        exit_code = result.returncode
    except subprocess.TimeoutExpired as error:
        output = error.stdout or b''
        if isinstance(output, bytes):
            output = output.decode(errors='replace')
        compilations = 0
        passed = False
        exit_code = 'timeout'
    log = root / 'out' / ('sparc64-turbofan-' + name.replace('/', '-') + '.log')
    log.write_text(output)
    results.append({'test': name, 'exit': exit_code, 'pass': passed,
                    'optimizing_compilations': compilations, 'command': command})
    print(f'{"PASS" if passed else "FAIL"} {name} '
          f'({compilations} optimizing compilations)', flush=True)
    if not passed:
        print(output, flush=True)
(root / 'out/sparc64-turbofan-selected.json').write_text(
    json.dumps(results, indent=2) + '\n')
failures = sum(not result['pass'] for result in results)
print(f'{len(cases) - failures}/{len(cases)} upstream optimizing cases passed',
      flush=True)
raise SystemExit(bool(failures))
