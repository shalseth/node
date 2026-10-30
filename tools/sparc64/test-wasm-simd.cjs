// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
vm.runInThisContext(fs.readFileSync(path.resolve(__dirname,
  '../../deps/v8/test/mjsunit/wasm/wasm-module-builder.js'), 'utf8'));
const tier = process.env.WASM_SIMD_TIER || 'liftoff';
const builder = new WasmModuleBuilder();
builder.addMemory(1, 1, false);
builder.exportMemoryAs('memory');
const operations = [];
for (const bits of [8, 16, 32, 64]) {
  const prefix = `I${bits}x${128 / bits}`;
  for (const op of ['Add', 'Sub', 'Mul', 'Eq', 'Ne', 'LtS', 'LtU', 'LeS', 'LeU',
                    'GtS', 'GtU', 'GeS', 'GeU', 'MinS', 'MinU', 'MaxS', 'MaxU']) {
    const constant = `kExpr${prefix}${op}`;
    let opcode;
    try { opcode = vm.runInThisContext(constant); } catch { continue; }
    const name = `${prefix}_${op}`;
    builder.addFunction(name, makeSig([kWasmI32, kWasmI32, kWasmI32], []))
      .addBody([
        kExprLocalGet, 2,
        kExprLocalGet, 0, ...SimdInstr(kExprS128LoadMem), 0, 0,
        kExprLocalGet, 1, ...SimdInstr(kExprS128LoadMem), 0, 0,
        ...SimdInstr(opcode), ...SimdInstr(kExprS128StoreMem), 0, 0,
      ]).exportFunc();
    operations.push({name, bits, op});
  }
}
assert.equal(operations.length, 59);
const instance = builder.instantiate();
const memory = new Uint8Array(instance.exports.memory.buffer);
let checks = 0, invocations = 0, nativeFunctions = 0;
for (const {name} of operations) {
  assert.equal(tier === 'liftoff' ? %IsLiftoffFunction(instance.exports[name]) :
                                  %IsTurboFanFunction(instance.exports[name]), true, name);
  ++nativeFunctions;
}
let seed = 0x12345678;
function random32() {
  seed ^= seed << 13; seed ^= seed >>> 17; seed ^= seed << 5;
  return seed >>> 0;
}
function put(bytes, offset, bits, value) {
  value = BigInt.asUintN(bits, value);
  for (let n = 0; n < bits / 8; ++n) bytes[offset + n] = Number((value >> BigInt(n * 8)) & 255n);
}
function reference(op, bits, a, b) {
  const signed = op.endsWith('S');
  const x = signed ? BigInt.asIntN(bits, a) : a;
  const y = signed ? BigInt.asIntN(bits, b) : b;
  switch (op) {
    case 'Add': return a + b;
    case 'Sub': return a - b;
    case 'Mul': return a * b;
    case 'Eq': return a === b ? -1n : 0n;
    case 'Ne': return a !== b ? -1n : 0n;
    case 'LtS': case 'LtU': return x < y ? -1n : 0n;
    case 'LeS': case 'LeU': return x <= y ? -1n : 0n;
    case 'GtS': case 'GtU': return x > y ? -1n : 0n;
    case 'GeS': case 'GeU': return x >= y ? -1n : 0n;
    case 'MinS': case 'MinU': return x < y ? x : y;
    case 'MaxS': case 'MaxU': return x > y ? x : y;
    default: throw new Error(op);
  }
}
for (const {name, bits, op} of operations) {
  const edges = [0n, 1n, (1n << BigInt(bits)) - 1n, 1n << BigInt(bits - 1),
                 (1n << BigInt(bits - 1)) - 1n, 0xaaaaaaaabbbbbbbbn];
  for (let index = 0; index < 64; ++index) {
    const lhs = new Uint8Array(16), rhs = new Uint8Array(16), expected = new Uint8Array(16);
    for (let lane = 0; lane < 128 / bits; ++lane) {
      const random = () => (BigInt(random32()) << 32n) | BigInt(random32());
      const a = BigInt.asUintN(bits, index < 6 ? edges[(index + lane) % 6] : random());
      const b = BigInt.asUintN(bits, index < 6 ? edges[(index + lane + 2) % 6] : random());
      put(lhs, lane * bits / 8, bits, a); put(rhs, lane * bits / 8, bits, b);
      put(expected, lane * bits / 8, bits, reference(op, bits, a, b));
    }
    for (const output of [131, 3, 67]) {
      memory.fill(0xcd, 0, 160);
      memory.set(lhs, 3); memory.set(rhs, 67);
      const before = memory.slice(0, 160);
      instance.exports[name](3, 67, output);
      ++invocations;
      for (let byte = 0; byte < 160; ++byte) {
        const value = byte >= output && byte < output + 16 ? expected[byte - output] : before[byte];
        assert.equal(memory[byte], value, `${name} case ${index} output ${output} byte ${byte}`);
        ++checks;
      }
    }
  }
  gc();
}
console.log('WASM_SIMD_MATRIX_OK ' + JSON.stringify({passed: true, tier, operations: operations.length,
  nativeFunctions, invocations, checks, seed: seed >>> 0}));
