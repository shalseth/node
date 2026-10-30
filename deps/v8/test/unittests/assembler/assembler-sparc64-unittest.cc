// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cstring>
#include <vector>

#include "gtest/gtest.h"
#include "src/codegen/register.h"
#include "src/codegen/sparc64/base-assembler-sparc64.h"

#if V8_HOST_ARCH_SPARC64
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace v8::internal {
namespace {

class BufferedAssembler : public AssemblerSparc64Base {
 public:
  void dq(uint64_t value) {
    emit(static_cast<Instr>(value >> 32));
    emit(static_cast<Instr>(value));
  }
  const std::vector<Instr>& instructions() const { return instructions_; }
  std::vector<uint8_t> bytes() const {
    std::vector<uint8_t> result;
    for (Instr instr : instructions_) {
      for (int shift = 24; shift >= 0; shift -= 8) {
        result.push_back(static_cast<uint8_t>(instr >> shift));
      }
    }
    return result;
  }

 private:
  void emit(Instr instr) override { instructions_.push_back(instr); }
  std::vector<Instr> instructions_;
};

TEST(AssemblerSparc64Test, Registers) {
  EXPECT_EQ(g0.code(), 0);
  EXPECT_EQ(o0.code(), 8);
  EXPECT_EQ(l0.code(), 16);
  EXPECT_EQ(i0.code(), 24);
  EXPECT_EQ(sp, o6);
  EXPECT_EQ(fp, l5);
  EXPECT_EQ(kNativeFramePointer, i6);
  EXPECT_EQ(d0.encoding(), 0);
  EXPECT_EQ(d15.encoding(), 30);
  EXPECT_EQ(d16.encoding(), 1);
  EXPECT_EQ(d31.encoding(), 31);
  EXPECT_STREQ(RegisterName(g7), "g7");
  EXPECT_STREQ(RegisterName(d31), "d31");
}

TEST(AssemblerSparc64Test, InstructionWords) {
  BufferedAssembler a;
  a.nop();
  a.add(o0, o0, o1);
  a.mov(o0, -4096);
  a.mov(o0, 4095);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.retl();
  a.ret();
  a.restore();
  const std::vector<Instr> expected = {0x01000000, 0x90020009, 0x90103000,
                                       0x90102fff, 0x9de3bf50, 0x81c3e008,
                                       0x81c7e008, 0x81e80000};
  EXPECT_EQ(a.instructions(), expected);
}

TEST(AssemblerSparc64Test, BigEndianOutput) {
  BufferedAssembler a;
  a.add(o0, o0, o1);
  const std::vector<uint8_t> expected = {0x90, 0x02, 0x00, 0x09};
  EXPECT_EQ(a.bytes(), expected);
}

TEST(AssemblerSparc64Test, SharedRegisterLists) {
  for (Register reg : {g0, g6, g7, sp, o7, i6, i7, l5, kRootRegister}) {
    EXPECT_FALSE(kAllocatableGeneralRegisters.has(reg));
  }
  EXPECT_TRUE(kAllocatableGeneralRegisters.has(o0));
  EXPECT_TRUE(kAllocatableGeneralRegisters.has(kContextRegister));
  EXPECT_EQ(kAllocatableDoubleRegisters.Count(), 15u);
  EXPECT_FALSE(kAllocatableDoubleRegisters.has(kScratchDoubleReg));
  for (DoubleRegister reg : kAllocatableDoubleRegisters) {
    EXPECT_EQ(reg.single_encoding(), reg.encoding());
  }
  EXPECT_EQ(kNumCalleeSavedDoubles, 0);
  EXPECT_EQ(kNumCallerSavedDoubles, 32);
}

TEST(AssemblerSparc64Test, BranchDisplacements) {
  BufferedAssembler a;
  a.b(equal, ConditionCodes::kXcc, 0);
  a.b(not_equal, ConditionCodes::kIcc, -4);
  a.b(always, ConditionCodes::kXcc, -(1 << 20), true);
  a.b(always, ConditionCodes::kXcc, (1 << 20) - 4);
  a.call(0);
  a.call(-4);
  a.call(-(int64_t{1} << 31));
  a.call((int64_t{1} << 31) - 4);
  const std::vector<Instr> expected = {0x02680000, 0x124fffff, 0x306c0000,
                                       0x106bffff, 0x40000000, 0x7fffffff,
                                       0x60000000, 0x5fffffff};
  EXPECT_EQ(a.instructions(), expected);
}

#if V8_HOST_ARCH_SPARC64

extern "C" const Instr sparc64_reference_begin[];
extern "C" const Instr sparc64_reference_end[];

TEST(AssemblerSparc64Test, GnuAssemblerReference) {
  BufferedAssembler a;
  a.sllx(o0, o1, 63);
  a.srlx(o0, o1, 32);
  a.srax(o0, o1, 0);
  a.ldx(o0, o1, -4096);
  a.lduw(o0, o1, 4095);
  a.ldsw(o0, o1, g1);
  a.ldub(o0, o1, 7);
  a.lduh(o0, o1, 6);
  a.ldsb(o0, o1, 0);
  a.ldsh(o0, o1, 2);
  a.stx(o0, o1, 16);
  a.stw(o0, o1, g1);
  a.stb(o0, o1, 1);
  a.sth(o0, o1, 2);
  a.membar(kSync);
  a.membar(kLoadLoad | kLoadStore | kStoreStore);
  a.casx(o2, o0, o1);
  a.faddd(d0, d1, d2);
  a.fsubd(d16, d17, d31);
  a.fmuld(d31, d16, d15);
  a.fdivd(d15, d31, d16);
  a.ldd(d16, o0, 0);
  a.std(d31, o0, 8);
  a.mulx(o0, o1, o2);
  a.sdivx(o0, o1, -1);
  a.udivx(o0, o1, o2);
  a.addcc(o0, o1, o2);
  a.subcc(o0, o1, 1);
  a.and_(o0, o1, o2);
  a.or_(o0, o1, 4095);
  a.xor_(o0, o1, o2);
  a.sethi(o0, 0x3fffff);
  a.b(equal, ConditionCodes::kXcc, 16);
  a.nop();
  a.b(not_equal, ConditionCodes::kIcc, -4, true);
  a.nop();
  a.call(16);
  a.nop();
  a.call(-4);
  a.nop();
  a.rdpc(o0);
  a.nop();
  a.ba(16);
  a.nop();
  a.flushw();
  a.sll(o0, o1, 31);
  a.srl(o0, o1, 0);
  a.sra(o0, o1, 16);
  a.sll(o0, o1, o2);
  a.srl(o0, o1, o2);
  a.sra(o0, o1, o2);
  a.sllx(o0, o1, o2);
  a.srlx(o0, o1, o2);
  a.srax(o0, o1, o2);
  a.fmovd(d16, d0);
  a.fnegd(d31, d16);
  a.fabsd(d0, d31);
  a.fsqrtd(d31, d16);
  a.fcmpd(d0, d1);
  a.fcmpd(d16, d31, 3);
  a.fb(kFUnordered, 0, 16);
  a.nop();
  a.fb(kFLess, 3, -4, true);
  a.nop();
  a.ta(1);
  a.ldf(d15, o0, 4);
  a.stf(d14, o0, -4);
  a.fmovs(d15, d14);
  a.andcc(g0, o0, o1);
  a.fadds(d13, d14, d15);
  a.fsubs(d14, d15, d13);
  a.fmuls(d15, d13, d14);
  a.fdivs(d13, d14, d15);
  a.fnegs(d14, d13);
  a.fabss(d15, d14);
  a.fsqrts(d13, d15);
  a.fcmps(d14, d15, 3);
  a.fxtod(d31, d16);
  a.fxtod(d0, d31);
  a.fxtos(d15, d31);
  a.fxtos(d0, d16);
  a.fdtos(d15, d31);
  a.fstod(d31, d15);
  a.fdtos(d0, d16);
  a.fstod(d16, d0);
  ASSERT_EQ(a.instructions().size(),
            (reinterpret_cast<uintptr_t>(sparc64_reference_end) -
             reinterpret_cast<uintptr_t>(sparc64_reference_begin)) /
                sizeof(Instr));
  for (size_t i = 0; i < a.instructions().size(); ++i) {
    EXPECT_EQ(a.instructions()[i], sparc64_reference_begin[i]) << i;
  }
}

class ExecutableCode {
 public:
  ~ExecutableCode() {
    if (memory_ != MAP_FAILED) munmap(memory_, size_);
  }

  bool Initialize(const BufferedAssembler& a) {
    const auto bytes = a.bytes();
    const size_t page_size = sysconf(_SC_PAGESIZE);
    size_ = (bytes.size() + page_size - 1) & ~(page_size - 1);
    memory_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_ == MAP_FAILED) return false;
    memcpy(memory_, bytes.data(), bytes.size());
    if (mprotect(memory_, size_, PROT_READ | PROT_EXEC) != 0) return false;
    auto* start = static_cast<char*>(memory_);
    __builtin___clear_cache(start, start + bytes.size());
    return true;
  }

  template <typename F>
  F entry() const {
    return reinterpret_cast<F>(memory_);
  }

 private:
  void* memory_ = MAP_FAILED;
  size_t size_ = 0;
};

void SetConstant(BufferedAssembler& a, Register rd, uint64_t value) {
  a.mov(rd, static_cast<int32_t>(value >> 60));
  for (int shift = 48; shift >= 0; shift -= 12) {
    a.sllx(rd, rd, 12);
    a.or_(rd, rd, static_cast<int32_t>((value >> shift) & 0xfff));
  }
}

TEST(AssemblerSparc64Test, NativeRegisterShifts) {
  for (bool wide : {false, true}) {
    for (int operation = 0; operation < 3; ++operation) {
      BufferedAssembler a;
      if (wide) {
        if (operation == 0) a.sllx(o0, o0, o1);
        if (operation == 1) a.srlx(o0, o0, o1);
        if (operation == 2) a.srax(o0, o0, o1);
      } else {
        if (operation == 0) {
          a.sll(o0, o0, o1);
          a.srl(o0, o0, 0);
        }
        if (operation == 1) a.srl(o0, o0, o1);
        if (operation == 2) a.sra(o0, o0, o1);
      }
      a.retl();
      a.nop();
      ExecutableCode code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
      const uint64_t value = 0xfedcba9887654321;
      for (unsigned count : {0, 1, 31, 32, 63, 64, 127}) {
        const unsigned shift = count & (wide ? 63 : 31);
        uint64_t expected;
        if (operation == 0) {
          expected =
              wide ? value << shift : static_cast<uint32_t>(value << shift);
        } else if (operation == 1) {
          expected =
              wide ? value >> shift : static_cast<uint32_t>(value) >> shift;
        } else {
          const int64_t input =
              wide ? static_cast<int64_t>(value) : static_cast<int32_t>(value);
          expected = static_cast<uint64_t>(input >> shift);
        }
        EXPECT_EQ(entry(value, count), expected)
            << "wide=" << wide << " operation=" << operation
            << " count=" << count;
      }
    }
  }
}

TEST(AssemblerSparc64Test, NativeLeafArithmetic) {
  BufferedAssembler a;
  a.add(o0, o0, o1);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  auto f = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
  EXPECT_EQ(f(3, 4), 7u);
  EXPECT_EQ(f(0xffffffffffffffff, 2), 1u);
  EXPECT_EQ(f(0x1234567800000000, 0x76543210), 0x1234567876543210u);
}

TEST(AssemblerSparc64Test, NativePcRelativeLiteral) {
  BufferedAssembler a;
  a.rdpc(o0);
  a.ldx(o0, o0, 16);
  a.ba(16);
  a.nop();
  a.dq(0x123456789abcdef0);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)()>()()), 0x123456789abcdef0u);
}

