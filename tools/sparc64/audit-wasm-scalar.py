#!/usr/bin/env python3
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
"""Require scalar feature/application acceptance and preserved Node/JIT evidence."""
import argparse,hashlib,json,re,subprocess,tarfile
from pathlib import Path
root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--allow-jspi',action='store_true')
parser.add_argument('--allow-production', action='store_true')
args=parser.parse_args()
if args.allow_production:
    args.allow_jspi = True
    from wasm_production_validation import validate_sources
    validate_sources(Path(__file__).resolve().parents[2])
digest=lambda p:hashlib.sha256(p.read_bytes()).hexdigest()
read=lambda name:json.loads((root/'out'/name).read_text())
subprocess.run(['python3','tools/sparc64/audit-wasm-atomics.py',
 *(['--allow-production'] if args.allow_production else ['--allow-jspi'] if args.allow_jspi else [])],cwd=root,check=True)
current={name:digest(root/'out/Release'/name) for name in ['node','d8']}
snapshot_keys=read('sparc64-snapshot-array-buffer-keys-current.json')
assert snapshot_keys['passed'] and snapshot_keys['binary_sha256']==current['node']
assert snapshot_keys['cases']==27 and len(snapshot_keys['results'])==12
assert digest(root/snapshot_keys['fixture'])==snapshot_keys['fixture_sha256']
assert {(r['tracking'],r['verify_heap'],r['phase']) for r in snapshot_keys['results']}=={
 (tracking,verify,phase) for tracking in [False,True] for verify in [False,True]
 for phase in ['build','restore-0','restore-1']}
assert all(r['passed'] and r['exit_code']==0 for r in snapshot_keys['results'])
snapshot_before=read('sparc64-snapshot-array-buffer-keys-baseline.json')
assert not snapshot_before['passed'] and len(snapshot_before['results'])==12
assert snapshot_before['binary_sha256']!=current['node']
assert snapshot_before['fixture_sha256']==snapshot_keys['fixture_sha256']
assert snapshot_before['fixture']==snapshot_keys['fixture'] and snapshot_before['cases']==27
before_directory=root/'out/sparc64-snapshot-before-fix'
before_manifest=json.loads((before_directory/'manifest.json').read_text())
assert digest(before_directory/'node')==before_manifest['node_sha256']==snapshot_before['binary_sha256']
before_build=json.loads((before_directory/'sparc64-wasm-build.json').read_text())[-1]
assert before_build['binary_sha256']['node']==snapshot_before['binary_sha256']
current_build=read('sparc64-wasm-build.json')[-1]
before_sources=dict(before_build['source_sha256'])
assert before_sources['deps/v8/src/snapshot/deserializer.cc']==before_manifest['deserializer_sha256']
before_sources['deps/v8/src/snapshot/deserializer.cc']=digest(root/'deps/v8/src/snapshot/deserializer.cc')
if args.allow_jspi:
 scalar_parent=root.with_name('node-sparc64-wasm-scalar')
 scalar_checkpoint=Path((scalar_parent/'out/sparc64-wasm-scalar-checkpoint-path.txt').read_text().strip())
 scalar_manifest=json.loads((scalar_checkpoint/'manifest.json').read_text())
 assert scalar_manifest==read('sparc64-jspi-parent.json') and scalar_manifest['profile']=='wasm-scalar'
 scalar_build=json.loads((scalar_checkpoint/'validation/sparc64-wasm-build.json').read_text())[-1]
 assert before_sources==scalar_build['source_sha256']
 expected_sources=dict(scalar_build['source_sha256'])
 for source in ['deps/v8/src/builtins/sparc64/builtins-sparc64.cc',
                'deps/v8/src/builtins/setup-builtins-internal.cc']:
  expected_sources[source]=digest(root/source)
 if args.allow_production:
  validate_sources(root)
 else:
  assert expected_sources==current_build['source_sha256']
else:
 assert before_sources==current_build['source_sha256']
before_state=json.loads((before_directory/'sparc64-snapshots-state.json').read_text())
assert not before_state['passed'] and before_state['binary_sha256']==snapshot_before['binary_sha256']
assert len(before_state['results'])==12
failed_restores=[r for r in before_state['results'] if r['name'].startswith('restore-')]
assert len(failed_restores)==3 and all(not r['passed'] and r['exit']==-4 for r in failed_restores)
for item in failed_restores:
 assert b'has_detach_key() || v == kNoView' in (
  before_directory/('sparc64-snapshots-state-'+item['name']+'.log')).read_bytes()
