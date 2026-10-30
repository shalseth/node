// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

function equal(actual, expected) {
  if (!Object.is(actual, expected)) {
    throw new Error(String(actual) + ' != ' + String(expected));
  }
}
let checked = 0;
function exercise(fn, vectors) {
  %PrepareFunctionForOptimization(fn);
  for (let n = 0; n < 2; n++) for (const [args] of vectors) fn(...args);
  %OptimizeFunctionOnNextCall(fn);
  fn(...vectors[0][0]);
  if (!(%GetOptimizationStatus(fn) & (1 << 5))) {
    throw new Error(fn.name + ' did not reach TurboFan');
  }
  for (const [args, expected] of vectors) {
    const actual = fn(...args);
    if (Array.isArray(expected)) {
      equal(actual.length, expected.length);
      for (let n = 0; n < expected.length; n++) equal(actual[n], expected[n]);
    } else {
      equal(actual, expected);
    }
    checked++;
  }
}
function integer(a, b) {
  return [(a / b) | 0, (a % b) | 0, Math.imul(a, b), (a + b) | 0];
}
const integers = [];
for (const a of [-2147483648, -65537, -1, 0, 1, 65537, 2147483647]) {
  for (const b of [-2147483648, -257, -1, 1, 257, 2147483647]) {
    const x = BigInt(a), y = BigInt(b);
    const i32 = value => Number(BigInt.asIntN(32, value));
    integers.push([[a, b], [i32(x / y), i32(x % y), i32(x * y), i32(x + y)]]);
  }
}
exercise(integer, integers);
function rotate8(value) { return (value >>> 8) | (value << 24); }
exercise(rotate8, [
  [[0x12345678], 0x78123456], [[1], 0x01000000],
  [[0x89abcdef], 0xef89abcd | 0], [[0x80000000], 0x00800000],
]);
function divide(a, b) { return a / b; }
exercise(divide, [
  [[1, 2], 0.5], [[1, -0], -Infinity], [[-1, Infinity], -0],
  [[Number.MIN_VALUE, 2], 0], [[-Number.MIN_VALUE, 2], -0],
  [[Number.MAX_VALUE, 0.5], Infinity], [[Infinity, Infinity], NaN],
  [[NaN, 1], NaN],
]);
function minmax(a, b) { return [Math.min(a, b), Math.max(a, b)]; }
exercise(minmax, [
  [[1, 2], [1, 2]], [[-0, 0], [-0, 0]], [[0, -0], [-0, 0]],
  [[Infinity, -Infinity], [-Infinity, Infinity]], [[NaN, 1], [NaN, NaN]],
]);
function f32(value) { return [Math.fround(value) | 0, Math.fround(value) >>> 0]; }
exercise(f32, [
  [[1.5], [1, 1]], [[-1.5], [-1, 4294967295]],
  [[2147483647], [-2147483648, 2147483648]], [[4294967295], [0, 0]],
  [[NaN], [0, 0]], [[Infinity], [0, 0]],
]);
const bytes = new Uint8Array([0, 0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef]);
const view = new DataView(bytes.buffer);
function read(view, little) {
  return [view.getUint16(1, little), view.getUint32(1, little),
          view.getBigUint64(1, little), view.getBigInt64(1, little)];
}
exercise(read, [
  [[view, false], [0x0123, 0x01234567, 0x0123456789abcdefn, 0x0123456789abcdefn]],
  [[view, true], [0x2301, 0x67452301, 0xefcdab8967452301n,
                  BigInt.asIntN(64, 0xefcdab8967452301n)]],
]);
function store(view, little) {
  view.setUint32(1, 0x89abcdef, little);
  view.setBigUint64(5, 0x0123456789abcdefn, little);
  return Array.from(new Uint8Array(view.buffer));
}
const output = new DataView(new ArrayBuffer(14));
exercise(store, [
  [[output, false], [0, 0x89, 0xab, 0xcd, 0xef, 1, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef, 0]],
  [[output, true], [0, 0xef, 0xcd, 0xab, 0x89, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 1, 0]],
]);
function floatingStore(view, little) {
  view.setFloat32(1, 1.0, little);
  view.setFloat64(5, 1.0, little);
  return Array.from(new Uint8Array(view.buffer));
}
exercise(floatingStore, [
  [[output, false], [0, 0x3f, 0x80, 0, 0, 0x3f, 0xf0, 0, 0, 0, 0, 0, 0, 0]],
  [[output, true], [0, 0, 0, 0x80, 0x3f, 0, 0, 0, 0, 0, 0, 0xf0, 0x3f, 0]],
]);
print('PASS optimized backend matrix:', checked, 'edge-case vectors');
