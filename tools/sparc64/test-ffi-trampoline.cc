// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.
#include "ffi/fast.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

using node::ffi::FastFFITrampoline;
using node::ffi::FastFFITrampolineConfig;
using node::ffi::FastFFIType;
static unsigned checks;
void Check(bool value) {
  if (!value) {
    std::fprintf(stderr, "FFI trampoline check %u failed\n", checks + 1);
    std::exit(1);
  }
  ++checks;
}
extern "C" uint64_t Zero() { return 0x123456789abcdef0ull; }
extern "C" uint64_t Eight(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                           uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  return a * 3 + b * 5 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23;
}
extern "C" uint64_t Mixed(int8_t a, uint8_t b, int16_t c, uint16_t d,
                           int32_t e, uint32_t f, int64_t g, uint64_t h) {
  return static_cast<int64_t>(a) * 3 + b * 5 + static_cast<int64_t>(c) * 7 +
         d * 11 + static_cast<int64_t>(e) * 13 + static_cast<uint64_t>(f) * 17 +
         g * 19 + h * 23;
}
using CallEight = uint64_t (*)(void*, uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t, uint64_t, uint64_t, uint64_t);
int main() {
  FastFFITrampoline trampoline;
  FastFFITrampolineConfig config{reinterpret_cast<void*>(Zero), nullptr, nullptr};
  Check(node_ffi_create_fast_trampoline(config, nullptr, 0,
                                        FastFFIType::kUint64, &trampoline));
  Check(reinterpret_cast<uint64_t (*)(void*)>(trampoline.code)(nullptr) == Zero());
  node_ffi_free_fast_trampoline(&trampoline);
  Check(trampoline.code == nullptr && trampoline.size == 0);
  FastFFIType args[8];
  for (auto& arg : args) arg = FastFFIType::kUint64;
  config.target = reinterpret_cast<void*>(Eight);
  Check(node_ffi_create_fast_trampoline(config, args, 8,
                                        FastFFIType::kUint64, &trampoline));
  auto call = reinterpret_cast<CallEight>(trampoline.code);
  for (uint64_t n = 0; n < 128; ++n) {
    const uint64_t a = 0x123456789abcdef0ull + n;
    Check(call(nullptr, a, n, n * 2, n * 3, n * 5, n * 7, n * 11, n * 13) ==
          Eight(a, n, n * 2, n * 3, n * 5, n * 7, n * 11, n * 13));
  }
  node_ffi_free_fast_trampoline(&trampoline);
  FastFFIType mixed[] = {FastFFIType::kInt8, FastFFIType::kUint8,
    FastFFIType::kInt16, FastFFIType::kUint16, FastFFIType::kInt32,
    FastFFIType::kUint32, FastFFIType::kInt64, FastFFIType::kUint64};
  config.target = reinterpret_cast<void*>(Mixed);
  Check(node_ffi_create_fast_trampoline(config, mixed, 8,
                                        FastFFIType::kUint64, &trampoline));
  call = reinterpret_cast<CallEight>(trampoline.code);
  Check(call(nullptr, 0x80, 255, 0x8000, 65535, 0x80000000, 0xffffffff,
             static_cast<uint64_t>(-7), 0xfedcba9876543210ull) ==
        Mixed(-128, 255, -32768, 65535, INT32_MIN, UINT32_MAX, -7,
              0xfedcba9876543210ull));
  node_ffi_free_fast_trampoline(&trampoline);
  mixed[0] = FastFFIType::kFloat64;
  Check(!node_ffi_create_fast_trampoline(config, mixed, 8,
                                         FastFFIType::kUint64, &trampoline));
  std::printf("FFI_TRAMPOLINE_OK {\"passed\":true,\"checks\":%u}\n", checks);
}
