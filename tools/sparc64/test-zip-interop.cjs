// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

'use strict';
const fs = require('node:fs/promises');
const path = require('node:path');
const zlib = require('node:zlib');
(async () => {
  const entries = [];
  for (const [name, data] of [['hello.txt', 'hello'], ['unicode-名前.txt', '漢字😀'],
                             ['raw.bin', Buffer.from([0, 1, 254, 255])]]) {
    entries.push(await zlib.ZipEntry.create(name, Buffer.from(data)));
  }
  const chunks = [];
  for await (const chunk of zlib.createZipArchive(entries)) chunks.push(chunk);
  const directory = path.resolve('out/sparc64-hardening-zip');
  await fs.mkdir(directory, {recursive: true});
  await fs.writeFile(path.join(directory, 'node.zip'), Buffer.concat(chunks));
  console.log('NODE_ZIP_ARCHIVE_WRITTEN');
})().catch(error => { console.error(error); process.exitCode = 1; });
