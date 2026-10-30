#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.
set -eu
cd "$(dirname "$0")/../.."
stage=${1:-amaro}
if [ "$#" -gt 0 ]; then shift; fi
case "$stage" in
  amaro) set -- --v8-disable-temporal-support --without-ffi "$@" ;;
  temporal) set -- --v8-enable-temporal-support --without-ffi "$@" ;;
  ffi) set -- --v8-disable-temporal-support "$@" ;;
  all) set -- --v8-enable-temporal-support "$@" ;;
  *) echo "Unknown feature stage: $stage" >&2; exit 2 ;;
esac
exec ./configure --dest-cpu=sparc64 --enable-d8 --v8-disable-maglev \
  --v8-enable-verify-heap \
  --shared-openssl --with-intl=system-icu \
  -Dv8_enable_sparkplug=1 -Dv8_enable_turbofan=1 \
  "$@"
