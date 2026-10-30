// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const { Worker, isMainThread, parentPort, workerData } = require('node:worker_threads');
const fs = require('node:fs');
const path = require('node:path');
const crypto = require('node:crypto');
const { setTimeout: delay } = require('node:timers/promises');
const root = path.resolve(__dirname, '../..');
const atomicTier = process.env.WASM_ATOMICS_TIER || 'liftoff';
const resultPath = process.env.WASM_ATOMICS_RESULT || 'out/sparc64-wasm-threads-result.json';
const directory = path.join(root, 'out/wasm-atomics-workload');
const sha256 = data => crypto.createHash('sha256').update(data).digest('hex');
function instantiate(module, memory, stack) {
  const exports = new WebAssembly.Instance(module, {env: {memory}}).exports;
  exports.__stack_pointer.value = stack;
  let nativeFunctions = 0;
  for (const value of Object.values(exports)) {
    if (typeof value === 'function') {
      assert.equal(atomicTier === 'optimized' ? %IsTurboFanFunction(value) :
                                               %IsLiftoffFunction(value), true);
      ++nativeFunctions;
    }
  }
  return {exports, nativeFunctions};
}
if (!isMainThread) {
  const {exports: wasm, nativeFunctions} = instantiate(
    workerData.module, workerData.memory, workerData.stack);
  parentPort.on('message', task => {
    try {
      if (task.type === 'queue') {
        parentPort.postMessage({type: 'ready'});
        const gate = wasm.wait32(task.gate, 0, 5_000_000_000n);
        assert.ok(gate === 0 || gate === 1);
        const worked = wasm.hash_queue(task.next, task.completed, task.data,
          task.hashes, task.count, task.blockSize, BigInt(task.seed));
        parentPort.postMessage({type: 'done', worked, nativeFunctions});
      } else if (task.type === 'lanes') {
        wasm.add_lanes(task.address, workerData.index, task.iterations);
        parentPort.postMessage({type: 'done'});
      } else if (task.type === 'tearing') {
        const errors = wasm.tear64(task.address, workerData.index, task.iterations);
        parentPort.postMessage({type: 'done', errors});
      } else if (task.type === 'ordering') {
        wasm.store_buffering(task.address, workerData.index, task.rounds);
        parentPort.postMessage({type: 'done'});
      } else if (task.type === 'growth') {
        parentPort.postMessage({type: 'ready'});
        const pages = wasm.poll_growth(task.address, task.previous);
        parentPort.postMessage({type: 'done', pages});
      } else if (task.type === 'wait') {
        parentPort.postMessage({type: 'ready'});
        const result = task.width === 8 ?
          wasm.wait64(task.address, BigInt(task.expected), 5_000_000_000n) :
          wasm.wait32(task.address, task.expected, 5_000_000_000n);
        parentPort.postMessage({type: 'done', result});
      } else if (task.type === 'jswait') {
        const array = new Int32Array(workerData.memory.buffer);
        parentPort.postMessage({type: 'ready'});
        const result = Atomics.wait(array, task.address / 4,
          Atomics.load(array, task.address / 4), 5000);
        parentPort.postMessage({type: 'done', result});
      } else {
        throw new Error('Unknown worker task');
      }
    } catch (error) {
      parentPort.postMessage({type: 'error', error: error.stack});
    }
  });
  parentPort.postMessage({type: 'initialized', nativeFunctions});
} else {
  (async () => {
    const manifest = JSON.parse(fs.readFileSync(path.join(directory, 'manifest.json')));
    for (const [name, hash] of Object.entries(manifest.files)) {
      if (name !== 'native-hash.so') assert.equal(sha256(fs.readFileSync(path.join(directory, name))), hash);
    }
    const module = new WebAssembly.Module(fs.readFileSync(path.join(directory, 'threaded-hash.wasm')));
    const memory = new WebAssembly.Memory({initial: 320, maximum: 1024, shared: true});
    const {exports: wasm, nativeFunctions} = instantiate(module, memory, 65536);
    const workers = [];
    const queues = [];
    const pending = [];
    function receive(index, expected) {
      return new Promise((resolve, reject) => {
        const accept = message => {
          if (message.type === 'error') reject(new Error(message.error));
          else if (message.type !== expected) reject(new Error(`Expected ${expected}, received ${message.type}`));
          else resolve(message);
        };
        if (queues[index].length) accept(queues[index].shift());
        else pending[index] = accept;
      });
    }
    try {
      for (let i = 0; i < 4; ++i) {
        queues[i] = [];
        const worker = new Worker(__filename, {workerData: {
          module, memory, stack: (i + 2) * 65536, index: i,
        }});
        worker.on('message', message => {
          if (pending[i]) {
            const callback = pending[i];
            pending[i] = null;
            callback(message);
          } else queues[i].push(message);
        });
        worker.on('error', error => {
          if (pending[i]) pending[i]({type: 'error', error: error.stack});
          else queues[i].push({type: 'error', error: error.stack});
        });
        workers.push(worker);
      }
      const initialized = await Promise.all(workers.map((_, i) => receive(i, 'initialized')));
      const input = fs.readFileSync(path.join(directory, 'input.bin'));
      const expected = fs.readFileSync(path.join(directory, 'expected.bin'));
      const data = 2 * 1024 * 1024;
      const hashes = data + input.length;
      new Uint8Array(memory.buffer, data, input.length).set(input);
      const view = new DataView(memory.buffer);
      const seed = BigInt(manifest.seed);
      let serial = 0;
      for (let i = 0; i < manifest.count; ++i) {
        assert.equal(BigInt.asUintN(64, wasm.hash_block(data + i * manifest.block_size,
          manifest.block_size, seed + BigInt(i))), expected.readBigUInt64LE(i * 8));
        ++serial;
      }
      const gate = 1048576;
      const next = gate + 8;
      const completed = gate + 16;
      wasm.store32(gate, 0);
      wasm.store32(next, 0);
      wasm.store32(completed, 0);
      const started = performance.now();
      for (const worker of workers) worker.postMessage({type: 'queue', gate, next,
        completed, data, hashes, count: manifest.count,
        blockSize: manifest.block_size, seed: manifest.seed});
      await Promise.all(workers.map((_, i) => receive(i, 'ready')));
      wasm.store32(gate, 1);
      wasm.notify(gate, 4);
      const results = await Promise.all(workers.map((_, i) => receive(i, 'done')));
      assert.equal(results.reduce((sum, result) => sum + result.worked, 0), manifest.count);
      assert.ok(results.every(result => result.worked > 0));
      assert.equal(wasm.load32(completed), manifest.count);
      assert.equal(wasm.load32(next), manifest.count + workers.length);
      const actual = Buffer.from(memory.buffer, hashes, expected.length);
      assert.deepEqual(actual, expected);
      const seconds = (performance.now() - started) / 1000;
      const laneAddress = gate + 128;
      new Uint8Array(memory.buffer, laneAddress, 64).fill(0);
      const laneIterations = 10003;
      for (const worker of workers) worker.postMessage({type: 'lanes',
        address: laneAddress, iterations: laneIterations});
      await Promise.all(workers.map((_, i) => receive(i, 'done')));
      for (let i = 0; i < workers.length; ++i) {
        assert.equal(view.getUint8(laneAddress + i), laneIterations & 255);
        assert.equal(view.getUint16(laneAddress + 8 + i * 2, true), laneIterations);
        assert.equal(view.getUint32(laneAddress + 16 + i * 4, true), laneIterations);
        assert.equal(view.getBigUint64(laneAddress + 32 + i * 8, true), BigInt(laneIterations));
      }
      const tearingIterations = 10003;
      for (const worker of workers) worker.postMessage({type: 'tearing',
        address: gate + 384, iterations: tearingIterations});
      const tearing = await Promise.all(workers.map((_, i) => receive(i, 'done')));
      assert.ok(tearing.every(result => result.errors === 0));
      const orderingAddress = gate + 256;
      const orderingRounds = 10000;
      new Uint8Array(memory.buffer, orderingAddress, 32).fill(0);
      for (let i = 0; i < 2; ++i) workers[i].postMessage({type: 'ordering',
        address: orderingAddress, rounds: orderingRounds});
      await Promise.all([receive(0, 'done'), receive(1, 'done')]);
      assert.equal(view.getUint32(orderingAddress + 24, true), 0);
      const growthAddress = gate + 448;
      wasm.store32(growthAddress, 0);
      const previousPages = memory.buffer.byteLength / 65536;
      workers[0].postMessage({type: 'growth', address: growthAddress, previous: previousPages});
      await receive(0, 'ready');
      for (let retry = 0; retry < 100 && !wasm.load32(growthAddress); ++retry) await delay(2);
      assert.equal(wasm.load32(growthAddress), 1);
      assert.equal(memory.grow(1), previousPages);
      const growth = await receive(0, 'done');
      assert.equal(growth.pages, previousPages + 1);
      let waitChecks = 0;
      const waitEvidence = [];
      for (const width of [4, 8]) {
        const address = gate + 32;
        const value = width === 4 ? 0x1234abcd : 0x123456789abcdefn;
        if (width === 4) view.setUint32(address, value, true);
        else view.setBigUint64(address, value, true);
        const wait = width === 4 ? wasm.wait32 : wasm.wait64;
        assert.equal(wait(address, value, 0n), 2);
        assert.equal(wait(address, width === 4 ? value + 1 : value + 1n, 0n), 1);
        waitChecks += 2;
        for (const notifier of ['wasm', 'js']) {
          workers[0].postMessage({type: 'wait', width, address, expected: String(value)});
          await receive(0, 'ready');
          let woke = 0;
          for (let retry = 0; retry < 100 && !woke; ++retry) {
            await delay(2);
            woke = notifier === 'wasm' ? wasm.notify(address, 1) :
              Atomics.notify(new Int32Array(memory.buffer), address / 4, 1);
          }
          assert.equal(woke, 1);
          const result = await receive(0, 'done');
          assert.equal(result.result, 0);
          ++waitChecks;
          waitEvidence.push({width, notifier, result: result.result});
        }
      }
      workers[0].postMessage({type: 'jswait', address: gate + 32});
      await receive(0, 'ready');
      let woke = 0;
      for (let retry = 0; retry < 100 && !woke; ++retry) {
        await delay(2);
        woke = wasm.notify(gate + 32, 1);
      }
      assert.equal(woke, 1);
      assert.equal((await receive(0, 'done')).result, 'ok');
      ++waitChecks;
      const result = {passed: true, workload: 'bundled xxHash64 atomic queue',
        serial, threaded: manifest.count, workers: results.map(r => r.worked),
        bytes: input.length, seconds, laneIterations, tearingIterations, orderingRounds,
        waitChecks, waitEvidence, growthPages: growth.pages,
        nativeFunctions: nativeFunctions + initialized.reduce((n, r) => n + r.nativeFunctions, 0),
        module_sha256: manifest.files['threaded-hash.wasm'],
        expected_sha256: manifest.files['expected.bin']};
      fs.writeFileSync(path.join(root, resultPath), JSON.stringify(result, null, 2) + '\n');
      console.log(JSON.stringify(result));
      console.log('WASM_THREADS_OK');
    } finally {
      await Promise.all(workers.map(worker => worker.terminate()));
    }
  })().catch(error => { console.error(error); process.exitCode = 1; });
}
