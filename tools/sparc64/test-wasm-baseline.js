// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

d8.file.execute('test/mjsunit/wasm/wasm-module-builder.js');
const selected = arguments[0] || 'all';
let checks = 0;
let nativeFunctions = 0;
function eq(actual, expected) {
  if (!Object.is(actual, expected)) {
    throw new Error(`Expected ${String(expected)}, got ${String(actual)}`);
  }
  ++checks;
}
function native(fn) {
  eq(%IsLiftoffFunction(fn), true);
  ++nativeFunctions;
  return fn;
}
function run(name, body) {
  if (selected !== 'all' && selected !== name) return;
  print(`BEGIN ${name}`);
  body();
  print(`PASS ${name}`);
}
run('scalar', () => {
  for (const [signature, opcode, a, b, expected] of [
    [kSig_i_ii, kExprI32Add, 0x7fffffff, 1, -2147483648],
    [kSig_i_ii, kExprI32Mul, -123, 17, -2091],
    [kSig_l_ll, kExprI64Add, 0x123456789abcdefn, 17n, 0x123456789abce00n],
    [kSig_f_ff, kExprF32Add, 1.25, -0.5, 0.75],
    [kSig_d_dd, kExprF64Mul, -1.25, 4, -5],
  ]) {
    const builder = new WasmModuleBuilder();
    builder.addFunction('f', signature)
      .addBody([kExprLocalGet, 0, kExprLocalGet, 1, opcode]).exportFunc();
    const f = native(builder.instantiate().exports.f);
    for (let i = 0; i < 20; ++i) eq(f(a, b), expected);
  }
});
run('integer', () => {
  const operations = [
    ['add', kExprI32Add, (a, b) => (a + b) | 0],
    ['sub', kExprI32Sub, (a, b) => (a - b) | 0],
    ['mul', kExprI32Mul, Math.imul],
    ['and', kExprI32And, (a, b) => a & b],
    ['or', kExprI32Ior, (a, b) => a | b],
    ['xor', kExprI32Xor, (a, b) => a ^ b],
    ['shl', kExprI32Shl, (a, b) => a << b],
    ['shr', kExprI32ShrU, (a, b) => (a >>> b) | 0],
    ['sar', kExprI32ShrS, (a, b) => a >> b],
    ['divs', kExprI32DivS, (a, b) => (a / b) | 0],
    ['divu', kExprI32DivU, (a, b) => ((a >>> 0) / (b >>> 0)) | 0],
    ['rems', kExprI32RemS, (a, b) => (a % b) | 0],
    ['remu', kExprI32RemU, (a, b) => ((a >>> 0) % (b >>> 0)) | 0],
  ];
  const builder = new WasmModuleBuilder();
  for (const [name, opcode] of operations) {
    builder.addFunction(name, kSig_i_ii)
      .addBody([kExprLocalGet, 0, kExprLocalGet, 1, opcode]).exportFunc();
  }
  const exports = builder.instantiate().exports;
  const values = [-2147483648, -12345, -1, 0, 1, 17, 2147483647];
  for (const [name, , reference] of operations) {
    const f = native(exports[name]);
    for (const a of values) for (const b of values) {
      const traps = ((name.startsWith('div') || name.startsWith('rem')) && b === 0) ||
          (name === 'divs' && a === -2147483648 && b === -1);
      if (traps) {
        let trapped = false;
        try { f(a, b); } catch (error) { trapped = error instanceof WebAssembly.RuntimeError; }
        eq(trapped, true);
      } else eq(f(a, b), reference(a, b));
    }
  }
});
run('float', () => {
  const builder = new WasmModuleBuilder();
  for (const [bits, signature, min, max] of [
    [32, kSig_f_ff, kExprF32Min, kExprF32Max],
    [64, kSig_d_dd, kExprF64Min, kExprF64Max],
  ]) {
    for (const [name, opcode] of [['min', min], ['max', max]]) {
      builder.addFunction(`f${bits}_${name}`, signature)
        .addBody([kExprLocalGet, 0, kExprLocalGet, 1, opcode]).exportFunc();
    }
  }
  const exports = builder.instantiate().exports;
  const values = [-Infinity, -3.5, -0, 0, 0.25, Infinity, NaN];
  for (const bits of [32, 64]) for (const name of ['min', 'max']) {
    const f = native(exports[`f${bits}_${name}`]);
    for (const a of values) for (const b of values) eq(f(a, b), Math[name](a, b));
  }
});
run('control', () => {
  const builder = new WasmModuleBuilder();
  builder.addFunction('choose', kSig_i_iii).addBody([
    kExprLocalGet, 0, kExprIf, kWasmI32,
    kExprLocalGet, 1, kExprElse, kExprLocalGet, 2, kExprEnd,
  ]).exportFunc();
  const f = native(builder.instantiate().exports.choose);
  eq(f(1, -7, 9), -7);
  eq(f(0, -7, 9), 9);
});
run('calls', () => {
  const builder = new WasmModuleBuilder();
  const imported = builder.addImport('env', 'add', kSig_i_ii);
  const callee = builder.addFunction('callee', kSig_i_ii).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kExprCallFunction, imported,
  ]).exportFunc();
  builder.addFunction('caller', kSig_i_ii).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kExprCallFunction, callee.index,
  ]).exportFunc();
  let imports = 0;
  const instance = builder.instantiate({env: {add(a, b) {
    ++imports;
    return a + b;
  }}});
  const caller = native(instance.exports.caller);
  native(instance.exports.callee);
  for (let i = 0; i < 20; ++i) eq(caller(i, -3), i - 3);
  eq(imports, 20);
});
run('tables', () => {
  const builder = new WasmModuleBuilder();
  const type = builder.addType(kSig_i_ii);
  const callee = builder.addFunction('sum', type).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kExprI32Add,
  ]).exportFunc();
  builder.addTable(kWasmFuncRef, 2, 2);
  builder.addActiveElementSegment(0, wasmI32Const(0), [callee.index]);
  builder.addFunction('indirect', kSig_i_iii).addBody([
    kExprLocalGet, 0, kExprLocalGet, 1, kExprLocalGet, 2,
    kExprCallIndirect, type, 0,
  ]).exportFunc();
  const exports = builder.instantiate().exports;
  const indirect = native(exports.indirect);
  native(exports.sum);
  eq(indirect(17, -3, 0), 14);
  for (const index of [1, 2, -1]) {
    let trapped = false;
    try { indirect(1, 2, index); }
    catch (error) { trapped = error instanceof WebAssembly.RuntimeError; }
    eq(trapped, true);
  }
});
run('stack', () => {
  const builder = new WasmModuleBuilder();
  const count = 600;
  builder.addFunction('large', makeSig(Array(count).fill(kWasmI32), [kWasmI32]))
    .addBody([kExprLocalGet, ...wasmUnsignedLeb(count - 1)]).exportFunc();
  const recursive = builder.addFunction('recursive', kSig_i_i);
  recursive.addBody([
    kExprLocalGet, 0, kExprI32Eqz, kExprIf, kWasmI32,
    kExprI32Const, 0, kExprElse,
    kExprLocalGet, 0, kExprI32Const, 1, kExprI32Sub,
    kExprCallFunction, recursive.index, kExprI32Const, 1, kExprI32Add, kExprEnd,
  ]).exportFunc();
  const exports = builder.instantiate().exports;
  const large = native(exports.large);
  const recurse = native(exports.recursive);
  eq(large(...Array.from({length: count}, (_, i) => i - 700)), -101);
  eq(recurse(100), 100);
  let overflow = false;
  try { recurse(1000000); } catch (error) { overflow = error instanceof RangeError; }
  eq(overflow, true);
});
run('reentry', () => {
  const builder = new WasmModuleBuilder();
  const callback = builder.addImport('env', 'callback', kSig_i_i);
  builder.addFunction('entry', kSig_i_i).addBody([
    kExprLocalGet, 0, kExprCallFunction, callback,
  ]).exportFunc();
  let exports;
  let callbacks = 0;
  const instance = builder.instantiate({env: {callback(n) {
    ++callbacks;
    if (n === 16) {
      const objects = Array.from({length: 1000}, (_, i) => ({i}));
      gc();
      eq(objects[999].i, 999);
    }
    return n ? exports.entry(n - 1) + 1 : 0;
  }}});
  exports = instance.exports;
  const entry = native(exports.entry);
  eq(entry(32), 32);
  eq(callbacks, 33);
});
run('memory', () => {
  const builder = new WasmModuleBuilder();
  builder.addMemory(1, 2, false);
  builder.exportMemoryAs('memory');
  builder.addFunction('load', kSig_i_i)
    .addBody([kExprLocalGet, 0, kExprI32LoadMem, 0, 0]).exportFunc();
  builder.addFunction('store', kSig_v_ii)
    .addBody([kExprLocalGet, 0, kExprLocalGet, 1, kExprI32StoreMem, 0, 0])
    .exportFunc();
  builder.addFunction('grow', kSig_i_i)
    .addBody([kExprLocalGet, 0, kExprMemoryGrow, 0]).exportFunc();
  const {load, store, grow, memory} = builder.instantiate().exports;
  for (const fn of [load, store, grow]) native(fn);
  store(1, 0x12345678);
  const bytes = new Uint8Array(memory.buffer);
  for (const [i, value] of [0x78, 0x56, 0x34, 0x12].entries()) eq(bytes[1 + i], value);
  eq(load(1), 0x12345678);
  eq(grow(1), 1);
  eq(memory.buffer.byteLength, 131072);
  eq(load(1), 0x12345678);
  eq(grow(1), -1);
  for (const invoke of [() => load(131070), () => store(131070, 123)]) {
    let trapped = false;
    try { invoke(); } catch (error) { trapped = error instanceof WebAssembly.RuntimeError; }
    eq(trapped, true);
  }
});
if (!nativeFunctions) throw new Error(`No case selected: ${selected}`);
print(JSON.stringify({case: selected, checks, nativeFunctions, passed: true}));
