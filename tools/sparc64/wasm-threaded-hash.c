/* Copyright 2026 the Node.js contributors. All rights reserved.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file.
 */
#include <stddef.h>
#include <stdint.h>
#define XXH_INLINE_ALL
#define XXH_NO_STREAM
#define XXH_NO_XXH3
#include "deps/zstd/lib/common/xxhash.h"

#define EXPORT __attribute__((visibility("default")))

EXPORT uint64_t hash_block(const unsigned char* data, uint32_t length,
                           uint64_t seed) {
  return XXH64(data, length, seed);
}

/* Each instance has its own stack; the queue and output live in shared memory. */
EXPORT uint32_t hash_queue(uint32_t* next, uint32_t* completed,
                           const unsigned char* data, uint64_t* hashes,
                           uint32_t count, uint32_t block_size, uint64_t seed) {
  uint32_t worked = 0;
  for (;;) {
    uint32_t index = __atomic_fetch_add(next, 1, __ATOMIC_SEQ_CST);
    if (index >= count) break;
    hashes[index] = XXH64(data + (size_t)index * block_size, block_size,
                         seed + index);
    __atomic_fetch_add(completed, 1, __ATOMIC_SEQ_CST);
    ++worked;
  }
  return worked;
}

EXPORT uint32_t load32(uint32_t* address) {
  return __atomic_load_n(address, __ATOMIC_SEQ_CST);
}
EXPORT void store32(uint32_t* address, uint32_t value) {
  __atomic_store_n(address, value, __ATOMIC_SEQ_CST);
}
EXPORT void add_lanes(unsigned char* lanes, uint32_t worker,
                      uint32_t iterations) {
  for (uint32_t i = 0; i < iterations; ++i) {
    __atomic_fetch_add(lanes + worker, 1, __ATOMIC_SEQ_CST);
    __atomic_fetch_add((uint16_t*)(lanes + 8 + worker * 2), 1,
                       __ATOMIC_SEQ_CST);
    __atomic_fetch_add((uint32_t*)(lanes + 16 + worker * 4), 1,
                       __ATOMIC_SEQ_CST);
    __atomic_fetch_add((uint64_t*)(lanes + 32 + worker * 8), 1,
                       __ATOMIC_SEQ_CST);
  }
}

EXPORT uint32_t tear64(uint64_t* address, uint32_t worker,
                       uint32_t iterations) {
  const uint64_t a = UINT64_C(0x0123456789abcdef);
  const uint64_t b = UINT64_C(0xfedcba9876543210);
  uint32_t errors = 0;
  for (uint32_t i = 0; i < iterations; ++i) {
    __atomic_store_n(address, worker & 1 ? a : b, __ATOMIC_SEQ_CST);
    uint64_t seen = __atomic_load_n(address, __ATOMIC_SEQ_CST);
    if (seen != a && seen != b) ++errors;
  }
  return errors;
}

static void barrier(uint32_t* state, uint32_t* sense) {
  *sense ^= 1;
  if (__atomic_fetch_add(state, 1, __ATOMIC_SEQ_CST) == 1) {
    __atomic_store_n(state, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(state + 1, *sense, __ATOMIC_SEQ_CST);
  } else {
    while (__atomic_load_n(state + 1, __ATOMIC_SEQ_CST) != *sense) {}
  }
}

EXPORT void store_buffering(uint32_t* state, uint32_t worker,
                            uint32_t rounds) {
  uint32_t sense = 0;
  for (uint32_t i = 0; i < rounds; ++i) {
    if (worker == 0) {
      __atomic_store_n(state + 2, 0, __ATOMIC_SEQ_CST);
      __atomic_store_n(state + 3, 0, __ATOMIC_SEQ_CST);
    }
    barrier(state, &sense);
    __atomic_store_n(state + 2 + worker, 1, __ATOMIC_SEQ_CST);
    state[4 + worker] = __atomic_load_n(state + 3 - worker, __ATOMIC_SEQ_CST);
    barrier(state, &sense);
    if (worker == 0 && state[4] == 0 && state[5] == 0) ++state[6];
    barrier(state, &sense);
  }
}

#ifdef __wasm__
EXPORT __attribute__((optnone, noinline)) uint32_t poll_growth(uint32_t* ready, uint32_t previous) {
  __atomic_store_n(ready, 1, __ATOMIC_SEQ_CST);
  uint32_t pages;
  do {
    pages = __builtin_wasm_memory_size(0);
  } while (pages == previous);
  return pages;
}
EXPORT uint32_t wait32(uint32_t* address, uint32_t expected, int64_t timeout) {
  return __builtin_wasm_memory_atomic_wait32((int*)address, expected, timeout);
}
EXPORT uint32_t wait64(uint64_t* address, uint64_t expected, int64_t timeout) {
  return __builtin_wasm_memory_atomic_wait64((long long*)address, expected,
                                             timeout);
}
EXPORT uint32_t notify(uint32_t* address, uint32_t count) {
  return __builtin_wasm_memory_atomic_notify((int*)address, count);
}
#endif
