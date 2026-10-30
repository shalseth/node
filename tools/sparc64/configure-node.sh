#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
cd "$(dirname "$0")/../.."
exec ./configure --dest-cpu=sparc64 --enable-d8 --v8-lite-mode \
  --v8-disable-maglev --v8-disable-temporal-support \
  --shared-openssl --with-intl=system-icu \
  --without-node-snapshot --without-node-code-cache \
  --without-amaro --without-ffi \
  -Dv8_enable_sparkplug=0 -Dv8_enable_turbofan=0 "$@"
