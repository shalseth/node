#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
umask 077
cd "$(dirname "$0")/../.."
root=$(pwd)
node="$root/out/Release/node"
npm="$root/deps/npm/bin/npm-cli.js"
apps="$root/out/sparc64-app-smoke"
mkdir -p "$apps/certs"
cp tools/sparc64/apps/package*.json tools/sparc64/apps/*.cjs "$apps/"

timeout 20 "$node" -e '
  const assert = require("node:assert/strict");
  assert.equal(process.arch, "sparc64");
  assert.ok(process.versions.openssl);
  assert.ok(process.versions.icu);
  assert.equal(new Intl.NumberFormat("en-US").format(1234.5), "1,234.5");
  console.log(JSON.stringify(process.versions));
'
timeout 20 "$node" "$npm" --version
openssl req -x509 -newkey rsa:2048 -nodes -days 1 \
  -subj /CN=localhost -addext subjectAltName=DNS:localhost,IP:127.0.0.1 \
  -keyout "$apps/certs/key.pem" -out "$apps/certs/cert.pem" \
  >"$apps/certificate.log" 2>&1
cd "$apps"
timeout 90 "$node" "$npm" ci --ignore-scripts --omit=optional \
  --no-audit --no-fund
timeout 20 "$node" "$npm" ls --depth=0
SPARC64_SMOKE_CERT_DIR="$apps/certs" timeout 30 "$node" express-websocket.cjs
timeout 30 "$node" http-server.cjs
echo APPLICATION_SMOKE_OK
