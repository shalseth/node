#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

"""Compare fresh-process startup with warm filesystem caches on the same host."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import time

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--parent', type=Path, default=root.with_name('node-sparc64-turbofan') / 'out/Release/node')
parser.add_argument('--samples', type=int, default=15)
args = parser.parse_args()
assert args.samples >= 10
node = root / 'out/Release/node'
parent = json.loads((root / 'out/sparc64-snapshot-parent.json').read_text())
def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()
assert digest(args.parent) == parent['artifact_sha256']['node']
workloads = {
    'empty': 'process.stdout.write("ok")',
    'modules': 'for(const m of ["fs","http","https","crypto","stream","worker_threads"])require("node:"+m);process.stdout.write("ok")',
}
modes = {
    'parent': [str(args.parent)],
    'snapshot': [str(node)],
    'fallback': [str(node), '--no-node-snapshot'],
}
samples = {mode + '/' + work: [] for mode in modes for work in workloads}
def execute(mode, work):
    command = [*modes[mode], '-e', workloads[work]]
    started = time.perf_counter()
    result = subprocess.run(command, cwd=root, capture_output=True, timeout=15)
    elapsed = (time.perf_counter() - started) * 1000
    assert result.returncode == 0 and result.stdout == b'ok', (command, result)
    return elapsed
load_before = os.getloadavg()
for mode in modes:
    for work in workloads:
        for _ in range(3):
            execute(mode, work)
schedule = [(mode, work) for _ in range(args.samples) for mode in modes for work in workloads]
random.Random(42).shuffle(schedule)
for mode, work in schedule:
    samples[mode + '/' + work].append(execute(mode, work))
summary = {key: dict(median_ms=statistics.median(values),
                     min_ms=min(values), max_ms=max(values)) for key, values in samples.items()}
result = dict(passed=True, scope='fresh process, warm filesystem caches; interleaved samples',
              samples=samples, summary=summary, commands=modes, workloads=workloads,
              parent_sha256=digest(args.parent), node_sha256=digest(node),
              load_before=load_before, load_after=os.getloadavg())
(root / 'out/sparc64-snapshots-startup-benchmark.json').write_text(
    json.dumps(result, indent=2) + '\n')
print(json.dumps(summary, indent=2))
