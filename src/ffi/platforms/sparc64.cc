#if HAVE_FFI

#include "ffi/fast.h"

#if defined(__sparc__) && defined(__arch64__)

#include <sys/mman.h>

#include <cstdint>

namespace {

using node::ffi::FastFFIType;

uint32_t Format3(unsigned op, unsigned rd, unsigned op3, unsigned rs1,
                 bool immediate, unsigned operand) {
  return (op << 30) | (rd << 25) | (op3 << 19) | (rs1 << 14) |
         (immediate ? 1u << 13 : 0) | operand;
}

void Emit(uint32_t** cursor, uint32_t instruction) {
  *(*cursor)++ = instruction;
}

bool IsFloatingPoint(FastFFIType type) {
  return type == FastFFIType::kFloat32 || type == FastFFIType::kFloat64;
}

bool IsNarrow(FastFFIType type) {
  return type == FastFFIType::kInt8 || type == FastFFIType::kUint8 ||
         type == FastFFIType::kInt16 || type == FastFFIType::kUint16;
}

void Extend(uint32_t** cursor, unsigned reg, FastFFIType type) {
  unsigned bits;
  bool sign;
  switch (type) {
    case FastFFIType::kInt8:
      bits = 8;
      sign = true;
      break;
    case FastFFIType::kUint8:
      bits = 8;
      sign = false;
      break;
    case FastFFIType::kInt16:
      bits = 16;
      sign = true;
      break;
    case FastFFIType::kUint16:
      bits = 16;
      sign = false;
      break;
    case FastFFIType::kInt32:
      bits = 32;
      sign = true;
      break;
    case FastFFIType::kUint32:
      bits = 32;
      sign = false;
      break;
    default:
      return;
  }
  const unsigned shift = (1u << 12) | (64 - bits);
  Emit(cursor, Format3(2, reg, 0x25, reg, true, shift));
  Emit(cursor, Format3(2, reg, sign ? 0x27 : 0x26, reg, true, shift));
}

}  // namespace

extern "C" bool node_ffi_create_fast_trampoline(
    const node::ffi::FastFFITrampolineConfig& config,
    const node::ffi::FastFFIType* args, size_t argc,
    node::ffi::FastFFIType result, node::ffi::FastFFITrampoline* out) {
  if (config.target == nullptr || out == nullptr || argc > 8 ||
      IsFloatingPoint(result) || IsNarrow(result)) {
    return false;
  }
  for (size_t n = 0; n < argc; ++n) {
    if (IsFloatingPoint(args[n]) || args[n] == FastFFIType::kBuffer ||
        args[n] == FastFFIType::kVoid) {
      return false;
    }
  }

  constexpr size_t kCodeSize = 256;
  void* code = mmap(nullptr, kCodeSize, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANON, -1, 0);
  if (code == MAP_FAILED) return false;
  uint32_t* cursor = static_cast<uint32_t*>(code);
  // Remove V8's receiver slot from the positional SPARC V9 parameter array.
  for (size_t n = 0; n < argc; ++n) {
    const unsigned dst = n < 6 ? 8 + n : 2;
    if (n < 5) {
      Emit(&cursor, Format3(2, dst, 2, 9 + n, false, 0));
    } else {
      Emit(&cursor, Format3(3, dst, 0x0b, 14, true, 2047 + 128 + (n + 1) * 8));
    }
    Extend(&cursor, dst, args[n]);
    if (n >= 6) {
      Emit(&cursor, Format3(3, dst, 0x0e, 14, true, 2047 + 128 + n * 8));
    }
  }
  const uintptr_t target = reinterpret_cast<uintptr_t>(config.target);
  Emit(&cursor, (1u << 25) | (4u << 22) | ((target >> 42) & 0x3fffff));
  Emit(&cursor, Format3(2, 1, 2, 1, true, (target >> 32) & 0x3ff));
  Emit(&cursor, Format3(2, 1, 0x25, 1, true, (1u << 12) | 32));
  Emit(&cursor, (2u << 25) | (4u << 22) | ((target >> 10) & 0x3fffff));
  Emit(&cursor, Format3(2, 2, 2, 2, true, target & 0x3ff));
  Emit(&cursor, Format3(2, 1, 2, 1, false, 2));
  Emit(&cursor, Format3(2, 0, 0x38, 1, false, 0));
  Emit(&cursor, 0x01000000);
  const size_t written =
      reinterpret_cast<char*>(cursor) - static_cast<char*>(code);
  __builtin___clear_cache(static_cast<char*>(code),
                          static_cast<char*>(code) + written);
  if (mprotect(code, kCodeSize, PROT_READ | PROT_EXEC) != 0) {
    munmap(code, kCodeSize);
    return false;
  }
  *out = {code, kCodeSize};
  return true;
}

extern "C" void node_ffi_free_fast_trampoline(
    node::ffi::FastFFITrampoline* trampoline) {
  if (trampoline != nullptr && trampoline->code != nullptr) {
    munmap(trampoline->code, trampoline->size);
    *trampoline = {};
  }
}

#endif  // defined(__sparc__) && defined(__arch64__)
#endif  // HAVE_FFI