fixed_state=read('sparc64-snapshots-state.json')
assert fixed_state['passed'] and fixed_state['binary_sha256']==current['node']
assert len(fixed_state['results'])==12 and all(r['passed'] and r['exit']==0 for r in fixed_state['results'])
assert {(r['tracking'],r['verify_heap'],r['phase']) for r in snapshot_before['results']}=={
 (tracking,verify,phase) for tracking in [False,True] for verify in [False,True]
 for phase in ['build','restore-0','restore-1']}
for item in snapshot_before['results']:
 if item['phase']=='build':
  assert item['passed'] and item['exit_code']==0
 else:
  assert not item['passed'] and item['exit_code'] in [-4,1]
  log=root/'out/sparc64-snapshot-array-buffer-keys-baseline'/(
   f'track-{int(item["tracking"])}-verify-{int(item["verify_heap"])}-{item["phase"]}.log')
  data=log.read_bytes()
  assert b'has_detach_key() || v == kNoView' in data or b'Missing expected exception (TypeError)' in data
acceptance=read('sparc64-wasm-scalar-acceptance.json')
assert acceptance['passed'] and acceptance['binary_sha256']==current
assert [r['name'] for r in acceptance['results']]==['upstream','spec','matrix','target-oracle-build','application']
for item in acceptance['results']:
 assert item['exit_code']==0
 if 'fixture' in item:assert digest(root/item['fixture'])==item['fixture_sha256']
rawdir=root/'out/sparc64-wasm-scalar-acceptance'
matrix=next(r['data'] for r in acceptance['results'] if r['name']=='matrix')
assert matrix==json.loads((rawdir/'matrix.stdout').read_text().splitlines()[-1])
assert matrix==dict(passed=True,checks=4502,nativeFunctions=361,cases=181)
features=read('sparc64-wasm-scalar-current.json')
assert features['passed'] and features['binary_sha256']==current['d8']
expected={'return-calls','return-calls-eh','call-ref','exceptions','exceptions-api',
 'exceptions-rethrow','exceptions-externref','exceptions-import','exceptions-export',
 'exceptions-type-reflection','exnref','exnref-api','exnref-global','exnref-rethrow','type-reflection-exnref'}
assert len(features['cases'])==15 and {c['name'] for c in features['cases']}==expected
for item in features['cases']:
 assert item['exit_code']==0
 source='deps/v8/test/mjsunit/wasm/'+item['name']+'.js'
 assert digest(root/source)==item['source_sha256']
 assert (root/source).read_bytes()==subprocess.check_output(['git','show','HEAD:'+source],cwd=root)
spec=read('sparc64-wasm-scalar-spec-current.json')
assert spec['passed'] and spec['binary_sha256']==current['d8'] and spec['tests']==175 and not spec['unexpected']
assert len(spec['selected'])==len(set(spec['selected']))==175
assert spec['upstream_expected_failures']==['table','memory64/memory64','memory64/table64']
status='deps/v8/test/wasm-spec-tests/wasm-spec-tests.status'
assert digest(root/status)==spec['status_sha256']
assert (root/status).read_bytes()==subprocess.check_output(['git','show','HEAD:'+status],cwd=root)
raw=read('sparc64-wasm-scalar-spec-current-raw.json')
assert raw['test_total']==175 and not raw['results']
archive=root/f'out/wasm-upstream-inputs/wasm-spec-tests-{spec["archive_sha1"]}.tar.gz'
assert digest(archive)==spec['archive_sha256']
with tarfile.open(archive) as stream:
 selected=[]
 for entry in stream:
  if not entry.isfile() or not entry.name.endswith('.js'):continue
  relative=Path(entry.name).relative_to('tests')
  if len(relative.parts)>1 and relative.parts[0] not in ['memory64','multi-memory','bulk-memory','exceptions']:continue
  selected.append('wasm-spec-tests/'+str(relative.with_suffix('')))
  path=root/'deps/v8/test/wasm-spec-tests/tests'/relative
  assert path.read_bytes()==stream.extractfile(entry).read()
  assert digest(path)==spec['source_sha256'][str(relative)]
 assert sorted(selected)==spec['selected']
