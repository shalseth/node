#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Run unchanged upstream JSPI cases with their original feature flags."""
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
names=a.case or ['jspi','jspi-export','jspi-generic-wrapper','jspi-no-stress','jspi-notraps',
 'regress/wasm/regress-336358915','regress/wasm/regress-336852356',
 'regress/wasm/regress-342522151','regress/wasm/regress-361123483',
 'regress/wasm/regress-361717714','regress/wasm/regress-364667545',
 'regress/wasm/regress-365376497','regress/wasm/regress-381917890',
 'regress/wasm/regress-392928805','regress/wasm/regress-406043349',
 'regress/wasm/regress-407797300','regress/wasm/regress-408254017',
 'regress/wasm/regress-v8-14471']
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
output=root/('out/sparc64-jspi-'+a.label);output.mkdir(parents=True,exist_ok=True)
before=digest(a.binary)
def one(name):
 relative=(name if '/' in name else 'wasm/'+name)+'.js'
 source='deps/v8/test/mjsunit/'+relative;path=root/source
 original=subprocess.check_output(['git','show','HEAD:'+source],cwd=root)
 assert path.read_bytes()==original
 flags=[f for line in re.findall(r'^// Flags: (.*)$',original.decode(),re.M) for f in shlex.split(line)]
 cmd=[str(a.binary),'--liftoff-only','--no-wasm-lazy-compilation','--allow-natives-syntax','--expose-gc',*flags,'test/mjsunit/mjsunit.js','test/mjsunit/'+relative]
 started=time.monotonic()
 try:
  r=subprocess.run(cmd,cwd=root/'deps/v8',capture_output=True,timeout=60);code=r.returncode;stdout=r.stdout;stderr=r.stderr
 except subprocess.TimeoutExpired as e:code='timeout';stdout=e.stdout or b'';stderr=e.stderr or b''
 leaf=name.replace('/','__')
 (output/(leaf+'.stdout')).write_bytes(stdout);(output/(leaf+'.stderr')).write_bytes(stderr)
 item=dict(name=name,command=cmd,exit_code=code,seconds=round(time.monotonic()-started,3),source_sha256=digest(path))
 print(json.dumps(item),flush=True);return item
with ThreadPoolExecutor(max_workers=4) as pool:cases=list(pool.map(one,names))
assert digest(a.binary)==before
result=dict(passed=all(c['exit_code']==0 for c in cases),binary=str(a.binary),binary_sha256=before,cases=cases)
output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
print('JSPI_UPSTREAM_OK' if result['passed'] else 'JSPI_FEATURE_GAPS',flush=True)
raise SystemExit(not result['passed'])
