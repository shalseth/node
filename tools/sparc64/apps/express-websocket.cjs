// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

const assert = require('node:assert/strict');
const { createHash } = require('node:crypto');
const { readFileSync } = require('node:fs');
const https = require('node:https');
const path = require('node:path');
const express = require('express');
const { WebSocket, WebSocketServer } = require('ws');

const certDirectory = process.env.SPARC64_SMOKE_CERT_DIR;
assert.ok(certDirectory, 'SPARC64_SMOKE_CERT_DIR must name the test certificates');
const cert = readFileSync(path.join(certDirectory, 'cert.pem'));
const key = readFileSync(path.join(certDirectory, 'key.pem'));
const deadline = setTimeout(() => {
  console.error('Application smoke test exceeded 20 seconds');
  process.exit(1);
}, 20_000);

async function main() {
  const app = express();
  app.use(express.json());
  app.get('/health', (req, res) => res.json({ ok: true, arch: process.arch }));
  app.post('/echo', (req, res) => res.json(req.body));
  const server = https.createServer({ cert, key }, app);
  const sockets = new WebSocketServer({ server, path: '/socket' });
  sockets.on('connection', (socket) => {
    socket.on('message', (data, binary) => socket.send(data, { binary }));
  });
  await new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(0, '127.0.0.1', resolve);
  });
  const port = server.address().port;
  try {
    async function request(url, body) {
      return new Promise((resolve, reject) => {
        const encoded = body === undefined ? undefined : JSON.stringify(body);
        const req = https.request({
          hostname: '127.0.0.1', port, path: url, ca: cert, agent: false,
          method: encoded ? 'POST' : 'GET',
          headers: encoded ? {
            'content-type': 'application/json',
            'content-length': Buffer.byteLength(encoded),
          } : {},
        }, (res) => {
          const chunks = [];
          res.on('data', (chunk) => chunks.push(chunk));
          res.on('error', reject);
          res.on('end', () => {
            try {
              assert.equal(res.statusCode, 200);
              resolve(JSON.parse(Buffer.concat(chunks).toString()));
            } catch (error) { reject(error); }
          });
        });
        req.on('error', reject);
        req.end(encoded);
      });
    }
    assert.deepEqual(await request('/health'), { ok: true, arch: 'sparc64' });
    const payload = { text: 'SPARC64: € 🚀', values: [1, 2, 3] };
    assert.deepEqual(await request('/echo', payload), payload);
    assert.equal(createHash('sha256').update('abc').digest('hex'),
                 'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad');
    await new Promise((resolve, reject) => {
      const client = new WebSocket(`wss://127.0.0.1:${port}/socket`, { ca: cert });
      let echoed = false;
      client.on('error', reject);
      client.on('open', () => client.send(payload.text));
      client.on('message', (data) => {
        try {
          assert.equal(data.toString(), payload.text);
          echoed = true;
          client.close(1000);
        } catch (error) { reject(error); }
      });
      client.on('close', (code) => {
        try {
          assert.ok(echoed, 'WebSocket must echo the payload before closing');
          assert.equal(code, 1000);
          resolve();
        } catch (error) { reject(error); }
      });
    });
    console.log('PASS Express HTTPS, JSON, SHA-256 and secure WebSocket echo');
  } finally {
    for (const client of sockets.clients) client.terminate();
    await new Promise((resolve) => sockets.close(resolve));
    await new Promise((resolve) => server.close(resolve));
  }
}

main().then(() => clearTimeout(deadline), (error) => {
  console.error(error);
  process.exit(1);
});
