// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <cstdint>
#include <cstdio>

#include "src/base/page-allocator.h"

int main() {
  v8::base::PageAllocator allocator;
  constexpr uintptr_t kAddressLimit = uintptr_t{1} << 48;
  const size_t page = allocator.AllocatePageSize();
  void* hints[] = {nullptr, reinterpret_cast<void*>(0xfff8001000000000ULL),
                   allocator.GetRandomMmapAddr()};
  for (void* hint : hints) {
    const size_t size = 2 * page;
    const size_t alignment = 8 * page;
    void* result = allocator.AllocatePages(hint, size, alignment,
                                           v8::PageAllocator::kReadWrite);
    const uintptr_t address = reinterpret_cast<uintptr_t>(result);
    if (!result || address >= kAddressLimit || size > kAddressLimit - address ||
        address % alignment != 0) {
      std::fprintf(stderr, "FAIL hint=%p allocation=%p size=%zu\n", hint,
                   result, size);
      if (result) allocator.FreePages(result, size);
      return 1;
    }
    auto* bytes = static_cast<volatile uint8_t*>(result);
    bytes[0] = 0x37;
    bytes[size - 1] = 0x73;
    if (bytes[0] != 0x37 || bytes[size - 1] != 0x73) return 1;
    allocator.FreePages(result, size);
  }
  const size_t too_large = kAddressLimit + page;
  void* rejected = allocator.AllocatePages(nullptr, too_large, page,
                                           v8::PageAllocator::kNoAccess);
  if (rejected != nullptr) {
    allocator.FreePages(rejected, too_large);
    std::fputs("FAIL allocation exceeds the representable range\n", stderr);
    return 1;
  }
  std::puts(
      "PASS low-48-bit allocation: null, high and random hints; "
      "oversized reservation rejected");
}
