// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
'use strict';
const assert = require('node:assert/strict');
const crypto = require('node:crypto');
const { once } = require('node:events');
const http = require('node:http');
const zlib = require('node:zlib');
let checks = 0;
let echoes = 0;
const sockets = new Set();
const server = http.createServer((req, res) => {
  if (req.url === '/redirect') {
    res.writeHead(302, {location: '/json'}).end();
  } else if (req.url === '/gzip') {
    res.writeHead(200, {'content-encoding': 'gzip', 'content-type': 'application/json'});
    res.end(zlib.gzipSync(JSON.stringify({answer: 42})));
  } else if (req.url === '/chunks') {
    res.write('one');
    setImmediate(() => { res.write('two'); setImmediate(() => res.end('three')); });
  } else if (req.url === '/echo') {
    req.pipe(res);
  } else if (req.url === '/abort') {
    const timer = setTimeout(() => res.end('late'), 1000);
    res.once('close', () => clearTimeout(timer));
  } else {
    res.writeHead(200, {'content-type': 'application/json'});
    res.end(JSON.stringify({answer: 42}));
  }
});
server.on('connection', socket => {
  sockets.add(socket);
  socket.once('close', () => sockets.delete(socket));
});
server.on('upgrade', (req, socket, head) => {
  const accept = crypto.createHash('sha1')
    .update(req.headers['sec-websocket-key'] + '258EAFA5-E914-47DA-95CA-C5AB0DC85B11')
    .digest('base64');
  socket.write('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n' +
    `Connection: Upgrade\r\nSec-WebSocket-Accept: ${accept}\r\n\r\n`);
  let pending = head;
  const consume = () => {
    while (pending.length >= 2) {
      const opcode = pending[0] & 15;
      assert(pending[0] & 128);
      assert(pending[1] & 128);
      let size = pending[1] & 127;
      let header = 2;
      if (size === 126) {
        if (pending.length < 4) return;
        size = pending.readUInt16BE(2); header = 4;
      } else assert(size < 126);
      if (pending.length < header + 4 + size) return;
      const key = pending.subarray(header, header + 4);
      const payload = Buffer.from(pending.subarray(header + 4, header + 4 + size));
      for (let i = 0; i < size; ++i) payload[i] ^= key[i & 3];
      pending = pending.subarray(header + 4 + size);
      const reply = Buffer.alloc(size < 126 ? 2 : 4);
      reply[0] = 128 | opcode;
      if (size < 126) reply[1] = size;
      else { reply[1] = 126; reply.writeUInt16BE(size, 2); }
      if (opcode === 8) { socket.end(Buffer.concat([reply, payload])); return; }
      assert(opcode === 1 || opcode === 2);
      ++echoes;
      socket.write(Buffer.concat([reply, payload]));
    }
  };
  socket.on('data', data => { pending = Buffer.concat([pending, data]); consume(); });
  consume();
});
(async () => {
  server.listen(0, '127.0.0.1');
  await once(server, 'listening');
  const base = `http://127.0.0.1:${server.address().port}`;
  try {
    for (const suffix of ['/json', '/redirect', '/gzip']) {
      const response = await fetch(base + suffix);
      assert.equal(response.status, 200);
      assert.deepEqual(await response.json(), {answer: 42});
      checks += 2;
    }
    assert.equal(await (await fetch(base + '/chunks')).text(), 'onetwothree');
    const payload = 'SPARC fetch \u2603\n'.repeat(100);
    assert.equal(await (await fetch(base + '/echo', {method: 'POST', body: payload})).text(), payload);
    await assert.rejects(fetch(base + '/abort', {signal: AbortSignal.timeout(25)}),
      error => error.name === 'TimeoutError');
    checks += 3;
    const ws = new WebSocket(base.replace('http:', 'ws:') + '/socket');
    ws.binaryType = 'arraybuffer';
    await once(ws, 'open');
    for (const text of ['hello SPARC', 'register windows \u2603']) {
      const result = once(ws, 'message');
      ws.send(text);
      const [event] = await result;
      assert.equal(event.data, text);
      ++checks;
    }
    const bytes = Uint8Array.from({length: 300}, (_, i) => i & 255);
    const binaryResult = once(ws, 'message');
    ws.send(bytes);
    const [binaryEvent] = await binaryResult;
    assert.deepEqual(new Uint8Array(binaryEvent.data), bytes);
    const closed = once(ws, 'close');
    ws.close(1000, 'done');
    const [event] = await closed;
    assert.equal(event.code, 1000);
    assert.equal(event.wasClean, true);
    assert.equal(echoes, 3);
    checks += 4;
    console.log(JSON.stringify({checks, echoes, fetch: true, websocket: true, passed: true}));
  } finally {
    for (const socket of sockets) socket.destroy();
    await new Promise(resolve => server.close(resolve));
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
