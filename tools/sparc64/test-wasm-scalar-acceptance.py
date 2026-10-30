#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Validate scalar features and the real C++ application against a target oracle."""
import hashlib,json,os,resource,shutil,subprocess,time
from pathlib import Path
root=Path(__file__).resolve().parents[2]
resource.setrlimit(resource.RLIMIT_CORE,(0,0))
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
current={n:digest(root/'out/Release'/n) for n in ['node','d8']}
output=root/'out/sparc64-wasm-scalar-acceptance';output.mkdir(parents=True,exist_ok=True)
records=[]
def run(name,command,timeout=180,cwd=root,env=None,fixture=None):
 before=digest(root/fixture) if fixture else None
 start=time.monotonic()
 r=subprocess.run(command,cwd=cwd,env=env,capture_output=True,timeout=timeout)
 (output/(name+'.stdout')).write_bytes(r.stdout);(output/(name+'.stderr')).write_bytes(r.stderr)
 item=dict(name=name,command=command,cwd=str(cwd),exit_code=r.returncode,seconds=round(time.monotonic()-start,3))
 if fixture:
  assert digest(root/fixture)==before
  item.update(fixture=fixture,fixture_sha256=before)
 if name=='matrix' and r.returncode==0:item['data']=json.loads(r.stdout.decode().splitlines()[-1])
 if name=='application' and r.returncode==0:
  assert b'WASM_SCALAR_APPLICATION_OK' in r.stdout
  item['data']=json.loads((root/'out/sparc64-wasm-scalar-application-result.json').read_text())
 records.append(item)
 (root/'out/sparc64-wasm-scalar-acceptance.json').write_text(json.dumps(dict(passed=False,binary_sha256=current,results=records),indent=2)+'\n')
 print(json.dumps(item),flush=True)
 assert r.returncode==0,(r.stdout+r.stderr)[-12000:].decode(errors='replace')
 return item
run('upstream',['python3','-u','tools/sparc64/test-wasm-scalar-upstream.py'])
run('spec',['python3','-u','tools/sparc64/test-wasm-scalar-spec.py'])
run('matrix',[str(root/'out/Release/d8'),'--liftoff-only','--no-wasm-lazy-compilation',
 '--allow-natives-syntax','--expose-gc','--verify-heap','--stack-size=128',
 '../../tools/sparc64/test-wasm-scalar.js'],cwd=root/'deps/v8',fixture='tools/sparc64/test-wasm-scalar.js')
app=root/'out/wasm-scalar-application'
shutil.copy2(app/'native-reference.json',app/'cross-reference.json')
source='tools/sparc64/wasm-json-report.cc'
manifest=json.loads((app/'build-manifest.json').read_text())
assert manifest['source_sha256'][source]==digest(root/source)
run('target-oracle-build',['g++','-std=c++17','-O2','-DSCALAR_APP_NATIVE',
 '-I'+str(app/'include'),source,'-o',str(app/'target-json-report-native')],fixture=source)
env=dict(os.environ,SCALAR_NATIVE_ORACLE=str(app/'target-json-report-native'))
run('application',[str(root/'out/Release/node'),'--liftoff-only','--no-wasm-lazy-compilation',
 '--allow-natives-syntax','--expose-gc','--verify-heap','--no-warnings',
 'tools/sparc64/test-wasm-scalar-application.cjs'],env=env,fixture='tools/sparc64/test-wasm-scalar-application.cjs')
assert json.loads((app/'cross-reference.json').read_text())==json.loads((app/'native-reference.json').read_text())
(app/'target-oracle.json').write_text(json.dumps(dict(passed=True,machine=os.uname().machine,
 command=records[-2]['command'],compiler_version=subprocess.check_output(['g++','--version'],text=True),
 source_sha256=manifest['source_sha256'],binary_sha256=digest(app/'target-json-report-native'),
 reference_sha256=digest(app/'native-reference.json'),cross_reference_sha256=digest(app/'cross-reference.json')),indent=2)+'\n')
assert all(digest(root/'out/Release'/n)==v for n,v in current.items())
result=dict(passed=True,binary_sha256=current,results=records)
(root/'out/sparc64-wasm-scalar-acceptance.json').write_text(json.dumps(result,indent=2)+'\n')
print('WASM_SCALAR_ACCEPTANCE_OK',flush=True)
