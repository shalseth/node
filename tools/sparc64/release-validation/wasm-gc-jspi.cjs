'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
require('node:vm').runInThisContext(fs.readFileSync(path.join(__dirname, 'wasm-module-builder.js'), 'utf8'));
const tier = process.env.WASM_SIMD_TIER;
const b = new WasmModuleBuilder();
const struct = b.addStruct([makeField(kWasmI64, true), makeField(kWasmExternRef, true)]);
const pause = b.addImport('env', 'pause', makeSig([], []));
const check = b.addImport('env', 'check', makeSig([kWasmExternRef], [kWasmI64]));
b.addFunction('hold', makeSig([kWasmI64, kWasmExternRef], [kWasmI64]))
  .addLocals(wasmRefType(struct), 1).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kGCPrefix, kExprStructNew, struct,
    kExprLocalSet, 2,
    kExprCallFunction, pause,
    kExprLocalGet, 2, kGCPrefix, kExprStructGet, struct, 0,
    kExprLocalGet, 2, kGCPrefix, kExprStructGet, struct, 1,
    kExprCallFunction, check, kExprI64Add,
  ]).exportFunc();
let pauses = 0;
const instance = b.instantiate({env: {
  pause: new WebAssembly.Suspending(async () => {
    gc();
    await new Promise(resolve => setImmediate(resolve));
    gc();
    pauses++;
  }),
  check: object => { assert.equal(object.text, 'SPARC64 € 🚀'); return object.value; },
}});
const native = instance.exports.hold;
const run = WebAssembly.promising(native);
(async () => {
  let calls = 0;
  for (let round = 0; round < 16; round++) {
    await Promise.all(Array.from({length: 4}, async (_, i) => {
      const value = BigInt(round * 4 + i);
      const result = await run(value * 13n, {value: value * 7n, text: 'SPARC64 € 🚀'});
      assert.equal(result, value * 20n);
      calls++;
    }));
  }
  assert.equal(tier === 'liftoff' ? %IsLiftoffFunction(native) : %IsTurboFanFunction(native), true);
  assert.equal(calls, 64);
  assert.equal(pauses, 64);
  console.log('WASM_GC_JSPI_INSTALLED_OK ' + JSON.stringify({passed: true, calls, pauses, tier}));
})().catch(error => { console.error(error); process.exitCode = 1; });
