// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = `globalThis.cachedWork = function cachedWork(value) {
  globalThis.cachedStatus = %GetOptimizationStatus(cachedWork);
  return value + 17;
};`;
const filename = process.argv[3];
function check() {
  const fn = globalThis.cachedWork;
  %PrepareFunctionForOptimization(fn);
  assert.equal(fn(2), 19);
  assert.equal(fn(3), 20);
  %OptimizeFunctionOnNextCall(fn);
  assert.equal(fn(4), 21);
  assert.ok(globalThis.cachedStatus & (1 << 11));
  assert.equal(fn(2.5), 19.5);
  gc();
  assert.equal(fn(10), 27);
}
if (process.argv[2] === 'produce') {
  const script = new vm.Script(source);
  script.runInThisContext();
  check();
  const data = script.createCachedData();
  assert.ok(data.length > 32);
  fs.writeFileSync(filename, data);
  console.log('CODE_CACHE_PRODUCED', data.length);
} else {
  const data = fs.readFileSync(filename);
  const mode = process.argv[2];
  assert.ok(['consume', 'corrupt', 'source', 'version', 'flags'].includes(mode));
  if (mode === 'corrupt') data[0] ^= 255;
  // SerializedCodeData's version and flag hashes follow the magic/source words.
  if (mode === 'version') data[4] ^= 255;
  if (mode === 'flags') data[12] ^= 255;
  const script = new vm.Script(mode === 'source' ?
    'globalThis.otherSource = 42;' : source, { cachedData: data });
  assert.equal(script.cachedDataRejected, mode !== 'consume');
  script.runInThisContext();
  if (mode === 'source') assert.equal(globalThis.otherSource, 42);
  else check();
  console.log('CODE_CACHE_STATE_OK', mode);
}
