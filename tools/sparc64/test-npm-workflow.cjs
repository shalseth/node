// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const root = path.resolve(__dirname, '../..');
const workspace = fs.mkdtempSync(path.join(root, 'out/sparc64-npm-workflow-'));
const fixture = path.join(workspace, 'fixture');
const consumer = path.join(workspace, 'consumer');
fs.mkdirSync(fixture);
fs.mkdirSync(consumer);
const env = { ...process.env, PATH: `${path.dirname(process.execPath)}:${process.env.PATH}` };
function npm(cwd, ...args) {
  const result = spawnSync(process.execPath, [path.join(root, 'deps/npm/bin/npm-cli.js'),
    ...args], { cwd, env, encoding: 'utf8', timeout: 20_000 });
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${args.join(' ')}\n${result.stdout}\n${result.stderr}`);
  return result.stdout;
}
try {
  fs.writeFileSync(path.join(fixture, 'package.json'), JSON.stringify({
    name: 'sparc64-runtime-fixture', version: '1.0.0', main: 'index.cjs',
    bin: { 'sparc64-runtime-fixture': 'cli.cjs' },
    scripts: { test: 'node cli.cjs' },
  }));
  fs.writeFileSync(path.join(fixture, 'index.cjs'),
                   'module.exports = (a, b) => a + b;\n');
  fs.writeFileSync(path.join(fixture, 'cli.cjs'),
    '#!/usr/bin/env node\n' +
    'console.log(JSON.stringify({arch:process.arch,execPath:process.execPath,' +
    'result:require("./index.cjs")(19,23)}));\n');
  const lifecycle = npm(fixture, 'run', 'test');
  assert.match(lifecycle, /"arch":"sparc64"/);
  assert.match(lifecycle, /"result":42/);
  assert.ok(lifecycle.includes(process.execPath));
  const packed = JSON.parse(npm(fixture, 'pack', '--ignore-scripts', '--json', '--offline'));
  assert.equal(packed.length, 1);
  fs.writeFileSync(path.join(consumer, 'package.json'),
                   JSON.stringify({ name: 'sparc64-consumer', version: '1.0.0', private: true }));
  npm(consumer, 'install', '--offline', '--ignore-scripts', '--no-audit', '--no-fund',
      path.join(fixture, packed[0].filename));
  npm(consumer, 'ci', '--offline', '--ignore-scripts', '--no-audit', '--no-fund');
  const executed = JSON.parse(npm(consumer, 'exec', '--offline', '--', 'sparc64-runtime-fixture'));
  assert.deepEqual(executed, { arch: 'sparc64', execPath: process.execPath, result: 42 });
  console.log('NPM_WORKFLOW_OK lifecycle, pack, local install, clean install and executable');
} finally {
  fs.rmSync(workspace, { recursive: true, force: true });
}
