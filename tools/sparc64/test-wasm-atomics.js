// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

d8.file.execute('test/mjsunit/wasm/wasm-module-builder.js');
const atomicTier = arguments[0] || 'liftoff';
let checks = 0;
let nativeFunctions = 0;
function eq(actual, expected) {
  if (!Object.is(actual, expected)) {
    throw new Error(`Expected ${String(expected)}, got ${String(actual)}`);
  }
  ++checks;
}
const operations = ['Load', 'Store', 'Add', 'Sub', 'And', 'Or', 'Xor',
                    'Exchange', 'CompareExchange'];
const opcodes = [
  [kWasmI32, 1, [kExprI32AtomicLoad8U, kExprI32AtomicStore8U,
    kExprI32AtomicAdd8U, kExprI32AtomicSub8U, kExprI32AtomicAnd8U,
    kExprI32AtomicOr8U, kExprI32AtomicXor8U, kExprI32AtomicExchange8U,
    kExprI32AtomicCompareExchange8U]],
  [kWasmI32, 2, [kExprI32AtomicLoad16U, kExprI32AtomicStore16U,
    kExprI32AtomicAdd16U, kExprI32AtomicSub16U, kExprI32AtomicAnd16U,
    kExprI32AtomicOr16U, kExprI32AtomicXor16U, kExprI32AtomicExchange16U,
    kExprI32AtomicCompareExchange16U]],
  [kWasmI32, 4, [kExprI32AtomicLoad, kExprI32AtomicStore,
    kExprI32AtomicAdd, kExprI32AtomicSub, kExprI32AtomicAnd,
    kExprI32AtomicOr, kExprI32AtomicXor, kExprI32AtomicExchange,
    kExprI32AtomicCompareExchange]],
  [kWasmI64, 1, [kExprI64AtomicLoad8U, kExprI64AtomicStore8U,
    kExprI64AtomicAdd8U, kExprI64AtomicSub8U, kExprI64AtomicAnd8U,
    kExprI64AtomicOr8U, kExprI64AtomicXor8U, kExprI64AtomicExchange8U,
    kExprI64AtomicCompareExchange8U]],
  [kWasmI64, 2, [kExprI64AtomicLoad16U, kExprI64AtomicStore16U,
    kExprI64AtomicAdd16U, kExprI64AtomicSub16U, kExprI64AtomicAnd16U,
    kExprI64AtomicOr16U, kExprI64AtomicXor16U, kExprI64AtomicExchange16U,
    kExprI64AtomicCompareExchange16U]],
  [kWasmI64, 4, [kExprI64AtomicLoad32U, kExprI64AtomicStore32U,
    kExprI64AtomicAdd32U, kExprI64AtomicSub32U, kExprI64AtomicAnd32U,
    kExprI64AtomicOr32U, kExprI64AtomicXor32U, kExprI64AtomicExchange32U,
    kExprI64AtomicCompareExchange32U]],
  [kWasmI64, 8, [kExprI64AtomicLoad, kExprI64AtomicStore,
    kExprI64AtomicAdd, kExprI64AtomicSub, kExprI64AtomicAnd,
    kExprI64AtomicOr, kExprI64AtomicXor, kExprI64AtomicExchange,
    kExprI64AtomicCompareExchange]],
];
for (const memory64 of [false, true]) {
  for (const shared of [false, true]) {
    const memory = new WebAssembly.Memory({
      initial: memory64 ? 1n : 1, maximum: memory64 ? 2n : 2, shared,
      ...(memory64 ? {address: 'i64'} : {}),
    });
    const bytes = new Uint8Array(memory.buffer);
    for (const constant of [false, true]) {
      for (const [kind, width, codes] of opcodes) {
        const mask = (1n << BigInt(width * 8)) - 1n;
        const cast = n => kind === kWasmI64 ? BigInt.asIntN(64, n) : Number(BigInt.asIntN(32, n));
        const address = n => memory64 ? BigInt(n) : n;
        const read = offset => {
          let value = 0n;
          for (let i = 0; i < width; ++i) value |= BigInt(bytes[offset + i]) << BigInt(i * 8);
          return value;
        };
        const write = (offset, n) => {
          for (let i = 0; i < width; ++i) bytes[offset + i] = Number(n >> BigInt(i * 8) & 255n);
        };
        for (let op = 0; op < operations.length; ++op) {
          const name = operations[op];
          const builder = new WasmModuleBuilder();
          builder.addImportedMemory('m', 'memory', 1, 2, shared, memory64);
          const params = [memory64 ? kWasmI64 : kWasmI32];
          if (name !== 'Load') params.push(kind);
          if (name === 'CompareExchange') params.push(kind);
          const body = constant ? (memory64 ? wasmI64Const(32) : wasmI32Const(32)) : [kExprLocalGet, 0];
          for (let arg = 1; arg < params.length; ++arg) body.push(kExprLocalGet, arg);
          body.push(kAtomicPrefix, codes[op], Math.log2(width), width);
          builder.addFunction('f', makeSig(params, name === 'Store' ? [] : [kind]))
            .addBody(body).exportFunc();
          const f = builder.instantiate({m: {memory}}).exports.f;
          eq(atomicTier === 'optimized' ? %IsTurboFanFunction(f) :
                                       %IsLiftoffFunction(f), true);
          ++nativeFunctions;
          const seeds = [0n, 1n, 0xffn, 0x100n, 0xffffn, 0x10000n,
            0x7fffffff000000ffn, 0x8000000000000000n, 0x0123456789abcdefn, mask];
          const offsets = constant ? [32 + width] : [width, 8 + width, 32 + width, 65536 - width];
          for (const offset of offsets) {
            for (const seed of seeds) {
              bytes.fill(0xa5, Math.max(0, offset - 8), Math.min(bytes.length, offset + width + 8));
              write(offset, seed);
              const old = seed & mask;
              const value = (seed ^ 0xfedcba9876543210n) & ((1n << 64n) - 1n);
              const argument = address(constant ? 0 : offset - width);
              let expected = old;
              let returned;
              if (name === 'CompareExchange') {
                returned = f(argument, cast(seed), cast(value));
                expected = value;
              } else if (name === 'Load') {
                returned = f(argument);
              } else {
                returned = f(argument, cast(value));
                if (name === 'Store' || name === 'Exchange') expected = value;
                if (name === 'Add') expected = old + value;
                if (name === 'Sub') expected = old - value;
                if (name === 'And') expected = old & value;
                if (name === 'Or') expected = old | value;
                if (name === 'Xor') expected = old ^ value;
              }
              eq(returned, name === 'Store' ? undefined : cast(old));
              eq(read(offset), expected & mask);
              for (let i = Math.max(0, offset - 8); i < Math.min(bytes.length, offset + width + 8); ++i) {
                if (i < offset || i >= offset + width) eq(bytes[i], 0xa5);
              }
              if (name === 'CompareExchange') {
                write(offset, seed);
                eq(f(argument, cast((seed + 1n) & mask), cast(value)), cast(old));
                eq(read(offset), old);
              }
            }
          }
          if (!constant) {
            const args = [address(65536), cast(1n), cast(2n)].slice(0, params.length);
            let trapped = false;
            try { f(...args); } catch (e) { trapped = e instanceof WebAssembly.RuntimeError; }
            eq(trapped, true);
            if (memory64) {
              for (const index of [0x100000000n, -1n]) {
                args[0] = index;
                trapped = false;
                try { f(...args); } catch (e) { trapped = e instanceof WebAssembly.RuntimeError; }
                eq(trapped, true);
              }
            }
            if (width > 1) {
              args[0] = address(1);
              trapped = false;
              try { f(...args); } catch (e) { trapped = e instanceof WebAssembly.RuntimeError; }
              eq(trapped, true);
            }
          }
        }
      }
    }
    // Growth must update shared and private instance bounds alike.
    eq(memory.grow(memory64 ? 1n : 1), memory64 ? 1n : 1);
  }
}
print(JSON.stringify({passed: true, checks, nativeFunctions}));
