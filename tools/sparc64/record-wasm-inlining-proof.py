#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Record compiler trace evidence from unchanged native inlining tests."""
import argparse
import hashlib
import json
from pathlib import Path
import re
root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--input-label', default='inlining-native-trace')
parser.add_argument('--label', default='current')
parser.add_argument('--binary', type=Path, default=root / 'out/Release/d8')
args = parser.parse_args()
digest = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
source = root / ('out/sparc64-wasm-production-upstream-' + args.input_label)
report = json.loads(source.with_suffix('.json').read_text())
assert report['passed'] and report['binary_sha256'] == digest(args.binary)
cases = {case['name']: case for case in report['cases']}
required = ['wasm/inlining', 'wasm/wasm-gc-inlining-nested', 'wasm/wasm-wrapper-inlining']
assert sorted(cases) == sorted(required)
for case in cases.values():
    assert case['passed']
    for flag in ['--trace-wasm-inlining', '--trace-turbo-inlining', '--wasm-inlining',
                 '--turbo-inline-js-wasm-calls']:
        assert flag in case['command'], flag
paths = {name: source / (name.replace('/', '__') + '.stdout') for name in required}
js_trace = paths['wasm/wasm-gc-inlining-nested'].read_text()
wasm_trace = paths['wasm/inlining'].read_text()
js_bodies = js_trace.splitlines().count('- inlining')
wasm_calls = len(re.findall(r'\[function \d+: inlining direct call #\d+ to function \d+\]', wasm_trace))
assert js_bodies >= 4 and wasm_calls > 0, (js_bodies, wasm_calls)
compiler_sources = ['deps/v8/src/compiler/js-inlining.cc',
                    'deps/v8/src/compiler/wasm-compiler.cc',
                    'deps/v8/src/compiler/wasm-inlining-into-js.cc']
result = dict(passed=True, binary_sha256=report['binary_sha256'],
              upstream_report=str(source.with_suffix('.json').relative_to(root)),
              upstream_report_sha256=digest(source.with_suffix('.json')),
              wasm_bodies_inlined_into_js=js_bodies, wasm_direct_calls_inlined=wasm_calls,
              trace_sha256={str(p.relative_to(root)): digest(p) for p in paths.values()},
              compiler_source_sha256={name: digest(root / name) for name in compiler_sources})
output = root / ('out/sparc64-wasm-inlining-proof-' + args.label + '.json')
assert not output.exists(), 'Preserve previous evidence before retrying'
output.write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