TEST(AssemblerSparc64Test, NativeLoop) {
  BufferedAssembler a;
  a.mov(o1, 0);
  a.add(o1, o1, o0);
  a.subcc(o0, o0, 1);
  a.b(greater, ConditionCodes::kXcc, -8);
  a.nop();
  a.mov(o0, o1);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)(uint64_t)>()(100)), 5050u);
}

TEST(AssemblerSparc64Test, NativeDirectCall) {
  BufferedAssembler a;
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(o0, i0);
  a.call(20);
  a.nop();
  a.mov(i0, o0);
  a.ret();
  a.restore();
  a.add(o0, o0, 1);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)(uint64_t)>()(0x1234567800000000)),
            0x1234567800000001u);
}

TEST(AssemblerSparc64Test, NativeXccComparison) {
  BufferedAssembler a;
  a.cmp(o0, 0);
  a.mov(o0, 1);
  a.b(not_equal, ConditionCodes::kXcc, 12);
  a.nop();
  a.mov(o0, 0);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  auto f = code.entry<uint64_t (*)(uint64_t)>();
  EXPECT_EQ(f(uint64_t{1} << 32), 1u);
  EXPECT_EQ(f(0), 0u);
}

TEST(AssemblerSparc64Test, NativeDelaySlotAndAnnul) {
  for (bool annul : {false, true}) {
    BufferedAssembler a;
    a.b(always, ConditionCodes::kXcc, 12, annul);
    a.add(o0, o0, 1);
    a.add(o0, o0, 100);
    a.retl();
    a.nop();
    ExecutableCode code;
    ASSERT_TRUE(code.Initialize(a));
    EXPECT_EQ((code.entry<uint64_t (*)(uint64_t)>()(10)), annul ? 10u : 11u);
  }
}

