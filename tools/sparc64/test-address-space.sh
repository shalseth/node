#!/bin/sh
# Copyright 2026 the Node.js contributors. All rights reserved.
# Use of this source code is governed by an MIT-style license that can be
# found in the LICENSE file.

set -eu
cd "$(dirname "$0")/../.."
mkdir -p out/sparc64-address-space
libs=out/Release/obj.target/tools/v8_gypfiles
timeout 60 "${CXX:-g++}" -std=gnu++20 -O2 -pthread \
  -DV8_TARGET_ARCH_SPARC64 -Ideps/v8 -Ideps/v8/include \
  -Ideps/v8/third_party/abseil-cpp tools/sparc64/test-address-space.cc \
  "$libs/libv8_libbase.a" "$libs/libabseil.a" -ldl -lrt -latomic \
  -o out/sparc64-address-space/test-address-space
exec timeout 10 out/sparc64-address-space/test-address-space
