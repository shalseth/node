// Copyright 2026 the Node.js contributors. All rights reserved.
// Use of this source code is governed by an MIT-style license that can be
// found in the LICENSE file.

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <string>

#include "src/inspector/string-16.h"

int main() {
  const std::u16string expected = {u'a', 0x03c0, 0x6f22, 0x5b57,
                                  0xd83d, 0xde00, 0, 0xd7ff, 0xe000};
  for (size_t offset = 0; offset < 16; ++offset) {
    std::array<uint8_t, 128> bytes{};
    for (size_t i = 0; i < expected.size(); ++i) {
      bytes[offset + i * 2] = expected[i] & 0xff;
      bytes[offset + i * 2 + 1] = expected[i] >> 8;
    }
    const auto* data = bytes.data() + offset;
    auto chars = v8_inspector::String16::fromUTF16LE(
        reinterpret_cast<const char16_t*>(data), expected.size());
    auto words = v8_inspector::String16::fromUTF16LE(
        reinterpret_cast<const uint16_t*>(data), expected.size());
    if (chars.length() != expected.size() || words.length() != expected.size())
      return 1;
    for (size_t i = 0; i < expected.size(); ++i) {
      if (chars.characters16()[i] != expected[i] ||
          words.characters16()[i] != expected[i]) return 2;
    }
  }
  puts("INSPECTOR_UNALIGNED_UTF16_OK");
}