TEST(AssemblerSparc64Test, NativeLoadWidths) {
  const uint64_t value = 0x8899aabbccddeeff;
  BufferedAssembler a;
  a.lduw(o0, o0, 0);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)(const uint64_t*)>()(&value)), 0x8899aabbu);
  BufferedAssembler b;
  b.ldsw(o0, o0, 4);
  b.retl();
  b.nop();
  ExecutableCode signed_code;
  ASSERT_TRUE(signed_code.Initialize(b));
  EXPECT_EQ((signed_code.entry<uint64_t (*)(const uint64_t*)>()(&value)),
            0xffffffffccddeeffu);
}

TEST(AssemblerSparc64Test, NativeDoubleArguments) {
  BufferedAssembler a;
  a.faddd(d0, d0, d1);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_DOUBLE_EQ((code.entry<double (*)(double, double)>()(1.25, 2.5)), 3.75);
}

TEST(AssemblerSparc64Test, NativeUpperDoubleRegisters) {
  BufferedAssembler a;
  a.ldd(d16, o0, 0);
  a.ldd(d31, o0, 8);
  a.fmuld(d31, d16, d31);
  a.std(d31, o0, 16);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  double values[] = {1.25, 2.5, 0};
  code.entry<void (*)(double*)>()(values);
  EXPECT_DOUBLE_EQ(values[2], 3.125);
}

