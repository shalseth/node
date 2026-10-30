#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>

static int last_call_was_jit;
static uint64_t observed[8];

uint64_t weighted_two(uint64_t a, uint64_t b) {
  Dl_info info;
  void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
  last_call_was_jit = dladdr(caller, &info) == 0;
  return a * 3 + b * 5;
}

uint64_t weighted_eight(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                        uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  Dl_info info;
  void* caller = __builtin_extract_return_addr(__builtin_return_address(0));
  last_call_was_jit = dladdr(caller, &info) == 0;
  observed[0] = a;
  observed[1] = b;
  observed[2] = c;
  observed[3] = d;
  observed[4] = e;
  observed[5] = f;
  observed[6] = g;
  observed[7] = h;
  return a * 3 + b * 5 + c * 7 + d * 11 + e * 13 + f * 17 + g * 19 + h * 23;
}

int32_t call_was_jit(void) {
  return last_call_was_jit;
}

uint64_t observed_argument(uint32_t index) {
  return observed[index & 7];
}

#include <stdlib.h>

void* allocate_bytes(uint64_t size) { return calloc(1, size); }
void release_bytes(void* memory) { free(memory); }
uint64_t checksum_bytes(const unsigned char* memory, uint64_t size) {
  uint64_t result = 0;
  for (uint64_t i = 0; i < size; i++) result += memory[i] * (i + 1);
  return result;
}
int32_t callback_twice(int32_t (*fn)(int32_t), int32_t value) {
  return fn(value) + fn(value + 1);
}
