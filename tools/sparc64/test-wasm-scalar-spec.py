#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run the full pinned scalar spec selection with unchanged upstream statuses."""
import argparse,hashlib,json,resource,subprocess,tarfile,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',type=Path,default=root/'out/Release');p.add_argument('--label',default='current')
a=p.parse_args();resource.setrlimit(resource.RLIMIT_CORE,(0,0))
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
sha1=(root/'deps/v8/test/wasm-spec-tests/tests.tar.gz.sha1').read_text().strip()
archive=root/f'out/wasm-upstream-inputs/wasm-spec-tests-{sha1}.tar.gz'
assert hashlib.sha1(archive.read_bytes()).hexdigest()==sha1
base=root/'deps/v8/test/wasm-spec-tests/tests'
selected=[];sources={};outside={}
with tarfile.open(archive) as t:
 for m in t:
  if not m.isfile() or not m.name.endswith('.js'):continue
  relative=Path(m.name).relative_to('tests');path=base/relative
  if len(relative.parts)>1 and relative.parts[0] not in ['memory64','multi-memory','bulk-memory','exceptions']:
   outside[str(relative)]='Deferred SIMD, GC-specific, JSPI or other proposal suite';continue
  assert path.read_bytes()==t.extractfile(m).read()
  selected.append('wasm-spec-tests/'+str(relative.with_suffix('')))
  sources[str(relative)]=digest(path)
assert len(selected)==175,len(selected)
binary=a.build/'d8';before=digest(binary)
prefix=root/('out/sparc64-wasm-scalar-spec-'+a.label)
command=['python3','-u','tools/run-tests.py','--outdir='+str(a.build),'--variants=default',
 '--no-presubmit','-j8','-t90','--progress=verbose','--random-seed=42','--rerun-failures-count=0',
 '--extra-flags=--liftoff-only --no-wasm-lazy-compilation','--json-test-results='+str(prefix)+'-raw.json',*sorted(selected)]
started=time.monotonic()
with prefix.with_suffix('.log').open('w') as log:
 code=subprocess.run(command,cwd=root/'deps/v8',stdout=log,stderr=subprocess.STDOUT,timeout=1200).returncode
raw=json.loads(Path(str(prefix)+'-raw.json').read_text())
assert digest(binary)==before
result=dict(passed=code==0 and not raw['results'] and raw['test_total']==len(selected),binary_sha256=before,
 command=command,exit_code=code,tests=raw['test_total'],selected=sorted(selected),unexpected=raw['results'],
 archive_sha1=sha1,archive_sha256=digest(archive),source_sha256=sources,outside_scope=outside,
 status_sha256=digest(root/'deps/v8/test/wasm-spec-tests/wasm-spec-tests.status'),
 upstream_expected_failures=['table','memory64/memory64','memory64/table64'],seconds=round(time.monotonic()-started,3))
prefix.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({k:result[k] for k in ['passed','tests','exit_code','seconds']}),flush=True)
assert result['passed'],'See scalar spec raw results and log'
print('WASM_SCALAR_SPEC_OK',flush=True)
