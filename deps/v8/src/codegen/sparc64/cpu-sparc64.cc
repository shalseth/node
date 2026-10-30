// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <unistd.h>

#include <cstdio>

#include "src/codegen/cpu-features.h"

namespace v8::internal {

void CpuFeatures::ProbeImpl(bool cross_compile) {
  supported_.Add(FPU);
  icache_line_size_ = 32;
  dcache_line_size_ = 32;
  if (cross_compile) return;
  long icache = sysconf(_SC_LEVEL1_ICACHE_LINESIZE);
  long dcache = sysconf(_SC_LEVEL1_DCACHE_LINESIZE);
  if (icache > 0) icache_line_size_ = static_cast<unsigned>(icache);
  if (dcache > 0) dcache_line_size_ = static_cast<unsigned>(dcache);
}

bool CpuFeatures::SupportsWasmSimd128() { return true; }

void CpuFeatures::PrintTarget() { printf("target sparc64\n"); }

void CpuFeatures::PrintFeatures() {
  printf("FPU=%d\n", CpuFeatures::IsSupported(FPU));
}

void CpuFeatures::FlushICache(void* start, size_t size) {
  auto* begin = static_cast<char*>(start);
  __builtin___clear_cache(begin, begin + size);
}

}  // namespace v8::internal
