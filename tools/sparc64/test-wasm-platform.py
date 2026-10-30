#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Exercise native atomic expected-value alignment with a negative control."""

import hashlib
import json
import os
from pathlib import Path
import resource
import signal
import subprocess

root = Path(__file__).resolve().parents[2]
assert os.uname().machine == 'sparc64', 'Run this test natively on SPARC64'
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
output = root / 'out/sparc64-wasm-platform'
output.mkdir(parents=True, exist_ok=True)
source = root / 'tools/sparc64/test-wasm-atomic.cc'
evidence = dict(passed=False, source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
                compiler=subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
                controls=[])
for aligned in [False, True]:
    name = 'aligned' if aligned else 'natural'
    binary = output / name
    command = ['g++', '-O3', '-mcpu=v9', '-DALIGNED_HEAD=' + str(int(aligned)),
               str(source), '-o', str(binary)]
    subprocess.run(command, check=True, capture_output=True, timeout=30)
    result = subprocess.run([str(binary)], capture_output=True, timeout=10)
    (output / f'{name}.stdout').write_bytes(result.stdout)
    (output / f'{name}.stderr').write_bytes(result.stderr)
    item = dict(name=name, command=command, exit_code=result.returncode,
                binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
    evidence['controls'].append(item)
    print(json.dumps(item), flush=True)
    assert result.returncode == 0 if aligned else result.returncode in [0, -signal.SIGBUS], item
    if aligned:
        assert b'ATOMIC_EXCHANGE_OK' in result.stdout, result.stdout
    else:
        evidence['negative_control_reproduced'] = result.returncode == -signal.SIGBUS
signature_source = root / 'tools/sparc64/test-wasm-signature.cc'
negative = output / 'original-signature/src/codegen/signature.h'
negative.parent.mkdir(parents=True, exist_ok=True)
negative.write_bytes(subprocess.check_output(
    ['git', 'show', 'HEAD:deps/v8/src/codegen/signature.h'], cwd=root))
evidence['signature_source_sha256'] = hashlib.sha256(signature_source.read_bytes()).hexdigest()
evidence['signature_header_sha256'] = hashlib.sha256(
    (root / 'deps/v8/src/codegen/signature.h').read_bytes()).hexdigest()
evidence['signature_controls'] = []
for fixed in [False, True]:
    for optimization in ['-O0', '-O3']:
        name = ('signature-fixed' if fixed else 'signature-original') + optimization
        binary = output / name
        command = ['g++', '-std=gnu++20', optimization, '-mcpu=v9', '-DNDEBUG',
                   '-DV8_TARGET_ARCH_SPARC64', '-ffunction-sections', '-fdata-sections']
        if not fixed:
            command.append('-I' + str(output / 'original-signature'))
        command += ['-I' + str(root / 'deps/v8'), '-I' + str(root / 'deps/v8/include'),
                    '-I' + str(root / 'deps/v8/third_party/abseil-cpp'),
                    str(signature_source), '-Wl,--gc-sections', '-o', str(binary)]
        build = subprocess.run(command, capture_output=True, timeout=60)
        (output / (name + '-build.log')).write_bytes(build.stdout + build.stderr)
        assert build.returncode == 0, build.stderr.decode(errors='replace')
        result = subprocess.run([str(binary)], capture_output=True, timeout=10)
        (output / (name + '.stdout')).write_bytes(result.stdout)
        (output / (name + '.stderr')).write_bytes(result.stderr)
        item = dict(name=name, command=command, exit_code=result.returncode,
                    expected_success=fixed, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest())
        evidence['signature_controls'].append(item)
        print(json.dumps(item), flush=True)
        assert result.returncode == (0 if fixed else 1), item
        if fixed:
            assert b'SIGNATURE_STORAGE_OK' in result.stdout

evidence['passed'] = True
output.with_suffix('.json').write_text(json.dumps(evidence, indent=2) + '\n')
print('WASM_PLATFORM_OK', flush=True)
