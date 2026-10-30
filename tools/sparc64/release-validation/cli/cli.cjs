#!/usr/bin/env node
'use strict';
const assert = require('node:assert/strict');
assert.equal(process.argv[2], 'SPARC64');
assert.equal(process.execPath, process.env.SPARC64_EXPECT_NODE);
console.log('INSTALLED_NPX_CLI_OK ' + JSON.stringify({passed: true, execPath: process.execPath}));
