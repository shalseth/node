// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <new>
#if ALIGNED_HEAD
struct alignas(uint64_t) Head {
#else
struct Head {
#endif
  uint32_t next;
  uint32_t length;
};
__attribute__((noinline)) bool exchange(std::atomic<Head>* table, Head* expected) {
  return table->compare_exchange_strong(*expected, Head{3, 4});
}
int main() {
  std::atomic<Head> table{Head{1, 2}};
  alignas(16) unsigned char bytes[32];
  auto* expected = new (bytes + alignof(Head)) Head{1, 2};
  std::printf("alignment=%zu expected_mod8=%zu\n", alignof(Head),
      reinterpret_cast<uintptr_t>(expected) & 7);
  std::fflush(stdout);
  if (!exchange(&table, expected)) return 1;
  auto value = table.load();
  if (value.next != 3 || value.length != 4) return 2;
  std::puts("ATOMIC_EXCHANGE_OK");
}
