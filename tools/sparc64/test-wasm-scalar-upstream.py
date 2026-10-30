#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run unchanged scalar tail-call, reference and exception cases directly."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib,json,re,resource,shlex,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--binary',type=Path,default=root/'out/Release/d8')
p.add_argument('--case',action='append')
p.add_argument('--label',default='current')
a=p.parse_args();resource.setrlimit(resource.RLIMIT_CORE,(0,0))
names=a.case or ['return-calls','return-calls-eh','call-ref','exceptions',
'exceptions-api','exceptions-rethrow','exceptions-externref','exceptions-import',
'exceptions-export','exceptions-type-reflection','exnref','exnref-api',
'exnref-global','exnref-rethrow','type-reflection-exnref']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
output=root/('out/sparc64-wasm-scalar-'+a.label);output.mkdir(parents=True,exist_ok=True)
before=digest(a.binary)
def one(name):
 source='deps/v8/test/mjsunit/wasm/'+name+'.js';path=root/source
 original=subprocess.check_output(['git','show','HEAD:'+source],cwd=root)
 assert path.read_bytes()==original
 flags=[f for line in re.findall(r'^// Flags: (.*)$',original.decode(),re.M) for f in shlex.split(line)]
 cmd=[str(a.binary),'--liftoff-only','--no-wasm-lazy-compilation','--allow-natives-syntax','--expose-gc',*flags,'test/mjsunit/mjsunit.js','test/mjsunit/wasm/'+name+'.js']
 started=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=root/'deps/v8',capture_output=True,timeout=60);code=r.returncode;stdout=r.stdout;stderr=r.stderr
 except subprocess.TimeoutExpired as e:code='timeout';stdout=e.stdout or b'';stderr=e.stderr or b''
 (output/(name+'.stdout')).write_bytes(stdout);(output/(name+'.stderr')).write_bytes(stderr)
 item=dict(name=name,command=cmd,exit_code=code,seconds=round(time.monotonic()-started,3),source_sha256=digest(path))
 print(json.dumps(item),flush=True);return item
with ThreadPoolExecutor(max_workers=4) as pool:cases=list(pool.map(one,names))
assert digest(a.binary)==before
result=dict(passed=all(c['exit_code']==0 for c in cases),binary=str(a.binary),binary_sha256=before,cases=cases)
output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
print('WASM_SCALAR_UPSTREAM_OK' if result['passed'] else 'WASM_SCALAR_FEATURE_GAPS',flush=True)
raise SystemExit(not result['passed'])
