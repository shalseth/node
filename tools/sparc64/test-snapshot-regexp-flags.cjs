// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const { Worker, isMainThread, parentPort, workerData } = require('node:worker_threads');
function run() {
  const re = /^(π|a)+(?<tail>b*)$/du;
  for (let i = 0; i < 8; i++) {
    const match = re.exec('πaaπbbb');
    assert.equal(match[0], 'πaaπbbb');
    assert.equal(match.groups.tail, 'bbb');
    assert.deepEqual(match.indices[0], [0, 7]);
    gc();
  }
  return {native: %RegexpHasNativeCode(re, false) && !%RegexpHasBytecode(re, false),
    bytecode: %RegexpHasBytecode(re, false)};
}
(async () => {
  if (!isMainThread) {
    const flags = require('node:v8');
    flags.setFlagsFromString('--expose-gc');
    globalThis.gc = require('node:vm').runInNewContext('gc');
    parentPort.postMessage(run());
    return;
  }
  const expectedNative = !process.execArgv.includes('--regexp-interpret-all');
  const main = run();
  assert.equal(main.native, expectedNative);
  const worker = new Worker(__filename);
  const result = await new Promise((resolve, reject) => {
    worker.once('message', resolve);
    worker.once('error', reject);
    worker.once('exit', code => { if (code) reject(Error('worker exit ' + code)); });
  });
  assert.equal(result.native, expectedNative);
  console.log('SNAPSHOT_REGEXP_FLAGS_OK', JSON.stringify({main, worker: result}));
})().catch(error => { console.error(error); process.exitCode = 1; });
