// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const { setImmediate: immediate } = require('node:timers/promises');
vm.runInThisContext(fs.readFileSync(path.resolve(__dirname,
  '../../deps/v8/test/mjsunit/wasm/wasm-module-builder.js'), 'utf8'));
const builder = new WasmModuleBuilder();
const pause = builder.addImport('env', 'pause', kSig_i_i);
const box = builder.addStruct([
  makeField(kWasmS128, true), makeField(kWasmExternRef, true),
]);
const count = 20;
const values = Array.from({length: count}, (_, n) =>
  [0x01020304 + n, 0x11223344 + n, 0x50607080 + n, 0x718293a4 + n]);
const fn = builder.addFunction('run',
  makeSig([kWasmI32, kWasmExternRef], [kWasmI32, kWasmExternRef]))
  .addLocals(kWasmS128, count).addLocals(wasmRefNullType(box), 1);
const body = [];
for (let n = 0; n < count; ++n) {
  const bytes = values[n].flatMap(v =>
    [v & 255, (v >>> 8) & 255, (v >>> 16) & 255, (v >>> 24) & 255]);
  body.push(...wasmS128Const(bytes), kExprLocalSet, n + 2);
}
body.push(kExprLocalGet, 2, kExprLocalGet, 1,
  kGCPrefix, kExprStructNew, box, kExprLocalSet, count + 2);
for (let round = 0; round < 3; ++round) {
  body.push(kExprLocalGet, 0, kExprCallFunction, pause, kExprLocalSet, 0);
}
body.push(kExprLocalGet, count + 2, kGCPrefix, kExprStructGet, box, 0,
  kExprLocalSet, 2, kExprLocalGet, 0);
for (let n = 0; n < count; ++n) {
  for (let lane = 0; lane < 4; ++lane) {
    body.push(kExprI32Const, 5, kExprI32Rol,
      kExprLocalGet, n + 2, ...SimdInstr(kExprI32x4ExtractLane), lane,
      kExprI32Xor);
  }
}
body.push(kExprLocalGet, count + 2, kGCPrefix, kExprStructGet, box, 1);
fn.addBody(body).exportFunc();
let suspensions = 0, checks = 0;
const instance = builder.instantiate({env: {
  pause: new WebAssembly.Suspending(async value => {
    %CheckIsOnCentralStack();
    await immediate();
    gc();
    ++suspensions;
    return value + 1;
  }),
}});
const run = WebAssembly.promising(instance.exports.run);
function expected(input) {
  let checksum = input + 3;
  for (const lanes of values) {
    for (const lane of lanes) {
      checksum = ((checksum << 5) | (checksum >>> 27)) ^ lane;
    }
  }
  return checksum;
}
async function call(input) {
  const reference = {input, data: new Array(100).fill(input)};
  const [checksum, retained] = await run(input, reference);
  assert.equal(checksum, expected(input));
  assert.equal(retained, reference);
  assert.equal(retained.data[99], input);
  checks += 3;
}
async function main() {
  await call(0);
  assert.equal(%IsLiftoffFunction(instance.exports.run), true);
  for (let base = 0; base < 16; base += 4) {
    await Promise.all(Array.from({length: 4}, (_, n) => call(base + n)));
  }
  %WasmTierUpFunction(instance.exports.run);
  await call(17);
  assert.equal(%IsTurboFanFunction(instance.exports.run), true);
  for (let base = 18; base < 34; base += 4) {
    await Promise.all(Array.from({length: 4}, (_, n) => call(base + n)));
  }
  gc();
  console.log('WASM_PRODUCTION_MIXED_OK ' + JSON.stringify({
    passed: true, calls: 34, checks, suspensions, vectors: count,
    concurrentStacks: 4, liftoff: true, optimized: true, wasmGC: true,
  }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