app=next(r['data'] for r in acceptance['results'] if r['name']=='application')
assert app['passed'] and app['cases']==56 and app['invocations']==168 and app['checks']==504
assert app['errors']==app['caught']==15 and app['nativeFunctions']==15 and app['tailDepth']==20000
assert app['growthNotifications']>=1
assert app==read('sparc64-wasm-scalar-application-result.json')
appdir=root/'out/wasm-scalar-application'
build=json.loads((appdir/'build-manifest.json').read_text())
assert build['library']['commit']==app['library']=='9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03'
assert digest(appdir/'include/nlohmann/json.hpp')=='9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6'
assert digest(appdir/'LICENSE.MIT')=='86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14'
for source,value in build['source_sha256'].items():assert digest(root/source)==value
for name,value in build['files'].items():assert digest(appdir/name)==value
assert digest(appdir/'json-report.wasm')==app['module_sha256']
wat=(appdir/'json-report.wat').read_text()
assert re.search(r'^\s+throw \d',wat,re.M) and re.search(r'^\s+catch \d',wat,re.M)
assert 'v128' not in wat and '"invoke_' not in wat
oracle=json.loads((appdir/'target-oracle.json').read_text())
assert oracle['passed'] and oracle['machine']=='sparc64'
assert oracle['source_sha256']==build['source_sha256']
assert digest(appdir/'target-json-report-native')==oracle['binary_sha256']
assert digest(appdir/'native-reference.json')==oracle['reference_sha256']==app['reference_sha256']
assert digest(appdir/'cross-reference.json')==oracle['cross_reference_sha256']
assert json.loads((appdir/'native-reference.json').read_text())==json.loads((appdir/'cross-reference.json').read_text())
compiler_source='deps/v8/src/wasm/baseline/liftoff-compiler.cc'
original=subprocess.check_output(['git','show','HEAD:'+compiler_source],cwd=root)
old_load=b'''      __ LoadFullPointer(target_reg, internal_function,
                         wasm::ObjectAccess::ToTagged(
                             WasmInternalFunction::kRawCallTargetOffset));'''
assert original.count(old_load)==1
assert (root/compiler_source).read_bytes()==original.replace(old_load,old_load.replace(b'LoadFullPointer',b'LoadCodePointer'),1)
for support in ['deps/v8/test/mjsunit/mjsunit.js','deps/v8/test/mjsunit/wasm/wasm-module-builder.js']:
 assert (root/support).read_bytes()==subprocess.check_output(['git','show','HEAD:'+support],cwd=root)
parent=root.with_name('node-sparc64-wasm-atomics')
checkpoint=Path((parent/'out/sparc64-wasm-atomics-checkpoint-path.txt').read_text().strip())
manifest=json.loads((checkpoint/'manifest.json').read_text())
assert manifest==read('sparc64-wasm-scalar-parent.json') and manifest['profile']=='wasm-atomics'
initial=read('sparc64-wasm-scalar-spec-baseline.json')
assert not initial['passed'] and initial['tests']==175
assert initial['binary_sha256']==manifest['artifact_sha256']['d8']
assert {r['name'] for r in initial['unexpected']}=={
 'wasm-spec-tests/br_on_non_null','wasm-spec-tests/br_on_null','wasm-spec-tests/call_ref',
 'wasm-spec-tests/ref_as_non_null','wasm-spec-tests/return_call','wasm-spec-tests/return_call_ref',
 'wasm-spec-tests/return_call_indirect','wasm-spec-tests/exceptions/try_table'}
header='deps/v8/src/wasm/baseline/sparc64/liftoff-assembler-sparc64-inl.h'
with tarfile.open(checkpoint/'source-overlay.tar.gz') as stream:
 parent_header=stream.extractfile(header).read().decode()
 deserializer='deps/v8/src/snapshot/deserializer.cc'
 parent_deserializer=stream.extractfile(deserializer).read()
assert parent_deserializer.count(b'                    buffer->views());')==1
assert (root/deserializer).read_bytes()==parent_deserializer.replace(
 b'                    buffer->views());',b'                    buffer->views_or_detach_key());',1)
current_header=(root/header).read_text()
for method in ['PrepareTailCall','TailCallIndirect']:
 pattern=r'void LiftoffAssembler::'+method+r'\(.*?\n\}'
 before_method=re.search(pattern,parent_header,re.S)[0]
 after_method=re.search(pattern,current_header,re.S)[0]
 current_header=current_header.replace(after_method,before_method,1)
if args.allow_production:
 validate_sources(root)
else:
 assert current_header==parent_header
subprocess.run(['python3',str(parent/'tools/sparc64/verify-checkpoint.py'),str(checkpoint),'--source-root',str(parent)],check=True)
for name in ['node','d8','cctest','node_mksnapshot']:
 assert digest(parent/'out/Release'/name)==manifest['artifact_sha256'][name]
for source,name in [('config.gypi','config.gypi'),('out/Release/v8_build_config.json','v8_build_config.json')]:
 assert digest(parent/source)==manifest['artifact_sha256'][name]
result=dict(passed=True,binary_sha256=current,scalar_spec_cases=175,feature_cases=15,
 matrix=matrix,application=app,parent_checkpoint=str(checkpoint),baseline_unexpected_fixed=8)
(root/'out/sparc64-wasm-scalar-audit.json').write_text(json.dumps(result,indent=2)+'\n')
print('WASM_SCALAR_AUDIT_OK',flush=True)
