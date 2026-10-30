#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Isolate an upstream host-endian Wasm test fixture without editing its source."""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--label', required=True)
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
source = 'deps/v8/test/mjsunit/wasm/instance-gc.js'
original = subprocess.check_output(['git', 'show', 'HEAD:' + source], cwd=root)
assert (root / source).read_bytes() == original
before = b'  var view = new Int32Array(instance.exports.memory.buffer);\n  view[0] = val;'
after = b'  var view = new DataView(instance.exports.memory.buffer);\n  view.setInt32(0, val, true);'
assert original.count(before) == 1
output = root / ('out/sparc64-wasm-production-instance-gc-control-' + args.label)
assert not output.exists(), 'Preserve previous evidence before retrying'
output.mkdir(parents=True)
derived = output / 'instance-gc-little-endian.js'
derived.write_bytes(original.replace(before, after, 1))
report = json.loads((root / ('out/sparc64-wasm-production-upstream-' + args.label +
                            '-gc-extended-default.json')).read_text())
case = next(item for item in report['cases'] if item['name'] == 'wasm/instance-gc')
assert not case['passed'] and case['exit_code'] == 1
assert len(report['cases']) == 17
assert all(item['passed'] and item['exit_code'] == 0
           for item in report['cases'] if item is not case)
binary = root / 'out/Release/d8'
binary_hash = digest(binary)
assert report['binary_sha256'] == binary_hash
prefix = root / ('out/sparc64-wasm-production-upstream-' + args.label + '-gc-extended-default')
negative = (prefix / 'wasm__instance-gc.stdout').read_text()
assert 'Failure: expected <99> found <1660944384>' in negative
results = []
for tier, flags in [('liftoff', ['--liftoff-only']),
                    ('optimized', ['--no-liftoff-only', '--no-liftoff']), ('default', [])]:
    command = [str(binary), *flags, '--no-wasm-lazy-compilation', '--expose-gc',
               'test/mjsunit/mjsunit.js', str(derived)]
    run = subprocess.run(command, cwd=root / 'deps/v8', capture_output=True, timeout=120)
    (output / (tier + '.stdout')).write_bytes(run.stdout)
    (output / (tier + '.stderr')).write_bytes(run.stderr)
    results.append(dict(tier=tier, command=command, exit_code=run.returncode,
                        passed=run.returncode == 0))
probe = '''
const buffer = new ArrayBuffer(4);
new Int32Array(buffer)[0] = 99;
print(JSON.stringify({hostBytes: Array.from(new Uint8Array(buffer)),
                      wasmLittleEndianValue: new DataView(buffer).getInt32(0, true)}));
'''
run = subprocess.run([str(binary), '-e', probe], capture_output=True, check=True, timeout=30)
host = json.loads(run.stdout)
assert host == dict(hostBytes=[0, 0, 0, 99], wasmLittleEndianValue=1660944384)
assert digest(binary) == binary_hash and (root / source).read_bytes() == original
result = dict(passed=all(item['passed'] for item in results), binary_sha256=binary_hash,
              original_source=source, original_sha256=hashlib.sha256(original).hexdigest(),
              original_test_passed=False, original_exit_code=case['exit_code'],
              original_stdout_sha256=digest(prefix / 'wasm__instance-gc.stdout'),
              correction_from=before.decode(), correction_to=after.decode(),
              derived_source=str(derived.relative_to(root)), derived_sha256=digest(derived),
              host_endian_probe=host, results=results)
output.with_suffix('.json').write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not result['passed'])
