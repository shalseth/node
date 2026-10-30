// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

const assert = require('node:assert/strict');
const { readFileSync, writeFileSync } = require('node:fs');
const https = require('node:https');
const path = require('node:path');
const { performance } = require('node:perf_hooks');
const { Worker, isMainThread, parentPort } = require('node:worker_threads');
const express = require('express');
const { WebSocket, WebSocketServer } = require('ws');

function reference(value) {
  return Number(BigInt.asIntN(32, BigInt(value) * 1664525n + 1013904223n));
}
let optimized = 0;
function calculate(value) {
  const result = (Math.imul(value, 1664525) + 1013904223) | 0;
  if (%GetOptimizationStatus(calculate) & (1 << 11)) optimized++;
  return result;
}

if (!isMainThread) {
  let next = 0;
  let stopping = false;
  parentPort.once('message', () => { stopping = true; });
  function batch() {
    for (let n = 0; n < 1000; n++, next++) {
      assert.equal(calculate(next), reference(next));
    }
    if (stopping) {
      parentPort.postMessage({ calls: next, optimized });
      parentPort.close();
    } else {
      setTimeout(batch, 5);
    }
  }
  batch();
} else {
  main().catch(error => { console.error(error); process.exit(1); });
}

async function main() {
  assert.equal(typeof gc, 'function');
  const directory = process.env.SPARC64_SMOKE_CERT_DIR;
  const cert = readFileSync(path.join(directory, 'cert.pem'));
  const key = readFileSync(path.join(directory, 'key.pem'));
  const deadline = setTimeout(() => {
    console.error('TurboFan application soak exceeded 240 seconds');
    process.exit(1);
  }, 240_000);
  const workers = Array.from({ length: 2 }, () => new Worker(__filename));
  const workerResults = workers.map(worker => new Promise((resolve, reject) => {
    let result;
    worker.on('message', message => { result = message; });
    worker.on('error', reject);
    worker.on('exit', code => {
      if (code === 0 && result) resolve(result);
      else reject(new Error(`Worker exit ${code}, result ${JSON.stringify(result)}`));
    });
  }));
  let handlerOptimized = 0;
  let websocketMessages = 0;
  function handler(req, res) {
    if (%GetOptimizationStatus(handler) & (1 << 11)) handlerOptimized++;
    const value = req.body.value;
    const objects = Array.from({ length: 32 }, (_, i) => ({ value: value + i }));
    res.json({ value, result: calculate(value), tail: objects[31].value,
               text: req.body.text });
  }
  const app = express();
  app.use(express.json());
  app.post('/calculate', handler);
  const server = https.createServer({ key, cert }, app);
  const sockets = new WebSocketServer({ server });
  sockets.on('connection', socket => socket.on('message', (data, binary) => {
    websocketMessages++;
    socket.send(data, { binary });
  }));
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const port = server.address().port;
  const agent = new https.Agent({ keepAlive: true, maxSockets: 16, ca: cert });
  function request(value) {
    const body = JSON.stringify({ value, text: 'SPARC64: € 🚀' });
    return new Promise((resolve, reject) => {
      const req = https.request({ hostname: '127.0.0.1', port, agent,
        path: '/calculate', method: 'POST', headers: {
          'content-type': 'application/json', 'content-length': Buffer.byteLength(body),
        } }, res => {
        let output = '';
        res.setEncoding('utf8');
        res.on('data', chunk => { output += chunk; });
        res.on('error', reject);
        res.on('end', () => {
          try {
            assert.equal(res.statusCode, 200);
            assert.deepEqual(JSON.parse(output), { value, result: reference(value),
              tail: value + 31, text: 'SPARC64: € 🚀' });
            resolve();
          } catch (error) { reject(error); }
        });
      });
      req.on('error', reject);
      req.end(body);
    });
  }
  const client = new WebSocket(`wss://127.0.0.1:${port}`, { ca: cert });
  await new Promise((resolve, reject) => {
    client.once('open', resolve);
    client.once('error', reject);
  });
  async function echo(value) {
    const binary = value % 2 === 0;
    const data = binary ? Buffer.alloc(8192, value & 255) : `SPARC64: € 🚀 ${value}`;
    const received = new Promise((resolve, reject) => {
      client.once('message', (message, isBinary) => {
        try {
          assert.equal(isBinary, binary);
          if (binary) assert.deepEqual(message, data);
          else assert.equal(message.toString(), data);
          resolve();
        } catch (error) { reject(error); }
      });
      client.once('error', reject);
    });
    client.send(data, { binary });
    await received;
    client.removeAllListeners('error');
  }
  const samples = [];
  const started = performance.now();
  let requests = 0;
  let batches = 0;
  for (let epoch = 0; epoch < 8; epoch++) {
    const until = started + (epoch + 1) * 15_000;
    do {
      const first = requests;
      await Promise.all(Array.from({ length: 16 }, (_, i) => request(first + i)));
      requests += 16;
      await echo(batches++);
      if (batches % 8 === 0) gc({ type: 'minor', execution: 'sync' });
    } while (performance.now() < until || requests < (epoch + 1) * 750);
    gc();
    await new Promise(resolve => setImmediate(resolve));
    const sample = { epoch, requests, milliseconds: performance.now() - started,
                     ...process.memoryUsage(), handlerOptimized, optimized };
    samples.push(sample);
    console.log(JSON.stringify(sample));
  }
  client.close(1000);
  await new Promise(resolve => client.once('close', resolve));
  agent.destroy();
  await new Promise(resolve => sockets.close(resolve));
  await new Promise(resolve => server.close(resolve));
  workers.forEach(worker => worker.postMessage('stop'));
  const background = await Promise.all(workerResults);
  assert.ok(handlerOptimized > 0, 'Real Express handler must execute TurboFan');
  assert.ok(optimized > 0, 'Request arithmetic must execute TurboFan');
  for (const worker of background) assert.ok(worker.optimized > 0);
  assert.equal(websocketMessages, batches);
  const settled = samples.slice(2);
  assert.ok(settled.at(-1).heapUsed - settled[0].heapUsed < 12 * 1024 * 1024,
            'Retained JS heap grew by at least 12 MiB after warmup');
  assert.ok(settled.at(-1).rss - settled[0].rss < 96 * 1024 * 1024,
            'RSS grew by at least 96 MiB after warmup');
  const result = { passed: true, requests, websocketMessages,
    milliseconds: performance.now() - started, handlerOptimized, optimized,
    workers: background, samples };
  writeFileSync('turbofan-soak-result.json', JSON.stringify(result, null, 2) + '\n');
  clearTimeout(deadline);
  console.log('TURBOFAN_APPLICATION_SOAK_OK', JSON.stringify({
    requests, websocketMessages, handlerOptimized, optimized, workers: background,
  }));
}
