#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Build the pinned JSON report application with genuine Wasm C++ exceptions."""
import hashlib,json,os,subprocess
from pathlib import Path
root=Path(__file__).resolve().parents[2]
out=root/'out/wasm-scalar-application';out.mkdir(parents=True,exist_ok=True)
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
inputs=json.loads((out/'source-manifest.json').read_text())
for name,record in inputs['files'].items():assert digest(out/name)==record['sha256']
source='tools/sparc64/wasm-json-report.cc';before=digest(root/source)
emxx=os.environ.get('EMXX','em++');wasm2wat=os.environ.get('WASM2WAT','wasm2wat')
command=[emxx,'-std=c++17','-O2','-fwasm-exceptions','-sSTANDALONE_WASM=1',
 '-sALLOW_MEMORY_GROWTH=1','--no-entry',
 '-sEXPORTED_FUNCTIONS=["_process","_result_data","_result_size","_caught_count","_malloc","_free"]',
 '-I'+str(out/'include'),source,'-o',str(out/'json-report.wasm')]
with (out/'build.log').open('w') as log:subprocess.run(command,cwd=root,check=True,stdout=log,stderr=subprocess.STDOUT,timeout=180)
native=[os.environ.get('CXX','g++'),'-std=c++17','-O2','-DSCALAR_APP_NATIVE',
 '-I'+str(out/'include'),source,'-o',str(out/'json-report-native')]
subprocess.run(native,cwd=root,check=True,timeout=180)
subprocess.run([wasm2wat,'--enable-exceptions',str(out/'json-report.wasm'),'-o',str(out/'json-report.wat')],check=True)
wat=(out/'json-report.wat').read_text()
assert 'throw ' in wat and 'catch ' in wat and 'try ' in wat
assert digest(root/source)==before
manifest=dict(command=command,native_command=native,compiler_version=subprocess.check_output([emxx,'--version'],text=True),
 native_compiler_version=subprocess.check_output([native[0],'--version'],text=True),
 source_sha256={source:before},library=inputs,
 files={name:digest(out/name) for name in ['json-report.wasm','json-report.wat','json-report-native']},
 exception_instructions=dict(throws=wat.count('throw '),catches=wat.count('catch '),tries=wat.count('try ')))
(out/'build-manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
print('WASM_SCALAR_APPLICATION_BUILD_OK',flush=True)
