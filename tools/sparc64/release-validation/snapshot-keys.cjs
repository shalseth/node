// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const { startupSnapshot } = require('node:v8');
const cases = [];
for (const length of [0, 32, 131072]) {
  for (const key of ['snapshot-key', Symbol('snapshot-key'), { secret: 17 }]) {
    for (const viewCount of [0, 1, 2]) {
      const buffer = new ArrayBuffer(length);
      const bytes = new Uint8Array(buffer);
      for (let i = 0; i < length; i++) bytes[i] = i & 255;
      const views = [];
      if (viewCount >= 1) views.push(new Uint8Array(buffer));
      if (viewCount >= 2) views.push(new DataView(buffer));
      %ArrayBufferSetDetachKey(buffer, key);
      cases.push({ buffer, key, length, views });
    }
  }
}
const pooled = Buffer.from('SPARC64 snapshot detach key');
%ArrayBufferSetDetachKey(pooled.buffer, Symbol('pooled-buffer-key'));
%CollectGarbage(null);
assert.ok(startupSnapshot.isBuildingSnapshot());
startupSnapshot.setDeserializeMainFunction(() => {
  %CollectGarbage(null);
  for (const { buffer, key, length, views } of cases) {
    assert.equal(buffer.byteLength, length);
    const bytes = new Uint8Array(buffer);
    for (let i = 0; i < length; i++) assert.equal(bytes[i], i & 255);
    assert.throws(() => %ArrayBufferDetach(buffer, {}), TypeError);
    assert.throws(() => %ArrayBufferDetach(buffer), TypeError);
    assert.equal(buffer.byteLength, length);
    %ArrayBufferDetach(buffer, key);
    assert.equal(buffer.byteLength, 0);
    assert.equal(bytes.length, 0);
    for (const view of views) {
      if (view instanceof DataView) {
        assert.throws(() => view.byteLength, TypeError);
      } else {
        assert.equal(view.length, 0);
      }
    }
  }
  assert.equal(pooled.toString(), 'SPARC64 snapshot detach key');
  assert.throws(() => %ArrayBufferDetach(pooled.buffer), TypeError);
  assert.equal(pooled.toString(), 'SPARC64 snapshot detach key');
  %CollectGarbage(null);
  console.log('SNAPSHOT_ARRAY_BUFFER_KEYS_OK', cases.length);
});
console.log('SNAPSHOT_ARRAY_BUFFER_KEYS_BUILT', cases.length);
