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
let checks = 0, suspensions = 0, nativeFunctions = 0, rejections = 0;
function equal(actual, expected) { assert.equal(actual, expected); ++checks; }
function native(instance) {
  for (const value of Object.values(instance.exports)) {
    if (typeof value === 'function') {
      equal(%IsLiftoffFunction(value), true);
      ++nativeFunctions;
    }
  }
  return instance;
}
async function pause(value) {
  %CheckIsOnCentralStack();
  await immediate();
  gc();
  ++suspensions;
  return value + 1;
}
function unary(imported) {
  const builder = new WasmModuleBuilder();
  const index = builder.addImport('env', 'step', kSig_i_i);
  builder.addFunction('run', kSig_i_i).addBody([
    kExprLocalGet, 0, kExprCallFunction, index,
  ]).exportFunc();
  return native(builder.instantiate({env: {step: new WebAssembly.Suspending(imported)}}));
}
function loopModule() {
  const builder = new WasmModuleBuilder();
  const index = builder.addImport('env', 'step', kSig_i_i);
  builder.addFunction('run', makeSig([kWasmI32, kWasmExternRef],
                                    [kWasmI32, kWasmExternRef]))
    .addLocals(kWasmI32, 2)
    .addBody([
      kExprBlock, kWasmVoid,
        kExprLoop, kWasmVoid,
          kExprLocalGet, 2, kExprLocalGet, 0, kExprI32GeS, kExprBrIf, 1,
          kExprLocalGet, 3, kExprLocalGet, 2, kExprCallFunction, index,
          kExprI32Add, kExprLocalSet, 3,
          kExprLocalGet, 2, ...wasmI32Const(1), kExprI32Add, kExprLocalSet, 2,
          kExprBr, 0,
        kExprEnd,
      kExprEnd,
      kExprLocalGet, 3, kExprLocalGet, 1,
    ]).exportFunc();
  return native(builder.instantiate({env: {step: new WebAssembly.Suspending(pause)}}));
}
async function main() {
  equal(typeof WebAssembly.promising, 'function');
  equal(typeof WebAssembly.Suspending, 'function');
  const looping = WebAssembly.promising(loopModule().exports.run);
  function launch(count, id) {
    const value = { id, payload: new Array(100).fill(id) };
    return { weak: new WeakRef(value), promise: looping(count, value) };
  }
  const repeated = launch(128, 'repeated');
  gc();
  const [sum, retained] = await repeated.promise;
  equal(sum, 128 * 129 / 2);
  equal(retained.id, 'repeated');
  equal(retained, repeated.weak.deref());
  equal(retained.payload[99], 'repeated');
  const concurrent = Array.from({length: 8}, (_, id) => launch(16, id));
  gc();
  const results = await Promise.all(concurrent.map(item => item.promise));
  for (let id = 0; id < results.length; id++) {
    equal(results[id][0], 16 * 17 / 2);
    equal(results[id][1].id, id);
    equal(results[id][1], concurrent[id].weak.deref());
  }
  const reason = new Error('JSPI rejection identity');
  const rejecting = WebAssembly.promising(unary(async value => {
    await pause(value);
    throw reason;
  }).exports.run);
  const passing = WebAssembly.promising(unary(pause).exports.run);
  for (let round = 0; round < 16; round++) {
    await assert.rejects(rejecting(round), error => error === reason);
    ++rejections; ++checks;
    equal(await passing(round), round + 1);
  }
  let nested = passing;
  for (let depth = 0; depth < 8; depth++) {
    const inner = nested;
    nested = WebAssembly.promising(unary(async value => {
      %CheckIsOnCentralStack();
      const result = await inner(value);
      gc();
      return result + 1;
    }).exports.run);
  }
  for (let round = 0; round < 8; round++) equal(await nested(round), round + 9);
  for (let round = 0; round < 64; round++) equal(await passing(round), round + 1);
  const catchBuilder = new WasmModuleBuilder();
  const jsTag = catchBuilder.addImportedTag('env', 'tag', kSig_v_r);
  const failing = catchBuilder.addImport('env', 'fail', kSig_i_i);
  catchBuilder.addFunction('run', kSig_i_i).addBody([
    kExprTry, kWasmI32,
      kExprLocalGet, 0, kExprCallFunction, failing,
    kExprCatch, jsTag,
      kExprDrop, ...wasmI32Const(777),
    kExprEnd,
  ]).exportFunc();
  const catching = native(catchBuilder.instantiate({env: {
    tag: WebAssembly.JSTag,
    fail: new WebAssembly.Suspending(async value => {
      await pause(value);
      throw reason;
    }),
  }}));
  equal(await WebAssembly.promising(catching.exports.run)(9), 777);
  const sync = WebAssembly.promising(unary(value => value * 3).exports.run);
  equal(await sync(14), 42);
  assert.throws(() => passing(Symbol('invalid numeric parameter')), TypeError);
  ++checks;
  equal(await passing(4), 5);
  gc();
  console.log('JSPI_RUNTIME_OK', JSON.stringify({
    passed: true, checks, suspensions, nativeFunctions, rejections,
    repeatedSuspensions: 128, concurrentStacks: 8, nestedDepth: 8, stackReuse: 64,
  }));
}
main().catch(error => { console.error(error); process.exitCode = 1; });
