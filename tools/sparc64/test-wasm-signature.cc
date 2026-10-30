// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <cstdio>

#include "src/codegen/signature.h"

using v8::internal::FixedSizeSignature;

template <typename S>
__attribute__((noinline)) bool Check(const S& sig, uint32_t expected) {
  uintptr_t first = reinterpret_cast<uintptr_t>(&sig);
  uintptr_t data = reinterpret_cast<uintptr_t>(sig.returns().begin());
  return data >= first && data + sizeof(uint32_t) <= first + sizeof(sig) &&
         sig.returns()[0] == expected;
}

__attribute__((noinline)) auto Make(uint32_t value) {
  return FixedSizeSignature<uint32_t>::Returns(value);
}

int main() {
  auto original = Make(0x1710);
  if (!Check(original, 0x1710)) return 1;
  auto copied = original;
  if (!Check(copied, 0x1710)) return 2;
  auto assigned = Make(0x1810);
  assigned = copied;
  if (!Check(assigned, 0x1710)) return 3;
  auto combined = original.Params(uint32_t{0x1910});
  if (!Check(combined, 0x1710) || combined.parameters()[0] != 0x1910) return 4;
  std::puts("SIGNATURE_STORAGE_OK");
}
