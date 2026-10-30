// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
d8.file.execute('test/mjsunit/wasm/wasm-module-builder.js');
let checks = 0, nativeFunctions = 0, cases = 0;
function eq(actual, expected) {
  if (!Object.is(actual, expected)) throw new Error(`Expected ${String(expected)}, got ${String(actual)}`);
  ++checks;
}
function native(f) { eq(%IsLiftoffFunction(f), true); ++nativeFunctions; return f; }
const kinds = [kWasmI32, kWasmI64, kWasmF32, kWasmF64, kWasmExternRef];
function value(kind, i) {
  if (kind === kWasmI32) return 2147418112 - i * 37;
  if (kind === kWasmI64) return 0x1234567800000000n + BigInt(i);
  if (kind === kWasmF32 || kind === kWasmF64) return i * 0.5 + 0.25;
  return {index: i};
}
function constant(kind, i) {
  if (kind === kWasmI32) return wasmI32Const(value(kind, i));
  if (kind === kWasmI64) return wasmI64Const(value(kind, i));
  if (kind === kWasmF32) return wasmF32Const(value(kind, i));
  if (kind === kWasmF64) return wasmF64Const(value(kind, i));
  return [kExprRefNull, kExternRefCode];
}
for (const count of [0, 1, 4, 5, 6, 7, 8, 9, 12, 20, 64, 600]) {
  for (const delta of [-1000, -3, 0, 3, 1000]) {
    for (const route of ['direct', 'indirect', 'ref']) {
      const builder = new WasmModuleBuilder();
      const params = Array.from({length: count}, (_, i) => kinds[i % kinds.length]);
      const callerCount = Math.min(1000, Math.max(0, count + delta));
      const callerParams = Array.from({length: callerCount}, (_, i) => kinds[i % kinds.length]);
      const indices = Array.from({length: Math.min(count, 10)}, (_, i) => count - Math.min(count, 10) + i);
      const returns = indices.map(i => params[i]);
      const signature = builder.addType(makeSig(params, returns));
      const callee = builder.addFunction('callee', signature)
        .addBody(indices.flatMap(i => [kExprLocalGet, ...wasmUnsignedLeb(i)]))
        .exportFunc();
      const body = [];
      for (let i = 0; i < count; ++i) {
        body.push(...(i < callerCount ? [kExprLocalGet, ...wasmUnsignedLeb(i)] : constant(params[i], i)));
      }
      if (route === 'direct') body.push(kExprReturnCall, ...wasmUnsignedLeb(callee.index));
      if (route === 'indirect') {
        builder.addTable(kWasmAnyFunc, 1, 1);
        builder.addActiveElementSegment(0, wasmI32Const(0), [callee.index]);
        body.push(...wasmI32Const(0), kExprReturnCallIndirect, ...wasmUnsignedLeb(signature), 0);
      }
      if (route === 'ref') {
        builder.addDeclarativeElementSegment([callee.index]);
        body.push(kExprRefFunc, ...wasmUnsignedLeb(callee.index), kExprReturnCallRef, ...wasmUnsignedLeb(signature));
      }
      builder.addFunction('caller', makeSig(callerParams, returns)).addBody(body).exportFunc();
      const exports = builder.instantiate().exports;
      native(exports.callee); const f = native(exports.caller);
      const args = callerParams.map((kind, i) => value(kind, i));
      const expected = indices.map(i => i < callerCount ? args[i] : params[i] === kWasmExternRef ? null : value(params[i], i));
      for (let iteration = 0; iteration < 3; ++iteration) {
        const raw = f(...args);
        const actual = returns.length === 0 ? [] : returns.length === 1 ? [raw] : raw;
        eq(actual.length, expected.length);
        actual.forEach((v, i) => eq(v, expected[i]));
        gc();
      }
      ++cases;
    }
  }
}
// Return-call recursion must not grow the native stack.
{
  const builder = new WasmModuleBuilder();
  const f = builder.addFunction('countdown', kSig_i_ii);
  f.addBody([kExprLocalGet, 0, kExprI32Eqz, kExprIf, kWasmI32,
    kExprLocalGet, 1, kExprElse, kExprLocalGet, 0, ...wasmI32Const(1), kExprI32Sub,
    kExprLocalGet, 1, ...wasmI32Const(1), kExprI32Add,
    kExprReturnCall, ...wasmUnsignedLeb(f.index), kExprEnd]).exportFunc();
  const countdown = native(builder.instantiate().exports.countdown);
  eq(countdown(100000, 0), 100000);
  ++cases;
}
print(JSON.stringify({passed: true, checks, nativeFunctions, cases}));