uint64_t WeightedArguments(uint64_t a, uint64_t b, uint64_t c, uint64_t d,
                           uint64_t e, uint64_t f, uint64_t g, uint64_t h) {
  return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 8 * h;
}

TEST(AssemblerSparc64Test, NativeCStackArguments) {
  BufferedAssembler a;
  a.save(sp, sp, -192);
  for (int n = 0; n < 6; ++n) {
    a.mov(Register::from_code(o0.code() + n), n + 1);
  }
  a.mov(g1, 7);
  a.stx(g1, sp, kSparcStackBias + kSparcMinimumFrameSize);
  a.mov(g1, 8);
  a.stx(g1, sp, kSparcStackBias + kSparcMinimumFrameSize + 8);
  SetConstant(a, g1, reinterpret_cast<uintptr_t>(&WeightedArguments));
  a.jmpl(o7, g1, 0);
  a.nop();
  a.mov(i0, o0);
  a.ret();
  a.restore();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)()>()()), 204u);
}

double MixedArguments(uint64_t a, double b, uint64_t c, double d) {
  return a + 2 * b + 3 * c + 4 * d;
}

TEST(AssemblerSparc64Test, NativeMixedCArguments) {
  BufferedAssembler a;
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(o0, i0);
  a.mov(o2, i2);
  SetConstant(a, g1, reinterpret_cast<uintptr_t>(&MixedArguments));
  a.jmpl(o7, g1, 0);
  a.nop();
  a.ret();
  a.restore();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_DOUBLE_EQ(
      (code.entry<double (*)(uint64_t, double, uint64_t, double)>()(1, 2.5, 3,
                                                                    4.25)),
      32.0);
}

using RecursiveEntry = uint64_t (*)(uint64_t);
RecursiveEntry recursive_entry;

V8_NOINLINE uint64_t ReenterGeneratedCode(uint64_t depth) {
  volatile uint64_t anchor = depth ^ 0x123456789abcdef0;
  uint64_t result = depth == 0 ? 0 : recursive_entry(depth - 1) + 1;
  return anchor == (depth ^ 0x123456789abcdef0) ? result : UINT64_MAX;
}

TEST(AssemblerSparc64Test, NativeWindowSpillAndReentry) {
  BufferedAssembler a;
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(o0, i0);
  SetConstant(a, g1, reinterpret_cast<uintptr_t>(&ReenterGeneratedCode));
  a.jmpl(o7, g1, 0);
  a.nop();
  a.mov(i0, o0);
  a.ret();
  a.restore();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  recursive_entry = code.entry<RecursiveEntry>();
  for (uint64_t depth : {0, 1, 7, 8, 16, 64, 256}) {
    EXPECT_EQ(recursive_entry(depth), depth);
  }
  recursive_entry = nullptr;
}

TEST(AssemblerSparc64Test, NativeCompareExchange) {
  BufferedAssembler a;
  a.membar(kSync);
  a.casx(o2, o0, o1);
  a.membar(kSync);
  a.mov(o0, o2);
  a.retl();
  a.nop();
  ExecutableCode code;
  ASSERT_TRUE(code.Initialize(a));
  auto f = code.entry<uint64_t (*)(uint64_t*, uint64_t, uint64_t)>();
  uint64_t value = 0x123456789abcdef0;
  EXPECT_EQ(f(&value, value, 0xfedcba9876543210), 0x123456789abcdef0u);
  EXPECT_EQ(value, 0xfedcba9876543210u);
  EXPECT_EQ(f(&value, 0, 1), 0xfedcba9876543210u);
  EXPECT_EQ(value, 0xfedcba9876543210u);
}

#endif  // V8_HOST_ARCH_SPARC64

}  // namespace
}  // namespace v8::internal
