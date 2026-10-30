/* Copyright 2026 the Node.js contributors. All rights reserved.
 * Use of this source code is governed by an MIT-style license that can be
 * found in the LICENSE file.
 */
#include <stddef.h>
void* memcpy(void* dst, const void* src, size_t length) {
  unsigned char* d = dst;
  const unsigned char* s = src;
  for (size_t i = 0; i < length; ++i) d[i] = s[i];
  return dst;
}
void* memset(void* dst, int value, size_t length) {
  unsigned char* d = dst;
  for (size_t i = 0; i < length; ++i) d[i] = value;
  return dst;
}
int memcmp(const void* a, const void* b, size_t length) {
  const unsigned char* x = a;
  const unsigned char* y = b;
  for (size_t i = 0; i < length; ++i) {
    if (x[i] != y[i]) return x[i] - y[i];
  }
  return 0;
}
