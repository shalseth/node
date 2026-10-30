#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
cd "$(dirname "$0")/../.."
root=$(pwd)
output="$root/out/sparc64-addon-gc"
mkdir -p "$output"
timeout 30 "${CC:-gcc}" -std=c11 -O2 -mcpu=v9 -fPIC -shared \
  -I"$root/src" -I"$root" -DNAPI_VERSION=10 \
  -DNODE_GYP_MODULE_NAME=sparc64_addon_gc \
  tools/sparc64/addon-gc.c -o "$output/addon-gc.node"
timeout 30 "$root/out/Release/node" --expose-gc \
  --compact-on-every-full-gc --max-old-space-size=96 --trace-gc \
  tools/sparc64/test-addon-gc.cjs "$output/addon-gc.node"
echo ADDON_GC_OK
