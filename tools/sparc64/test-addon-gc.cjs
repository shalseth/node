// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

const assert = require('node:assert/strict');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
assert.equal(process.arch, 'sparc64');
assert.equal(typeof global.gc, 'function');
const addon = require(path.resolve(process.argv[2]));
const started = performance.now();
let callbacks = 0;
let majorCollections = 0;
let minorCollections = 0;

function collect(type) {
  gc({ type, execution: 'sync' });
  if (type === 'major') majorCollections++;
  else minorCollections++;
}

function invokeNative(round, payload) {
  return addon.roundTrip(function fromNative(box) {
    callbacks++;
    assert.strictEqual(box.payload, payload);
    const stack = new Error('native reentry stack').stack;
    assert.match(stack, /fromNative/);
    assert.match(stack, /invokeNative/);
    collect('major');
    box.afterGc = { round, text: 'SPARC64: € 🚀', values: [1, 2, 3] };
    collect('minor');
    assert.equal(box.afterGc.round, round);
    if (round % 2 === 0) {
      const nested = addon.roundTrip(function nestedFromNative(inner) {
        callbacks++;
        assert.strictEqual(inner.payload, payload);
        collect('major');
        const nestedStack = new Error('nested native reentry').stack;
        assert.match(nestedStack, /nestedFromNative/);
        assert.match(nestedStack, /fromNative/);
        return payload;
      }, payload);
      assert.strictEqual(nested.result, payload);
      assert.strictEqual(nested.box.payload, payload);
    }
    return { payload, round };
  }, payload);
}

for (let round = 0; round < 6; ++round) {
  const payload = { round, values: Array.from({ length: 64 }, (_, n) => ({ n })) };
  const result = invokeNative(round, payload);
  collect('major');
  assert.strictEqual(result.box.payload, payload);
  assert.strictEqual(result.result.payload, payload);
  assert.equal(result.box.afterGc.text, 'SPARC64: € 🚀');
  assert.deepEqual(result.box.afterGc.values, [1, 2, 3]);
}

let thrown;
assert.throws(() => addon.roundTrip(function throwingFromNative() {
  callbacks++;
  collect('major');
  thrown = new Error('native callback exception');
  throw thrown;
}, { exception: true }), (error) => {
  assert.strictEqual(error, thrown);
  assert.match(error.stack, /throwingFromNative/);
  return true;
});
assert.equal(callbacks, 10);
assert.equal(majorCollections, 16);
assert.equal(minorCollections, 6);
console.log(`PASS native addon: callbacks=${callbacks}, major_gc=${majorCollections}, ` +
            `minor_gc=${minorCollections}, elapsed_ms=${Math.round(performance.now() - started)}`);
