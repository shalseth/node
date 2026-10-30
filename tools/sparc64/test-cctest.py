#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Run Node's native C++ tests with the disabled-Wasm case recorded explicitly."""

import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--profile', choices=['hardening', 'wasm'], default='hardening')
args = parser.parse_args()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
binary = root / 'out/Release/cctest'
prefix = 'sparc64-' + args.profile + '-cctest'
output = root / 'out' / (prefix + '.json')
excluded = {'EnvironmentTest.KeepsEmbedderWasmStreamingCallbackWhenAsked':
            'Executes WebAssembly.compileStreaming; WebAssembly is disabled'}
if args.profile == 'wasm':
    config = json.loads((root / 'out/Release/v8_build_config.json').read_text())
    assert config['has_webassembly']
    excluded = {}
command = [str(binary), '--gtest_output=json:' + str(output)]
if excluded:
    command.append('--gtest_filter=-' + ':'.join(excluded))
started = time.monotonic()
with (root / 'out' / (prefix + '.log')).open('w') as log:
    code = subprocess.run(command, cwd=root, stdout=log, stderr=subprocess.STDOUT, timeout=600).returncode
data = json.loads(output.read_text()) if output.exists() else {}
passed = code == 0 and data.get('tests', 0) >= (281 if args.profile == 'wasm' else 280) and data.get('failures', 1) == 0
result = dict(passed=passed, binary_sha256=hashlib.sha256(binary.read_bytes()).hexdigest(),
              node_sha256=hashlib.sha256((root / 'out/Release/node').read_bytes()).hexdigest(),
              command=command, exit_code=code, tests=data.get('tests', 0),
              failures=data.get('failures'), excluded=excluded,
              seconds=round(time.monotonic() - started, 3))
(root / 'out' / (prefix + '-run.json')).write_text(json.dumps(result, indent=2) + '\n')
print(json.dumps(result), flush=True)
raise SystemExit(not passed)
