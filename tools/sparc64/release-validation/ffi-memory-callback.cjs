'use strict';
const assert = require('node:assert/strict');
const ffi = require('node:ffi');
const {lib, functions: f} = ffi.dlopen(process.env.SPARC64_FFI_ORACLE, {
  allocate_bytes: {arguments: ['u64'], return: 'pointer'},
  release_bytes: {arguments: ['pointer'], return: 'void'},
  checksum_bytes: {arguments: ['pointer', 'u64'], return: 'u64'},
  callback_twice: {arguments: ['pointer', 'i32'], return: 'i32'},
});
const memory = f.allocate_bytes(128n);
assert.notEqual(memory, 0n);
const callback = lib.registerCallback({arguments: ['i32'], return: 'i32'}, n => n * 7 - 3);
let checks = 0;
try {
  for (let round = 0; round < 64; round++) {
    const bytes = Buffer.from(Array.from({length: 128}, (_, i) => (i * 17 + round * 13) & 255));
    ffi.exportBuffer(bytes, memory, bytes.length);
    assert.equal(f.checksum_bytes(memory, 128n), bytes.reduce((sum, value, i) => sum + BigInt(value * (i + 1)), 0n));
    assert.deepEqual(ffi.toBuffer(memory, 128), bytes);
    assert.equal(f.callback_twice(callback, round), round * 14 + 1);
    ffi.setInt32(memory, 1, -1234567 - round);
    ffi.setFloat64(memory, 9, round + 0.25);
    assert.equal(ffi.getInt32(memory, 1), -1234567 - round);
    assert.equal(ffi.getFloat64(memory, 9), round + 0.25);
    checks += 5;
    if (round % 16 === 0) gc();
  }
} finally {
  lib.unregisterCallback(callback);
  f.release_bytes(memory);
  lib.close();
}
console.log('FFI_MEMORY_CALLBACK_INSTALLED_OK ' + JSON.stringify({passed: true, checks}));
