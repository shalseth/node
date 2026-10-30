// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';

const assert = require('node:assert/strict');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const { mkdtemp, writeFile, rm } = require('node:fs/promises');
const http = require('node:http');
const net = require('node:net');
const os = require('node:os');
const path = require('node:path');

let child;
const deadline = setTimeout(() => {
  child?.kill('SIGKILL');
  console.error('http-server smoke test exceeded 20 seconds');
  process.exit(1);
}, 20_000);

async function main() {
  const root = await mkdtemp(path.join(os.tmpdir(), 'sparc64-http-server-'));
  try {
    const body = 'Node 26 on SPARC64: static files work.\n';
    await writeFile(path.join(root, 'index.html'), body);
    const reservation = net.createServer();
    reservation.listen(0, '127.0.0.1');
    await once(reservation, 'listening');
    const port = reservation.address().port;
    await new Promise((resolve) => reservation.close(resolve));
    child = spawn(process.execPath, [
      require.resolve('http-server/bin/http-server'), root,
      '-a', '127.0.0.1', '-p', String(port), '-c-1',
    ], { stdio: ['ignore', 'pipe', 'pipe'] });
    let output = '';
    await new Promise((resolve, reject) => {
      child.on('error', reject);
      child.on('exit', (code) => reject(new Error(`http-server exited: ${code}\n${output}`)));
      const collect = (data) => {
        output += data;
        if (output.includes(`127.0.0.1:${port}`)) resolve();
      };
      child.stdout.on('data', collect);
      child.stderr.on('data', collect);
    });
    const response = await new Promise((resolve, reject) => {
      http.get(`http://127.0.0.1:${port}/`, (res) => {
        let text = '';
        res.setEncoding('utf8');
        res.on('data', (chunk) => { text += chunk; });
        res.on('error', reject);
        res.on('end', () => resolve({ code: res.statusCode, text }));
      }).on('error', reject);
    });
    assert.deepEqual(response, { code: 200, text: body });
    console.log('PASS published http-server CLI serves static content');
  } finally {
    if (child && child.exitCode === null) {
      const exited = once(child, 'exit');
      child.kill('SIGTERM');
      await exited;
    }
    await rm(root, { recursive: true, force: true });
  }
}

main().then(() => clearTimeout(deadline), (error) => {
  child?.kill('SIGKILL');
  console.error(error);
  process.exit(1);
});
