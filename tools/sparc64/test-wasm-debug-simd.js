// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
utils.load('test/inspector/wasm-inspector-test.js');
const {session, contextGroup, Protocol} =
    InspectorTest.start('Inspect distinct SIMD lanes across registers and spills.');
session.setupScriptMap();
const count = 20;
const values = Array.from({length: count}, (_, n) =>
    [0x01020304 + n, 0x11223344 + n, 0x50607080 + n, 0x718293a4 + n]);
const builder = new WasmModuleBuilder();
const fn = builder.addFunction('probe', kSig_i_v)
    .addLocals(kWasmS128, count,
               Array.from({length: count}, (_, n) => 'vector' + n));
const body = [];
for (let n = 0; n < count; ++n) {
  const bytes = values[n].flatMap(v =>
      [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
  body.push(...wasmS128Const(bytes), kExprLocalSet, n);
}
const stopOffset = body.length;
body.push(kExprLocalGet, 0, kExprDrop, kExprI32Const, 0);
let expectedChecksum = 0;
for (let n = 0; n < count; ++n) {
  for (let lane = 0; lane < 4; ++lane) {
    body.push(kExprI32Const, 5, kExprI32Rol,
              kExprLocalGet, n, kSimdPrefix, kExprI32x4ExtractLane, lane,
              kExprI32Xor);
    expectedChecksum = ((expectedChecksum << 5) | (expectedChecksum >>> 27)) ^
                       values[n][lane];
  }
}
fn.addBody(body).exportFunc();
const debugSimdBytes = builder.toArray();
function check(condition, message) {
  if (!condition) throw new Error(message);
}
InspectorTest.runAsyncTestSuite([async function distinctLanes() {
  await Protocol.Runtime.enable();
  await Protocol.Debugger.enable();
  const parsed = Protocol.Debugger.onceScriptParsed(2);
  const instantiated = WasmInspectorTest.instantiate(debugSimdBytes);
  const [, {params: script}] = await parsed;
  await instantiated;
  check(script.url.startsWith('wasm://'), 'Expected a Wasm script');
  const breakpoint = await Protocol.Debugger.setBreakpoint({location: {
    scriptId: script.scriptId, lineNumber: 0,
    columnNumber: fn.body_offset + stopOffset
  }});
  check(!breakpoint.error, 'Could not set breakpoint');
  const paused = Protocol.Debugger.oncePaused();
  const execution = WasmInspectorTest.evalWithUrl(
      'instance.exports.probe()', 'debug_simd_probe');
  const {params: pause} = await paused;
  let checks = 0;
  let failure;
  try {
    for (let round = 0; round < 2; ++round) {
      if (round) {
        const gc = await Protocol.HeapProfiler.collectGarbage();
        check(!gc.error, 'GC while paused failed');
      }
      const scope = pause.callFrames[0].scopeChain.find(s => s.type === 'local');
      check(scope, 'Missing Wasm local scope');
      const properties = await Protocol.Runtime.getProperties({
        objectId: scope.object.objectId
      });
      check(properties.result, 'Could not inspect locals');
      for (let n = 0; n < count; ++n) {
        const item = properties.result.result.find(p => p.name === '$vector' + n);
        check(item, 'Missing vector ' + n);
        const actual = await WasmInspectorTest.getWasmValue(item.value);
        const expected = 'i32x4 ' + values[n].map(v =>
            '0x' + v.toString(16).toUpperCase().padStart(8, '0')).join(' ') +
            ' (v128)';
        check(actual === expected, 'vector ' + n + ': ' + actual +
                                   ', expected ' + expected);
        ++checks;
      }
    }
  } catch (error) {
    failure = error;
  }
  await Protocol.Debugger.resume();
  const result = await execution;
  check(!failure, String(failure));
  check(result.result && !result.result.exceptionDetails,
        'Wasm did not resume successfully');
  check(result.result.result.value === expectedChecksum,
        'SIMD lanes changed across the debugger pause');
  InspectorTest.log('WASM_DEBUG_SIMD_OK ' + JSON.stringify({
    passed: true, vectors: count, checks, gcWhilePaused: true,
    checksum: expectedChecksum
  }));
}]);
