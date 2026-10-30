'use strict';
const assert = require('node:assert/strict');
const addon = require('./build/Release/oracle.node');
const values = new Uint32Array(1027);
for (let i = 0; i < values.length; i++) values[i] = Math.imul(i, 0x9e3779b9) >>> 0;
for (let round = 0; round < 64; round++) {
  const input = values.subarray(round, values.length - round);
  const expected = input.reduce((sum, value, i) => sum + BigInt(value) * BigInt(i + 1), 0n);
  assert.equal(addon.checksum(input), BigInt.asUintN(64, expected));
  assert.equal(addon.callback(x => x * 7n + 3n, BigInt(round)), BigInt(round) * 7n + 3n);
  if (round % 16 === 0) gc();
}
assert.throws(() => addon.checksum(new Uint8Array(1)), TypeError);
console.log('INSTALLED_NATIVE_ADDON_OK ' + JSON.stringify({passed: true, rounds: 64}));
