// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

// Run with --allow-natives-syntax --maglev --no-turbofan --expose-gc.

function equal(expected, actual) {
  if (!Object.is(expected, actual)) {
    throw new Error(`expected ${expected}, got ${actual}`);
  }
}

function optimize(fn, args) {
  console.log('MAGLEV', fn.name);
  %PrepareFunctionForOptimization(fn);
  for (let i = 0; i < 5; ++i) fn(...args);
  %OptimizeMaglevOnNextCall(fn);
  const result = fn(...args);
  if ((%GetOptimizationStatus(fn) & (1 << 4)) === 0) {
    throw new Error(`${fn.name} did not enter Maglev`);
  }
  return result;
}

function add(a, b) { return a + b; }
equal(42, optimize(add, [19, 23]));
equal(2147483648, add(2147483647, 1));

function multiply(a, b) { return a * b; }
equal(42, optimize(multiply, [6, 7]));
equal(-0, multiply(0, -1));
equal(4294967296, multiply(65536, 65536));

function divide(a, b) { return a / b; }
equal(6, optimize(divide, [42, 7]));
equal(3.5, divide(7, 2));
equal(-0, divide(0, -2));
equal(Infinity, divide(1, 0));

function modulo(a, b) { return a % b; }
equal(2, optimize(modulo, [17, 5]));
equal(-2, modulo(-17, 5));
equal(-0, modulo(-10, 5));

function bits(x) { return (x | 0) ^ 0x12345678; }
equal(0x1234567b, optimize(bits, [3.75]));
equal(0x1234567b, bits(4294967299.75));
equal(0x12345678, bits(NaN));

function floatMath(a, b) { return Math.sin(a) + Math.pow(a, b); }
equal(Math.sin(1.25) + Math.pow(1.25, 2.5),
      optimize(floatMath, [1.25, 2.5]));

function compare(a, b) { return (a < b ? 1 : 0) + (a >= b ? 2 : 0); }
equal(1, optimize(compare, [1.25, 2.5]));
equal(0, compare(NaN, 1));
equal(0, compare(1, NaN));

const bytes = new Uint8ClampedArray(8);
function clamp(a, x) { a[1] = x; return a[1]; }
equal(2, optimize(clamp, [bytes, 2.5]));
for (const [input, output] of [[3.5, 4], [1.5, 2], [0.5, 0],
                              [-1, 0], [NaN, 0], [Infinity, 255]]) {
  equal(output, clamp(bytes, input));
}

const view = new DataView(new ArrayBuffer(32));
function unaligned(v, x, little) {
  v.setFloat64(1, x, little);
  return v.getFloat64(1, little);
}
equal(1.25, optimize(unaligned, [view, 1.25, true]));
equal(-2.5, unaligned(view, -2.5, false));
function integerView(v, x, little) {
  v.setInt32(3, x, little);
  return v.getInt32(3, little);
}
equal(-1234567, optimize(integerView, [view, -1234567, true]));
equal(-7654321, integerView(view, -7654321, false));

function objectAccess(o) { return o.x + o.y; }
equal(42, optimize(objectAccess, [{x: 19, y: 23}]));
equal('ab', objectAccess({x: 'a', y: 'b', z: 1}));

function allocate(n) {
  let p = {value: 0};
  for (let i = 0; i < n; ++i) p = {value: p.value + 1, previous: p};
  return p.value;
}
equal(20, optimize(allocate, [20]));
gc();
equal(2000, allocate(2000));

function collect(n) {
  const holder = {child: {value: 0}};
  gc();
  for (let i = 0; i < n; ++i) {
    holder.child = {value: holder.child.value + 1};
    gc({type: 'minor'});
  }
  return holder.child.value;
}
equal(20, optimize(collect, [20]));
equal(100, collect(100));

console.log('SPARC64 Maglev smoke passed');
