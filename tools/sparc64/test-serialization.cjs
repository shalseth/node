// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const { serialize, deserialize } = require('node:v8');
let checks = 0;
const values = [0n, 1n, -1n, 1337n, -1337n, 2n ** 63n, 2n ** 64n - 1n,
  2n ** 128n + 123n, -(2n ** 256n) + 7n, 2n ** 2048n - 1n];
for (const value of values) {
  if (value !== 0n) {
    let absolute = value < 0n ? -value : value;
    const bytes = [];
    do {
      for (let i = 0; i < 8; i++) {
        bytes.push(Number(absolute & 255n));
        absolute >>= 8n;
      }
    } while (absolute);
    assert.deepEqual([...serialize(value).subarray(-bytes.length)], bytes);
    checks++;
  }
  for (let padding = 0; padding < 16; padding++) {
    const object = { prefix: 'x'.repeat(padding), value,
      map: new Map([[value, { value }]]) };
    const encoded = serialize(object);
    for (let offset = 0; offset < 16; offset++) {
      const allocation = Buffer.alloc(encoded.length + 16);
      const view = allocation.subarray(offset, offset + encoded.length);
      encoded.copy(view);
      assert.deepEqual(deserialize(view), object);
      checks++;
    }
    assert.deepEqual(structuredClone(object), object);
    checks++;
  }
}
console.log(`PASS BigInt serialization: ${checks} endian/alignment/clone checks`);
