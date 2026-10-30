// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <sys/mman.h>
#undef MAP_TYPE  // Conflicts with V8's instance type.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "include/v8-function-callback.h"
#include "src/codegen/interface-descriptors-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/codegen/register-configuration.h"
#include "src/codegen/sparc64/assembler-sparc64.h"
#include "src/compiler/backend/sparc64/code-generator-sparc64-inl.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/diagnostics/disasm.h"
#include "src/execution/isolate-data.h"
#include "src/heap/base/stack.h"
#include "src/heap/memory-chunk.h"
#include "src/interpreter/bytecodes.h"
#include "src/numbers/conversions-inl.h"
#include "src/objects/js-function.h"
#include "src/sandbox/js-dispatch-table.h"

namespace heap::base {
extern "C" void PushAllRegistersAndIterateStack(Stack*, void*,
                                                Stack::IterateStackCallback);
}

namespace v8::internal {
namespace {

template <typename Descriptor>
constexpr bool DescriptorRegistersAreValid() {
  constexpr auto regs = Descriptor::registers();
  constexpr auto doubles = Descriptor::double_registers();
  constexpr int count = Descriptor::GetRegisterParameterCount();
  RegList used;
  DoubleRegList used_doubles;
  for (int n = 0; n < count; ++n) {
    Register reg = regs[n];
    DoubleRegister dreg = doubles[n];
    if (!reg.is_valid() && !dreg.is_valid()) return false;
    if (reg.is_valid()) {
      if (reg == g0 || reg == g6 || reg == g7 || reg == sp || reg == fp ||
          reg == kNativeFramePointer || reg == o7 || reg == i7 ||
          reg == kRootRegister || used.has(reg)) {
        return false;
      }
      used.set(reg);
    }
    if (dreg.is_valid()) {
      if (used_doubles.has(dreg)) return false;
      used_doubles.set(dreg);
    }
  }
  return true;
}

TEST(AssemblerSparc64BufferTest, BuiltinDescriptorRegisters) {
#define CHECK_DESCRIPTOR(name, ...) \
  EXPECT_TRUE(DescriptorRegistersAreValid<name##Descriptor>()) << #name;
  INTERFACE_DESCRIPTOR_LIST(CHECK_DESCRIPTOR)
#undef CHECK_DESCRIPTOR
}

class ExecutableAssemblerBuffer {
 public:
  ~ExecutableAssemblerBuffer() {
    if (memory_ != MAP_FAILED) munmap(memory_, size_);
  }

  bool Initialize(const Assembler& assembler) {
    const size_t page_size = sysconf(_SC_PAGESIZE);
    size_ = (assembler.pc_offset() + page_size - 1) & ~(page_size - 1);
    memory_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_ == MAP_FAILED) return false;
    memcpy(memory_, assembler.buffer_start(), assembler.pc_offset());
    if (mprotect(memory_, size_, PROT_READ | PROT_EXEC) != 0) return false;
    auto* start = static_cast<char*>(memory_);
    __builtin___clear_cache(start, start + assembler.pc_offset());
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

TEST(AssemblerSparc64BufferTest, LabelAtOffsetZero) {
  Assembler a(AssemblerOptions{});
  Label start;
  a.bind(&start);
  a.ba(&start);
  a.nop();
  EXPECT_TRUE(start.is_bound());
  EXPECT_EQ(start.pos(), 0);
  EXPECT_EQ(a.instr_at(0), 0x10800000u);
}

TEST(AssemblerSparc64BufferTest, MultipleForwardReferences) {
  Assembler a(AssemblerOptions{});
  Label target;
  a.b(equal, ConditionCodes::kXcc, &target);
  a.nop();
  a.ba(&target);
  a.nop();
  a.call(&target);
  a.nop();
  a.bind(&target);
  EXPECT_EQ(a.instr_at(0), 0x02680006u);
  EXPECT_EQ(a.instr_at(8), 0x10800004u);
  EXPECT_EQ(a.instr_at(16), 0x40000002u);
}

TEST(AssemblerSparc64BufferTest, MovedLabel) {
  Assembler a(AssemblerOptions{});
  Label original;
  a.ba(&original);
  a.nop();
  Label target = std::move(original);
  a.bind(&target);
  EXPECT_EQ(a.instr_at(0), 0x10800002u);
}

TEST(AssemblerSparc64BufferTest, LabelAcrossBufferGrowth) {
  Assembler a(AssemblerOptions{}, NewAssemblerBuffer(128));
  const int original_size = a.buffer_size();
  Label target;
  a.ba(&target);
  a.nop();
  for (int n = 0; n < 4096; ++n) a.nop();
  a.bind(&target);
  EXPECT_GT(a.buffer_size(), original_size);
  EXPECT_EQ(a.instr_at(0) & 0x3fffff,
            static_cast<Instr>(target.pos() / kInstrSize));
}

TEST(AssemblerSparc64BufferTest, NativeLoopWithLabels) {
  Assembler a(AssemblerOptions{});
  Label loop;
  a.mov(o1, 0);
  a.bind(&loop);
  a.add(o1, o1, o0);
  a.subcc(o0, o0, 1);
  a.b(greater, ConditionCodes::kXcc, &loop);
  a.nop();
  a.mov(o0, o1);
  a.retl();
  a.nop();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)(uint64_t)>()(100)), 5050u);
}

TEST(AssemblerSparc64BufferTest, NativeCallWithForwardLabel) {
  Assembler a(AssemblerOptions{});
  Label target;
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(o0, i0);
  a.call(&target);
  a.nop();
  a.mov(i0, o0);
  a.ret();
  a.restore();
  a.bind(&target);
  a.add(o0, o0, 1);
  a.retl();
  a.nop();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)(uint64_t)>()(0x1234567800000000)),
            0x1234567800000001u);
}

TEST(AssemblerSparc64BufferTest, NativeAlignedLiteral) {
  Assembler a(AssemblerOptions{});
  a.nop();
  a.LoadLiteral(o0, 0x123456789abcdef0);
  a.retl();
  a.nop();
  EXPECT_EQ(a.instr_at(4), 0x01000000u);
  Address literal = reinterpret_cast<Address>(a.buffer_start()) + 24;
  EXPECT_EQ(literal % sizeof(Address), 0u);
  EXPECT_EQ(Assembler::target_address_at(literal, kNullAddress),
            0x123456789abcdef0u);
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)()>()()), 0x123456789abcdef0u);
}

TEST(AssemblerSparc64BufferTest, NativeLiteralAcrossBufferGrowth) {
  Assembler a(AssemblerOptions{}, NewAssemblerBuffer(128));
  const int original_size = a.buffer_size();
  a.LoadLiteral(o0, 0xfedcba9876543210);
  a.retl();
  a.nop();
  for (int n = 0; n < 4096; ++n) a.nop();
  EXPECT_GT(a.buffer_size(), original_size);
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)()>()()), 0xfedcba9876543210u);
}

TEST(AssemblerSparc64BufferTest, NativeLiteralPatch) {
  Assembler a(AssemblerOptions{});
  a.LoadLiteral(o0, 0x123456789abcdef0);
  a.retl();
  a.nop();
  uint8_t instructions[16];
  memcpy(instructions, a.buffer_start(), sizeof(instructions));
  Address literal = reinterpret_cast<Address>(a.buffer_start()) + 16;
  Assembler::set_target_address_at(literal, kNullAddress, 0xfedcba9876543210,
                                   nullptr, SKIP_ICACHE_FLUSH);
  EXPECT_EQ(memcmp(instructions, a.buffer_start(), sizeof(instructions)), 0);
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ((code.entry<uint64_t (*)()>()()), 0xfedcba9876543210u);
}

size_t maximum_flat_frame_depth;

uint64_t ProbeFlatFrameChain(const uintptr_t* frame) {
  asm volatile("flushw" ::: "memory");
  size_t depth = 0;
  while (frame != nullptr) {
    if ((reinterpret_cast<uintptr_t>(frame) & 15) != 0) return 0;
    if ((frame[1] & 3) != 0 || frame[1] == 0) return 0;
    if (frame[-1] != StackFrame::TypeToMarker(StackFrame::INTERNAL)) return 0;
    const auto* caller = reinterpret_cast<const uintptr_t*>(frame[0]);
    if (caller != nullptr && reinterpret_cast<uintptr_t>(caller) !=
                                 reinterpret_cast<uintptr_t>(frame) + 32) {
      return 0;
    }
    frame = caller;
    if (++depth > 4096) return 0;
  }
  maximum_flat_frame_depth = std::max(maximum_flat_frame_depth, depth);
  return 1;
}

TEST(AssemblerSparc64BufferTest, NativeFlatFramesAcrossWindowFlush) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  Label function, base_case, finish, failure, epilogue;
  constexpr Register virtual_fp = fp;

  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(virtual_fp, 0);
  a.mov(l3, i6);
  a.mov(o0, i0);
  a.call(&function);
  a.nop();
  a.mov(i0, o0);
  a.ret();
  a.restore();

  a.bind(&function);
  a.EnterFrame(StackFrame::INTERNAL);
  a.set_has_frame(true);
  a.AllocateStackSpace(kSystemPointerSize);
  a.cmp(o0, 0);
  a.b(equal, ConditionCodes::kXcc, &base_case);
  a.nop();
  a.sub(o0, o0, 1);
  a.call(&function);
  a.nop();
  a.add(o0, o0, 1);
  a.ba(&finish);
  a.nop();

  a.bind(&base_case);
  a.mov(o0, 0);
  a.bind(&finish);
  a.stx(o0, virtual_fp, -16);
  a.cmp(i6, l3);
  a.b(not_equal, ConditionCodes::kXcc, &failure);
  a.nop();
  a.mov(o0, virtual_fp);
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeFlatFrameChain));
  a.CallCFunction(g1, 1, SetIsolateDataSlots::kNo);
  a.cmp(o0, 1);
  a.b(not_equal, ConditionCodes::kXcc, &failure);
  a.nop();
  a.ldx(o0, virtual_fp, -16);
  a.ba(&epilogue);
  a.nop();

  a.bind(&failure);
  a.LoadLiteral(o0, UINT64_MAX);
  a.bind(&epilogue);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.Ret();

  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  auto function_entry = code.entry<uint64_t (*)(uint64_t)>();
  for (uint64_t depth : {0, 1, 8, 64, 256}) {
    maximum_flat_frame_depth = 0;
    EXPECT_EQ(function_entry(depth), depth);
    EXPECT_EQ(maximum_flat_frame_depth, depth + 1);
  }
}

TEST(AssemblerSparc64BufferTest, NativeMacroStackOperations) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.EnterFrame(StackFrame::INTERNAL);
  a.Move(o0, intptr_t{0x123456789abcdef0});
  a.Push(o0, i0);
  a.Pop(o1, o2);
  a.AllocateStackSpace(8192);
  a.Move(o3, sp);
  a.Move(sp, o3);
  a.StoreU64(o1, MemOperand(o3));
  a.StoreU64(o2, MemOperand(sp, 8184));
  a.LoadU64(o0, MemOperand(sp));
  a.LoadU64(o1, MemOperand(sp, 8184));
  a.add(i0, o0, o1);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t)>()(7), 0x123456789abcdef7u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroLargeAddressAliases) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.mov(g2, o0);
  a.Move(g1, intptr_t{0x123456789abcdef0});
  a.StoreU64(g1, MemOperand(g2, 8192));
  a.LoadU64(g2, MemOperand(g2, 8192));
  a.AddS64(g2, g2, 0x10000);
  a.SubS64(o0, g2, 0x10001);
  a.Ret();
  std::vector<uint64_t> data(1025);
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t*)>()(data.data()),
            0x123456789abcdeefu);
  EXPECT_EQ(data[1024], 0x123456789abcdef0u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroRootAccess) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(kRootRegister, i0);
  a.LoadRootRelative(o0, -8192);
  a.StoreRootRelative(8192, o0);
  a.LoadRoot(o1, RootIndex::kUndefinedValue);
  a.add(i0, o0, o1);
  a.ret();
  a.restore();
  std::vector<uint64_t> roots(8192);
  auto* root = roots.data() + 4096;
  root[-1024] = 17;
  const int offset = IsolateData::root_slot_offset(RootIndex::kUndefinedValue);
  ASSERT_GE(offset, -4096 * kSystemPointerSize);
  ASSERT_LT(offset, 4096 * kSystemPointerSize);
  root[offset / kSystemPointerSize] = 25;
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t*)>()(root), 42u);
  EXPECT_EQ(root[1024], 17u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroStandardFrame) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.mov(kJavaScriptCallArgCountRegister, i0);
  a.mov(kContextRegister, i1);
  a.mov(kJavaScriptCallTargetRegister, i2);
  a.PushStandardFrame(kJavaScriptCallTargetRegister);
  a.LoadU64(o0, MemOperand(fp, StandardFrameConstants::kArgCOffset));
  a.LoadU64(o1, MemOperand(fp, StandardFrameConstants::kContextOffset));
  a.LoadU64(o2, MemOperand(fp, StandardFrameConstants::kFunctionOffset));
  a.add(i0, o0, o1);
  a.add(i0, i0, o2);
  a.LeaveFrame(StackFrame::INTERPRETED);
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(
      (code.entry<uint64_t (*)(uint64_t, uint64_t, uint64_t)>()(7, 11, 13)),
      31u);
}

uintptr_t expected_fast_call_pc;

struct NativeResultPair {
  uint64_t first;
  uint64_t second;
};

NativeResultPair MakeNativeResultPair(uint64_t value) {
  return {value, value + 1};
}

TEST(AssemblerSparc64BufferTest, NativeMacroCFunctionReturnsPair) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.EnterFrame(StackFrame::INTERNAL);
  a.set_has_frame(true);
  a.mov(o0, i0);
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&MakeNativeResultPair));
  a.CallCFunction(g1, 1, SetIsolateDataSlots::kNo);
  a.add(i0, o0, o1);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t)>()(21), 43u);
}

uint64_t ProbeFastCCall(uint64_t* roots, uintptr_t caller_fp, uint64_t a,
                        uint64_t b, uint64_t c, uint64_t d) {
  asm volatile("flushw" ::: "memory");
  uintptr_t native_sp;
  asm volatile("mov %%sp, %0" : "=r"(native_sp));
  if (((native_sp + kSparcStackBias) & 15) != 0) return UINT64_MAX;
  auto* root_bytes = reinterpret_cast<uint8_t*>(roots);
  auto read_root = [&](int offset) {
    return *reinterpret_cast<uintptr_t*>(root_bytes + offset);
  };
  if (read_root(IsolateData::fast_c_call_caller_fp_offset()) != caller_fp) {
    return UINT64_MAX;
  }
  if (read_root(IsolateData::fast_c_call_caller_pc_offset()) !=
      expected_fast_call_pc) {
    return UINT64_MAX;
  }
  auto* frame = reinterpret_cast<uintptr_t*>(caller_fp);
  if (frame[-1] != StackFrame::TypeToMarker(StackFrame::INTERNAL)) {
    return UINT64_MAX;
  }
  return a + b + c + d;
}

TEST(AssemblerSparc64BufferTest, NativeMacroCFunctionBridge) {
  for (int padding : {0, kSystemPointerSize}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label failure, finish;
    a.save(sp, sp, -kSparcMinimumFrameSize);
    a.mov(fp, 0);
    a.mov(kRootRegister, i0);
    a.EnterFrame(StackFrame::INTERNAL);
    a.set_has_frame(true);
    a.AllocateStackSpace(padding);
    a.mov(l2, sp);
    a.mov(l3, kNativeFramePointer);
    a.mov(o0, i0);
    a.mov(o1, fp);
    a.Move(o2, 3);
    a.Move(o3, 5);
    a.Move(o4, 7);
    a.Move(o5, 11);
    a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeFastCCall));
    const int return_offset = a.CallCFunction(g1, 6);
    a.mov(i0, o0);
    a.cmp(sp, l2);
    a.b(not_equal, ConditionCodes::kXcc, &failure);
    a.nop();
    a.cmp(kNativeFramePointer, l3);
    a.b(not_equal, ConditionCodes::kXcc, &failure);
    a.nop();
    a.Jump(&finish);
    a.bind(&failure);
    a.Move(i0, -1);
    a.bind(&finish);
    a.LeaveFrame(StackFrame::INTERNAL);
    a.ret();
    a.restore();
    std::vector<uint64_t> roots(8192);
    uint64_t* root = roots.data() + 4096;
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint64_t (*)(uint64_t*)>();
    expected_fast_call_pc = reinterpret_cast<uintptr_t>(entry) + return_offset;
    EXPECT_EQ(entry(root), 26u) << "padding=" << padding;
    const int fp_offset = IsolateData::fast_c_call_caller_fp_offset();
    EXPECT_EQ(root[fp_offset / kSystemPointerSize], 0u);
    const int pc_offset = IsolateData::fast_c_call_caller_pc_offset();
    EXPECT_EQ(root[pc_offset / kSystemPointerSize], expected_fast_call_pc);
  }
}

uint64_t ProbeStackCArguments(uint64_t a0, uint64_t a1, uint64_t a2,
                              uint64_t a3, uint64_t a4, uint64_t a5,
                              uint64_t a6, uint64_t a7) {
  asm volatile("flushw" ::: "memory");
  return a0 + 2 * a1 + 3 * a2 + 4 * a3 + 5 * a4 + 6 * a5 + 7 * a6 + 8 * a7;
}

TEST(AssemblerSparc64BufferTest, NativeMacroCStackArguments) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  Label failure, finish;
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.EnterFrame(StackFrame::INTERNAL);
  a.set_has_frame(true);
  a.mov(l2, sp);
  a.Move(l3, 333);
  a.Move(l4, 444);
  a.PrepareCallCFunction(8, g1);
  for (int n = 0; n < 6; ++n) a.Move(kCArgRegs[n], n + 1);
  a.Move(g2, 7);
  a.StoreU64(g2, MemOperand(sp));
  a.Move(g2, 8);
  a.StoreU64(g2, MemOperand(sp, kSystemPointerSize));
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeStackCArguments));
  a.CallCFunction(g1, 8, SetIsolateDataSlots::kNo);
  a.mov(i0, o0);
  a.cmp(sp, l2);
  a.b(not_equal, ConditionCodes::kXcc, &failure);
  a.nop();
  a.cmp(l3, 333);
  a.b(not_equal, ConditionCodes::kXcc, &failure);
  a.nop();
  a.cmp(l4, 444);
  a.b(not_equal, ConditionCodes::kXcc, &failure);
  a.nop();
  a.Jump(&finish);
  a.bind(&failure);
  a.Move(i0, -1);
  a.bind(&finish);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)()>()(), 204u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroCMaximumArguments) {
  MacroAssembler callee(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                        CodeObjectRequired::kNo);
  callee.flushw();
  callee.LoadU64(o0, MemOperand(sp, 249 * kSystemPointerSize));
  callee.Ret();
  ExecutableAssemblerBuffer callee_code;
  ASSERT_TRUE(callee_code.Initialize(callee));
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.EnterFrame(StackFrame::INTERNAL);
  a.set_has_frame(true);
  a.PrepareCallCFunction(kMaxCParameters, g1);
  for (int n = 0; n < kMaxCParameters - kRegisterPassedArguments; ++n) {
    a.Move(g2, n + kRegisterPassedArguments + 1);
    a.StoreU64(g2, MemOperand(sp, n * kSystemPointerSize));
  }
  a.LoadLiteral(g1,
                reinterpret_cast<uintptr_t>(callee_code.entry<void (*)()>()));
  a.CallCFunction(g1, kMaxCParameters, SetIsolateDataSlots::kNo);
  a.mov(i0, o0);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)()>()(), 256u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroSignedWord32Branches) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  Label equal_case, less_case;
  a.JumpIfEqual(o0, 0x12345678, &equal_case);
  a.JumpIfLessThan(o0, 0x12345678, &less_case);
  a.Move(o0, 3);
  a.Ret();
  a.bind(&equal_case);
  a.Move(o0, 2);
  a.Ret();
  a.bind(&less_case);
  a.Move(o0, 1);
  a.Ret();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  auto entry = code.entry<uint64_t (*)(uint64_t)>();
  EXPECT_EQ(entry(0xfeedface12345678), 2u);
  EXPECT_EQ(entry(0x0000000080000000), 1u);
  EXPECT_EQ(entry(0xffffffffffffffff), 1u);
  EXPECT_EQ(entry(0xffffffff7fffffff), 3u);
}

TEST(AssemblerSparc64BufferTest, NativeMacroBuiltinCalls) {
  Assembler callee(AssemblerOptions{});
  callee.add(o0, o0, 1);
  callee.retl();
  callee.nop();
  ExecutableAssemblerBuffer callee_code;
  ASSERT_TRUE(callee_code.Initialize(callee));
  for (bool tail_call : {false, true}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label function;
    a.save(sp, sp, -kSparcMinimumFrameSize);
    a.mov(fp, 0);
    a.mov(kRootRegister, i0);
    a.mov(o0, i1);
    a.call(&function);
    a.nop();
    a.mov(i0, o0);
    a.ret();
    a.restore();
    a.bind(&function);
    a.EnterFrame(StackFrame::INTERNAL);
    if (tail_call) {
      a.LeaveFrame(StackFrame::INTERNAL);
      a.TailCallBuiltin(Builtin::kAbort);
    } else {
      a.CallBuiltin(Builtin::kAbort);
      a.LeaveFrame(StackFrame::INTERNAL);
      a.Ret();
    }
    std::vector<uint64_t> roots(8192);
    uint64_t* root = roots.data() + 4096;
    const int offset = IsolateData::BuiltinEntrySlotOffset(Builtin::kAbort);
    ASSERT_GE(offset, -4096 * kSystemPointerSize);
    ASSERT_LT(offset, 4096 * kSystemPointerSize);
    root[offset / kSystemPointerSize] =
        reinterpret_cast<uintptr_t>(callee_code.entry<void (*)()>());
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EXPECT_EQ((code.entry<uint64_t (*)(uint64_t*, uint64_t)>()(root, 41)), 42u);
  }
}

TEST(AssemblerSparc64BufferTest, NativeCWindowRootRegister) {
  MacroAssembler callee(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                        CodeObjectRequired::kNo);
  callee.LoadRootRelative(o0, -8192);
  callee.Ret();
  ExecutableAssemblerBuffer callee_code;
  ASSERT_TRUE(callee_code.Initialize(callee));
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(fp, 0);
  a.mov(kRootRegister, i0);
  a.EnterFrame(StackFrame::INTERNAL);
  a.set_has_frame(true);
  a.LoadLiteral(g1,
                reinterpret_cast<uintptr_t>(callee_code.entry<void (*)()>()));
  a.CallCFunction(g1, 0, SetIsolateDataSlots::kNo);
  a.mov(i0, o0);
  a.LeaveFrame(StackFrame::INTERNAL);
  a.ret();
  a.restore();
  std::vector<uint64_t> roots(8192);
  auto* root = roots.data() + 4096;
  root[-1024] = 42;
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t*)>()(root), 42u);
}

TEST(AssemblerSparc64BufferTest, NativeDoubleUnaryOperations) {
  Assembler a(AssemblerOptions{});
  a.fmovd(d16, d0);
  a.fnegd(d31, d16);
  a.fabsd(d0, d31);
  a.fsqrtd(d0, d0);
  a.retl();
  a.nop();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  auto entry = code.entry<double (*)(double)>();
  EXPECT_DOUBLE_EQ(entry(2.25), 1.5);
  EXPECT_DOUBLE_EQ(entry(-9.0), 3.0);
  EXPECT_EQ(entry(std::numeric_limits<double>::infinity()),
            std::numeric_limits<double>::infinity());
  EXPECT_TRUE(std::isnan(entry(std::numeric_limits<double>::quiet_NaN())));
  const double zero = entry(-0.0);
  EXPECT_EQ(zero, 0.0);
  EXPECT_FALSE(std::signbit(zero));
}

TEST(AssemblerSparc64BufferTest, NativeFloatBranchPredicates) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();
  const std::pair<double, double> pairs[] = {
      {-1.0, 1.0}, {1.0, -1.0}, {1.0, 1.0}, {-0.0, 0.0}, {nan, 1.0},
      {1.0, nan},  {nan, nan},  {inf, inf}, {inf, -inf}};
  for (int fcc = 0; fcc < 4; ++fcc) {
    for (int condition = 0; condition < 16; ++condition) {
      Assembler a(AssemblerOptions{});
      Label taken;
      a.fcmpd(d0, d1, fcc);
      a.fb(static_cast<FloatCondition>(condition), fcc, &taken);
      a.nop();
      a.mov(o0, 0);
      a.retl();
      a.nop();
      a.bind(&taken);
      a.mov(o0, 1);
      a.retl();
      a.nop();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(double, double)>();
      for (const auto& [left, right] : pairs) {
        const bool unordered = std::isnan(left) || std::isnan(right);
        const bool less = left < right;
        const bool greater = left > right;
        const bool equal = left == right;
        const bool expected[] = {false,
                                 !equal,
                                 less || greater,
                                 unordered || less,
                                 less,
                                 unordered || greater,
                                 greater,
                                 unordered,
                                 true,
                                 equal,
                                 unordered || equal,
                                 greater || equal,
                                 unordered || greater || equal,
                                 less || equal,
                                 unordered || less || equal,
                                 !unordered};
        EXPECT_EQ(entry(left, right),
                  static_cast<uint64_t>(expected[condition]))
            << "fcc=" << fcc << " condition=" << static_cast<int>(condition);
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeUnalignedIntegerLoads) {
  std::array<uint8_t, 32> data;
  for (size_t n = 0; n < data.size(); ++n) data[n] = 0x80 + n;
  for (int bytes : {1, 2, 4, 8}) {
    for (bool is_signed : {false, true}) {
      for (int offset = 0; offset < 8; ++offset) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        a.LoadUnaligned(o0, MemOperand(o0, offset), bytes, is_signed);
        a.Ret();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        uint64_t expected = 0;
        for (int n = 0; n < bytes; ++n) {
          expected = (expected << 8) | data[offset + n];
        }
        if (is_signed && bytes < 8) expected |= UINT64_MAX << (bytes * 8);
        EXPECT_EQ(code.entry<uint64_t (*)(const uint8_t*)>()(data.data()),
                  expected)
            << "bytes=" << bytes << " offset=" << offset
            << " signed=" << is_signed;
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeUnalignedIntegerStores) {
  const uint64_t value = 0xfedcba9876543210;
  for (int bytes : {1, 2, 4, 8}) {
    for (int offset = 0; offset < 8; ++offset) {
      std::array<uint8_t, 32> data;
      data.fill(0xaa);
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      a.StoreUnaligned(o1, MemOperand(o0, offset), bytes);
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      code.entry<void (*)(uint8_t*, uint64_t)>()(data.data(), value);
      for (int n = 0; n < static_cast<int>(data.size()); ++n) {
        const uint8_t expected =
            n >= offset && n < offset + bytes
                ? static_cast<uint8_t>(value >> ((offset + bytes - n - 1) * 8))
                : 0xaa;
        EXPECT_EQ(data[n], expected)
            << "bytes=" << bytes << " offset=" << offset;
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeUnalignedLargeOffsetAndAliases) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.mov(g2, o1);
  a.StoreUnaligned(g2, MemOperand(o0, 8193), 8);
  a.LoadUnaligned(o0, MemOperand(o0, 8193), 8);
  a.Ret();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  std::vector<uint8_t> data(8208, 0xaa);
  const uint64_t value = 0xfedcba9876543210;
  EXPECT_EQ(
      (code.entry<uint64_t (*)(uint8_t*, uint64_t)>()(data.data(), value)),
      value);
  EXPECT_EQ(data[8192], 0xaa);
  EXPECT_EQ(data[8201], 0xaa);
}

TEST(AssemblerSparc64BufferTest, NativeFloat32BitMoves) {
  const uint32_t patterns[] = {0,          0x80000000, 0x3f800000, 0xbf800000,
                               1,          0x007fffff, 0x7f800000, 0xff800000,
                               0x7fc12345, 0x7f812345, 0xff812345, 0xffffffff};
  for (int reg = 0; reg < 16; ++reg) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    FloatRegister dst = FloatRegister::from_code(reg);
    a.MovIntToFloat(dst, o0);
    a.fmovs(d15, dst);
    a.MovFloatToInt(o0, d15);
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint64_t (*)(uint64_t)>();
    for (uint32_t bits : patterns) {
      EXPECT_EQ(entry(UINT64_C(0xdeadbeef00000000) | bits), bits) << reg;
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeDoubleBitMoves) {
  const uint64_t patterns[] = {0,
                               UINT64_C(0x8000000000000000),
                               UINT64_C(0x3ff0000000000000),
                               UINT64_C(0xbff0000000000000),
                               1,
                               UINT64_C(0x000fffffffffffff),
                               UINT64_C(0x7ff0000000000000),
                               UINT64_C(0xfff0000000000000),
                               UINT64_C(0x7ff812345678abcd),
                               UINT64_C(0x7ff012345678abcd),
                               UINT64_C(0xfff012345678abcd),
                               UINT64_MAX};
  for (int reg = 0; reg < 32; ++reg) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    DoubleRegister dst = DoubleRegister::from_code(reg);
    a.MovInt64ToDouble(dst, o0);
    a.fmovd(d31, dst);
    a.MovDoubleToInt64(o0, d31);
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    for (uint64_t bits : patterns) {
      EXPECT_EQ(code.entry<uint64_t (*)(uint64_t)>()(bits), bits) << reg;
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeFloatMemoryBoundaries) {
  for (int bytes : {4, 8}) {
    for (int offset : {-8192, -4096, 0, 4096, 8192}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      if (bytes == 4) {
        a.LoadF32(d14, MemOperand(o0, offset));
        a.fmovs(d15, d14);
        a.StoreF32(d15, MemOperand(o1, offset));
      } else {
        a.LoadF64(d31, MemOperand(o0, offset));
        a.fmovd(d15, d31);
        a.StoreF64(d15, MemOperand(o1, offset));
      }
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      std::vector<uint64_t> input(2050, UINT64_C(0x7ff01234ff812345));
      std::vector<uint64_t> output(2050, UINT64_C(0xaaaaaaaaaaaaaaaa));
      code.entry<void (*)(uint64_t*, uint64_t*)>()(input.data() + 1024,
                                                   output.data() + 1024);
      auto* raw = reinterpret_cast<uint8_t*>(output.data());
      auto* expected = reinterpret_cast<uint8_t*>(input.data());
      const int start = 8192 + offset;
      for (int n = 0; n < static_cast<int>(output.size() * 8); ++n) {
        EXPECT_EQ(raw[n], n >= start && n < start + bytes ? expected[n] : 0xaa)
            << "offset=" << offset << " bytes=" << bytes << " n=" << n;
      }
    }
  }
}

void ReturnBranchResult(MacroAssembler* a, compiler::ArchOpcode opcode,
                        compiler::FlagsCondition condition) {
  Label taken;
  compiler::EmitSparc64Branch(a, opcode, condition, &taken);
  a->mov(o0, 0);
  a->Ret();
  a->bind(&taken);
  a->mov(o0, 1);
  a->Ret();
}

TEST(AssemblerSparc64BufferTest, NativeCompilerIntegerConditions) {
  using namespace compiler;  // NOLINT(build/namespaces)
  const FlagsCondition conditions[] = {kEqual,
                                       kNotEqual,
                                       kSignedLessThan,
                                       kSignedGreaterThanOrEqual,
                                       kSignedLessThanOrEqual,
                                       kSignedGreaterThan,
                                       kUnsignedLessThan,
                                       kUnsignedGreaterThanOrEqual,
                                       kUnsignedLessThanOrEqual,
                                       kUnsignedGreaterThan};
  const uint64_t values[] = {0,
                             1,
                             UINT64_MAX,
                             0x7fffffff,
                             0x80000000,
                             UINT64_C(0x7fffffffffffffff),
                             UINT64_C(0x8000000000000000),
                             UINT64_C(0xfedcba9876543210)};
  for (bool word32 : {false, true}) {
    for (FlagsCondition condition : conditions) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      a.cmp(o0, o1);
      ReturnBranchResult(&a, word32 ? kSparcCmp32 : kSparcCmp64, condition);
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
      for (uint64_t lhs : values) {
        for (uint64_t rhs : values) {
          uint64_t left = word32 ? static_cast<uint32_t>(lhs) : lhs;
          uint64_t right = word32 ? static_cast<uint32_t>(rhs) : rhs;
          int64_t signed_left =
              word32 ? static_cast<int32_t>(left) : static_cast<int64_t>(left);
          int64_t signed_right = word32 ? static_cast<int32_t>(right)
                                        : static_cast<int64_t>(right);
          const bool expected[] = {
              left == right, left != right,
              signed_left<signed_right, signed_left >= signed_right,
                          signed_left <= signed_right, signed_left>
                  signed_right,
              left<right, left >= right, left <= right, left> right};
          EXPECT_EQ(entry(lhs, rhs), static_cast<uint64_t>(expected[condition]))
              << "word32=" << word32
              << " condition=" << static_cast<int>(condition);
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerFloatMinMax) {
  std::vector<uint64_t> values = {0,
                                  1,
                                  UINT64_MAX,
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0x7ff0000000000000),
                                  UINT64_C(0xfff0000000000000),
                                  UINT64_C(0x7ff0000000000001),
                                  UINT64_C(0x7fefffffffffffff),
                                  UINT64_C(0x3ff0000000000000),
                                  UINT64_C(0xbff0000000000000),
                                  0x80000000,
                                  0x7f800000,
                                  0xff800000,
                                  0x7f800001,
                                  0x7fc00000,
                                  0x7f7fffff,
                                  0x3f800000,
                                  0xbf800000};
  uint64_t random = 97531;
  for (int n = 0; n < 128; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool single : {false, true}) {
    for (bool minimum : {false, true}) {
      for (DoubleRegister dst : {d0, d1, d15, d31}) {
        if (single && dst == d31) continue;
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        a.EnterCFunctionFrame();
        if (single) {
          a.MovIntToFloat(d0, o0);
          a.MovIntToFloat(d1, o1);
        } else {
          a.MovInt64ToDouble(d0, o0);
          a.MovInt64ToDouble(d1, o1);
        }
        compiler::EmitSparc64FloatMinMax(&a, dst, d0, d1, single, minimum);
        if (single)
          a.MovFloatToInt(o0, dst);
        else
          a.MovDoubleToInt64(o0, dst);
        a.LeaveFrame(StackFrame::MANUAL);
        a.RetCFunction();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
        for (uint64_t left : values) {
          for (uint64_t right : values) {
            double lhs = single ? static_cast<double>(base::bit_cast<float>(
                                      static_cast<uint32_t>(left)))
                                : base::bit_cast<double>(left);
            double rhs = single ? static_cast<double>(base::bit_cast<float>(
                                      static_cast<uint32_t>(right)))
                                : base::bit_cast<double>(right);
            uint64_t actual = entry(left, right);
            if (std::isnan(lhs) || std::isnan(rhs)) {
              double result = single
                                  ? static_cast<double>(base::bit_cast<float>(
                                        static_cast<uint32_t>(actual)))
                                  : base::bit_cast<double>(actual);
              EXPECT_TRUE(std::isnan(result));
            } else {
              bool select_left = lhs == rhs ? (minimum ? std::signbit(lhs)
                                                       : !std::signbit(lhs))
                                            : (minimum ? lhs < rhs : lhs > rhs);
              uint64_t expected = select_left ? left : right;
              if (single) expected = static_cast<uint32_t>(expected);
              EXPECT_EQ(actual, expected)
                  << "left=" << left << " right=" << right
                  << " single=" << single << " minimum=" << minimum;
            }
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerAtomicOperations) {
  using compiler::Sparc64AtomicOp;
  for (int bytes : {1, 2, 4, 8}) {
    int bits = bytes * 8;
    uint64_t mask = bytes == 8 ? UINT64_MAX : (uint64_t{1} << bits) - 1;
    for (bool is_signed : {false, true}) {
      for (auto op :
           {Sparc64AtomicOp::kExchange, Sparc64AtomicOp::kCompareExchange,
            Sparc64AtomicOp::kAdd, Sparc64AtomicOp::kSub, Sparc64AtomicOp::kAnd,
            Sparc64AtomicOp::kOr, Sparc64AtomicOp::kXor}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        compiler::EmitSparc64Atomic(&a, o0, o0, o1, o2, o3, o4, o5, bytes,
                                    is_signed, op);
        a.Ret();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        auto entry =
            code.entry<uint64_t (*)(uint64_t*, uint64_t, uint64_t, uint64_t)>();
        for (int offset = 0; offset <= 8 - bytes; offset += bytes) {
          int shift = (8 - bytes - offset) * 8;
          for (uint64_t initial :
               {uint64_t{0}, UINT64_MAX, UINT64_C(0x0123456789abcdef),
                UINT64_C(0x8000800080008000)}) {
            uint64_t old = (initial >> shift) & mask;
            for (uint64_t value : {uint64_t{0}, uint64_t{1}, UINT64_MAX,
                                   UINT64_C(0x123456789abcdef0)}) {
              for (bool match : {false, true}) {
                std::array<uint64_t, 3> words = {123, initial, 456};
                uint64_t expected = (old + !match) & mask;
                uint64_t updated;
                switch (op) {
                  case Sparc64AtomicOp::kExchange:
                    updated = value;
                    break;
                  case Sparc64AtomicOp::kCompareExchange:
                    updated = match ? value : old;
                    break;
                  case Sparc64AtomicOp::kAdd:
                    updated = old + value;
                    break;
                  case Sparc64AtomicOp::kSub:
                    updated = old - value;
                    break;
                  case Sparc64AtomicOp::kAnd:
                    updated = old & value;
                    break;
                  case Sparc64AtomicOp::kOr:
                    updated = old | value;
                    break;
                  case Sparc64AtomicOp::kXor:
                    updated = old ^ value;
                    break;
                }
                uint64_t returned = old;
                if (is_signed && bytes != 8 &&
                    (old & (uint64_t{1} << (bits - 1))))
                  returned |= ~mask;
                EXPECT_EQ(entry(&words[1], offset, value, expected), returned);
                EXPECT_EQ(words[1], (initial & ~(mask << shift)) |
                                        ((updated & mask) << shift));
                EXPECT_EQ(words[0], 123u);
                EXPECT_EQ(words[2], 456u);
              }
            }
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerAtomicContention) {
  constexpr int thread_count = 8;
  constexpr int iterations = 2001;
  for (int bytes : {1, 2, 4, 8}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    compiler::EmitSparc64Atomic(&a, o0, o0, o1, o2, o3, o4, o5, bytes, false,
                                compiler::Sparc64AtomicOp::kAdd);
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint64_t (*)(uint64_t*, uint64_t, uint64_t)>();
    for (bool separate_lanes : {false, true}) {
      std::array<uint64_t, 3> words = {123, 0, 456};
      std::atomic<bool> start{false};
      std::vector<std::thread> threads;
      std::array<std::vector<uint64_t>, thread_count> returned;
      for (int tid = 0; tid < thread_count; ++tid) {
        threads.emplace_back([&, tid] {
          int offset = separate_lanes ? (tid % (8 / bytes)) * bytes : 0;
          returned[tid].reserve(iterations);
          while (!start.load(std::memory_order_acquire)) {
          }
          for (int n = 0; n < iterations; ++n)
            returned[tid].push_back(entry(&words[1], offset, 1));
        });
      }
      start.store(true, std::memory_order_release);
      for (auto& thread : threads) thread.join();
      uint64_t expected_word = 0;
      uint64_t mask =
          bytes == 8 ? UINT64_MAX : (uint64_t{1} << (bytes * 8)) - 1;
      for (int offset = 0; offset <= 8 - bytes; offset += bytes) {
        int writers = 0;
        for (int tid = 0; tid < thread_count; ++tid)
          writers +=
              (separate_lanes ? (tid % (8 / bytes)) * bytes : 0) == offset;
        expected_word |= (static_cast<uint64_t>(writers * iterations) & mask)
                         << ((8 - bytes - offset) * 8);
      }
      EXPECT_EQ(words[1], expected_word);
      EXPECT_EQ(words[0], 123u);
      EXPECT_EQ(words[2], 456u);
      if (bytes == 8) {
        std::vector<uint64_t> all;
        for (const auto& values : returned)
          all.insert(all.end(), values.begin(), values.end());
        std::sort(all.begin(), all.end());
        for (size_t n = 0; n < all.size(); ++n) EXPECT_EQ(all[n], n);
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerDivisionAndRemainder) {
  std::vector<uint64_t> values = {0,
                                  1,
                                  2,
                                  3,
                                  UINT64_MAX,
                                  0x7fffffff,
                                  0x80000000,
                                  UINT64_C(0x7fffffffffffffff),
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0xdeadbeef00000001)};
  uint64_t random = 86420;
  for (int n = 0; n < 128; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool word32 : {false, true}) {
    for (bool is_unsigned : {false, true}) {
      for (bool remainder : {false, true}) {
        for (Register dst : {o0, o1, o2}) {
          MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                           CodeObjectRequired::kNo);
          compiler::EmitSparc64Divide(&a, dst, o0, o1, word32, is_unsigned,
                                      remainder);
          a.Move(o0, dst);
          a.Ret();
          ExecutableAssemblerBuffer code;
          ASSERT_TRUE(code.Initialize(a));
          auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
          for (uint64_t lhs : values) {
            for (uint64_t rhs : values) {
              uint64_t expected = 0;
              if (is_unsigned) {
                uint64_t left = word32 ? static_cast<uint32_t>(lhs) : lhs;
                uint64_t right = word32 ? static_cast<uint32_t>(rhs) : rhs;
                if (right) expected = remainder ? left % right : left / right;
              } else {
                int64_t left = word32 ? static_cast<int32_t>(lhs)
                                      : static_cast<int64_t>(lhs);
                int64_t right = word32 ? static_cast<int32_t>(rhs)
                                       : static_cast<int64_t>(rhs);
                if (left == INT64_MIN && right == -1) {
                  expected = remainder ? 0 : static_cast<uint64_t>(INT64_MIN);
                } else if (right) {
                  int64_t result = remainder ? left % right : left / right;
                  expected =
                      word32
                          ? static_cast<uint64_t>(static_cast<int32_t>(result))
                          : static_cast<uint64_t>(result);
                }
              }
              EXPECT_EQ(entry(lhs, rhs), expected)
                  << "lhs=" << lhs << " rhs=" << rhs << " word32=" << word32
                  << " unsigned=" << is_unsigned << " remainder=" << remainder;
            }
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerMultiplyHigh) {
  std::vector<uint64_t> values = {0,
                                  1,
                                  UINT64_MAX,
                                  0x7fffffff,
                                  0x80000000,
                                  UINT64_C(0x7fffffffffffffff),
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0xdeadbeef00000001)};
  for (int bit = 0; bit < 64; ++bit) values.push_back(uint64_t{1} << bit);
  uint64_t random = 24680;
  for (int n = 0; n < 64; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool word32 : {false, true}) {
    for (bool is_unsigned : {false, true}) {
      for (Register dst : {o0, o1, o2}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        compiler::EmitSparc64MultiplyHigh(&a, dst, o0, o1, word32, is_unsigned);
        a.Move(o0, dst);
        a.Ret();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
        for (uint64_t lhs : values) {
          for (uint64_t rhs : values) {
            uint64_t expected;
            if (word32) {
              expected = is_unsigned ? (uint64_t{static_cast<uint32_t>(lhs)} *
                                        static_cast<uint32_t>(rhs)) >>
                                           32
                                     : static_cast<uint64_t>(
                                           (int64_t{static_cast<int32_t>(lhs)} *
                                            static_cast<int32_t>(rhs)) >>
                                           32);
            } else {
              expected =
                  is_unsigned
                      ? static_cast<uint64_t>(
                            (static_cast<unsigned __int128>(lhs) * rhs) >> 64)
                      : static_cast<uint64_t>(
                            (static_cast<__int128>(static_cast<int64_t>(lhs)) *
                             static_cast<int64_t>(rhs)) >>
                            64);
            }
            EXPECT_EQ(entry(lhs, rhs), expected)
                << "lhs=" << lhs << " rhs=" << rhs << " word32=" << word32
                << " unsigned=" << is_unsigned;
          }
        }
      }
    }
  }
}

double ProbeFloatUnary(double x) {
  asm volatile("flushw" ::: "memory");
  return std::sin(x);
}

double ProbeFloatBinary(double x, double y) {
  asm volatile("flushw" ::: "memory");
  return std::atan2(x, y);
}

double ProbeFloatModulo(double x, double y) {
  asm volatile("flushw" ::: "memory");
  return std::fmod(x, y);
}

TEST(AssemblerSparc64BufferTest, NativeFloatingPointCBridge) {
  const double values[] = {0.0,
                           -0.0,
                           1.0,
                           -1.0,
                           3.141592653589793,
                           1e300,
                           -1e300,
                           1e-300,
                           -1e-300,
                           std::numeric_limits<double>::denorm_min(),
                           std::numeric_limits<double>::min(),
                           std::numeric_limits<double>::max(),
                           INFINITY,
                           -INFINITY,
                           std::numeric_limits<double>::quiet_NaN()};
  const uintptr_t functions[] = {
      reinterpret_cast<uintptr_t>(&ProbeFloatUnary),
      reinterpret_cast<uintptr_t>(&ProbeFloatBinary),
      reinterpret_cast<uintptr_t>(&ProbeFloatModulo)};
  for (int kind = 0; kind < 3; ++kind) {
    for (int padding : {0, 8}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      a.EnterCFunctionFrame();
      a.set_has_frame(true);
      a.AllocateStackSpace(padding);
      a.mov(l2, sp);
      a.MovInt64ToDouble(d0, o0);
      a.MovInt64ToDouble(d1, o1);
      a.PrepareCallCFunction(0, g1);
      a.LoadLiteral(g1, functions[kind]);
      a.CallCFunction(g1, 0, SetIsolateDataSlots::kNo);
      Label balanced;
      a.cmp(sp, l2);
      a.b(equal, ConditionCodes::kXcc, &balanced);
      a.nop();
      a.Trap();
      a.bind(&balanced);
      a.MovDoubleToInt64(o0, d0);
      a.LeaveFrame(StackFrame::MANUAL);
      a.RetCFunction();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
      for (double lhs : values) {
        for (double rhs : values) {
          double expected = kind == 0   ? std::sin(lhs)
                            : kind == 1 ? std::atan2(lhs, rhs)
                                        : std::fmod(lhs, rhs);
          uint64_t actual = entry(base::bit_cast<uint64_t>(lhs),
                                  base::bit_cast<uint64_t>(rhs));
          if (std::isnan(expected)) {
            EXPECT_TRUE(std::isnan(base::bit_cast<double>(actual)));
          } else {
            EXPECT_EQ(actual, base::bit_cast<uint64_t>(expected));
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeFloatWidthConversion) {
  std::vector<uint64_t> values = {0,
                                  UINT64_MAX,
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0x7ff0000000000000),
                                  UINT64_C(0xfff0000000000000),
                                  0x80000000,
                                  0x7f800000,
                                  0xff800000};
  for (int exponent = -1074; exponent <= 1023; ++exponent) {
    double value = std::ldexp(1.0, exponent);
    values.push_back(base::bit_cast<uint64_t>(value));
    values.push_back(base::bit_cast<uint64_t>(-value));
    values.push_back(base::bit_cast<uint64_t>(std::nextafter(value, 0.0)));
    values.push_back(base::bit_cast<uint64_t>(std::nextafter(value, INFINITY)));
  }
  uint64_t random = 13579;
  for (int n = 0; n < 4096; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool narrow : {false, true}) {
    for (DoubleRegister src : {d0, d15, d31}) {
      if (!narrow && src == d31) continue;
      for (DoubleRegister dst : {d0, d15, d31}) {
        if (narrow && dst == d31) continue;
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        a.EnterCFunctionFrame();
        if (narrow) {
          a.MovInt64ToDouble(src, o0);
          a.fdtos(dst, src);
          a.MovFloatToInt(o0, dst);
        } else {
          a.MovIntToFloat(src, o0);
          a.fstod(dst, src);
          a.MovDoubleToInt64(o0, dst);
        }
        a.LeaveFrame(StackFrame::MANUAL);
        a.RetCFunction();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        auto entry = code.entry<uint64_t (*)(uint64_t)>();
        for (uint64_t raw : values) {
          uint64_t actual = entry(raw);
          if (narrow) {
            float expected = static_cast<float>(base::bit_cast<double>(raw));
            if (std::isnan(expected)) {
              EXPECT_TRUE(std::isnan(
                  base::bit_cast<float>(static_cast<uint32_t>(actual))));
            } else {
              EXPECT_EQ(actual, base::bit_cast<uint32_t>(expected));
            }
          } else {
            double expected = static_cast<double>(
                base::bit_cast<float>(static_cast<uint32_t>(raw)));
            if (std::isnan(expected)) {
              EXPECT_TRUE(std::isnan(base::bit_cast<double>(actual)));
            } else {
              EXPECT_EQ(actual, base::bit_cast<uint64_t>(expected));
            }
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerSilenceNaN) {
  std::vector<uint64_t> values = {0,
                                  1,
                                  UINT64_MAX,
                                  UINT64_C(0x8000000000000000),
                                  UINT64_C(0x7ff0000000000000),
                                  UINT64_C(0xfff0000000000000),
                                  UINT64_C(0x7ff0000000000001),
                                  UINT64_C(0xfff0000000000001),
                                  UINT64_C(0x7ff8000000000001),
                                  UINT64_C(0xfff8000000000001),
                                  UINT64_C(0x7fefffffffffffff)};
  uint64_t random = 98765;
  for (int n = 0; n < 4096; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
    values.push_back((random & UINT64_C(0x800fffffffffffff)) |
                     UINT64_C(0x7ff0000000000000));
  }
  for (DoubleRegister src : {d0, d15, d31}) {
    for (DoubleRegister dst : {d0, d15, d31}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      a.EnterCFunctionFrame();
      a.MovInt64ToDouble(src, o0);
      compiler::EmitSparc64SilenceNaN(&a, dst, src);
      a.MovDoubleToInt64(o0, dst);
      a.LeaveFrame(StackFrame::MANUAL);
      a.RetCFunction();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t)>();
      for (uint64_t raw : values) {
        bool nan = (raw & UINT64_C(0x7ff0000000000000)) ==
                       UINT64_C(0x7ff0000000000000) &&
                   (raw & UINT64_C(0x000fffffffffffff)) != 0;
        uint64_t expected = nan ? raw | (uint64_t{1} << 51) : raw;
        EXPECT_EQ(entry(raw), expected) << "raw=" << raw;
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerCountLeadingZeros) {
  std::vector<uint64_t> values = {0, UINT64_MAX, UINT64_C(0xdeadbeef00000000)};
  for (int bit = 0; bit < 64; ++bit) {
    uint64_t power = uint64_t{1} << bit;
    for (uint64_t value : {power, power - 1, power + 1})
      values.push_back(value);
  }
  uint64_t random = 67890;
  for (int n = 0; n < 4096; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool word32 : {false, true}) {
    for (Register dst : {o0, o1}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      compiler::EmitSparc64CountLeadingZeros(&a, dst, o0, word32);
      a.Move(o0, dst);
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t)>();
      for (uint64_t raw : values) {
        uint64_t expected =
            word32 ? (static_cast<uint32_t>(raw)
                          ? __builtin_clz(static_cast<uint32_t>(raw))
                          : 32)
                   : (raw ? __builtin_clzll(raw) : 64);
        EXPECT_EQ(entry(raw), expected)
            << "raw=" << raw << " word32=" << word32;
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerDoubleWords) {
  const uint64_t words[] = {
      0,          1,          UINT64_MAX,
      0x80000000, 0x7ff00000, 0xfff00000,
      0x7ff80000, 0x7fffffff, UINT64_C(0xdeadbeef00000001)};
  for (DoubleRegister fpreg : {d0, d15, d31}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterCFunctionFrame();
    compiler::EmitSparc64DoubleFromWord32Pair(&a, fpreg, o0, o1);
    compiler::EmitSparc64ExtractDoubleWord(&a, o3, fpreg, true);
    compiler::EmitSparc64ExtractDoubleWord(&a, o4, fpreg, false);
    a.StoreU64(o3, MemOperand(o2));
    a.StoreU64(o4, MemOperand(o2, 8));
    a.MovDoubleToInt64(o0, fpreg);
    a.LeaveFrame(StackFrame::MANUAL);
    a.RetCFunction();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t, uint64_t*)>();
    for (uint64_t high : words) {
      for (uint64_t low : words) {
        uint64_t extracted[2] = {};
        EXPECT_EQ(entry(high, low, extracted),
                  (high << 32) | static_cast<uint32_t>(low));
        EXPECT_EQ(extracted[0], static_cast<uint32_t>(high));
        EXPECT_EQ(extracted[1], static_cast<uint32_t>(low));
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerFloatToInteger) {
  std::vector<uint64_t> values = {0,
                                  UINT64_MAX,
                                  UINT64_C(0x7ff0000000000000),
                                  UINT64_C(0xfff0000000000000),
                                  UINT64_C(0x7ff0000000000001),
                                  UINT64_C(0x8000000000000000),
                                  0x7f800000,
                                  0xff800000,
                                  0x7f800001};
  for (int exponent = -2; exponent <= 65; ++exponent) {
    double power = std::ldexp(1.0, exponent);
    for (double value :
         {power, std::nextafter(power, 0.0), std::nextafter(power, INFINITY)}) {
      for (double signed_value : {value, -value}) {
        values.push_back(base::bit_cast<uint64_t>(signed_value));
        values.push_back(
            base::bit_cast<uint32_t>(static_cast<float>(signed_value)));
      }
    }
  }
  uint64_t random = 54321;
  for (int n = 0; n < 4096; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool word32 : {false, true}) {
    for (bool is_unsigned : {false, true}) {
      for (bool single : {false, true}) {
        for (bool check_success : {false, true}) {
          MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                           CodeObjectRequired::kNo);
          a.EnterCFunctionFrame();
          if (single) {
            a.MovIntToFloat(d0, o0);
          } else {
            a.MovInt64ToDouble(d0, o0);
          }
          compiler::EmitSparc64FloatToInteger(&a, o0,
                                              check_success ? o2 : no_reg, d0,
                                              word32, is_unsigned, single);
          if (check_success) a.StoreU64(o2, MemOperand(o1));
          a.LeaveFrame(StackFrame::MANUAL);
          a.RetCFunction();
          ExecutableAssemblerBuffer code;
          ASSERT_TRUE(code.Initialize(a));
          auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t*)>();
          int bits = word32 ? 32 : 64;
          long double upper = std::ldexp(1.0L, bits - !is_unsigned);
          long double lower = is_unsigned ? 0 : -upper;
          for (uint64_t raw : values) {
            long double value =
                single ? static_cast<long double>(
                             base::bit_cast<float>(static_cast<uint32_t>(raw)))
                       : static_cast<long double>(base::bit_cast<double>(raw));
            long double truncated = std::trunc(value);
            bool valid =
                std::isfinite(value) && truncated >= lower && truncated < upper;
            uint64_t expected =
                !valid ? 0
                : is_unsigned
                    ? static_cast<uint64_t>(truncated)
                    : static_cast<uint64_t>(static_cast<int64_t>(truncated));
            uint64_t success = 42;
            EXPECT_EQ(entry(raw, &success), expected)
                << "raw=" << raw << " word32=" << word32
                << " unsigned=" << is_unsigned << " single=" << single;
            EXPECT_EQ(success,
                      check_success ? static_cast<uint64_t>(valid) : 42u);
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerIntegerToFloat) {
  std::vector<uint64_t> values = {
      0, 1, UINT64_MAX, 0x7fffffff, 0x80000000, UINT64_C(0xdeadbeef00000001)};
  for (int bit = 0; bit < 64; ++bit) {
    uint64_t power = uint64_t{1} << bit;
    uint64_t half_ulp = uint64_t{1} << std::max(0, bit - 53);
    for (uint64_t delta :
         {uint64_t{0}, uint64_t{1}, uint64_t{3}, half_ulp, half_ulp + 1}) {
      values.push_back(power + delta);
      values.push_back(power - delta);
    }
  }
  uint64_t random = 12345;
  for (int n = 0; n < 1024; ++n) {
    random ^= random << 13;
    random ^= random >> 7;
    random ^= random << 17;
    values.push_back(random);
  }
  for (bool word32 : {false, true}) {
    for (bool is_unsigned : {false, true}) {
      for (bool single : {false, true}) {
        for (DoubleRegister dst : {d0, d15, d31}) {
          if (single && dst == d31) continue;
          MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                           CodeObjectRequired::kNo);
          a.EnterCFunctionFrame();
          compiler::EmitSparc64IntegerToFloat(&a, dst, o0, word32, is_unsigned,
                                              single);
          if (single) {
            a.MovFloatToInt(o0, dst);
          } else {
            a.MovDoubleToInt64(o0, dst);
          }
          a.LeaveFrame(StackFrame::MANUAL);
          a.RetCFunction();
          ExecutableAssemblerBuffer code;
          ASSERT_TRUE(code.Initialize(a));
          auto entry = code.entry<uint64_t (*)(uint64_t)>();
          for (uint64_t value : values) {
            uint64_t expected;
            if (single) {
              float converted =
                  word32
                      ? (is_unsigned
                             ? static_cast<float>(static_cast<uint32_t>(value))
                             : static_cast<float>(static_cast<int32_t>(value)))
                      : (is_unsigned
                             ? static_cast<float>(value)
                             : static_cast<float>(static_cast<int64_t>(value)));
              expected = base::bit_cast<uint32_t>(converted);
            } else {
              double converted =
                  word32
                      ? (is_unsigned
                             ? static_cast<double>(static_cast<uint32_t>(value))
                             : static_cast<double>(static_cast<int32_t>(value)))
                      : (is_unsigned ? static_cast<double>(value)
                                     : static_cast<double>(
                                           static_cast<int64_t>(value)));
              expected = base::bit_cast<uint64_t>(converted);
            }
            EXPECT_EQ(entry(value), expected)
                << "value=" << value << " word32=" << word32
                << " unsigned=" << is_unsigned << " single=" << single;
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerMultiplyWithHighWord) {
  using namespace compiler;  // NOLINT(build/namespaces)
  const uint64_t values[] = {0,
                             1,
                             UINT64_MAX,
                             0x7fffffff,
                             0x80000000,
                             46340,
                             46341,
                             0xffff4afb,
                             UINT64_C(0xdeadbeef00000001),
                             UINT64_C(0x1234567880000000)};
  for (Register low : {o0, o1, o2}) {
    for (Register high : {o0, o1, o3}) {
      if (low == high) continue;
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      EmitSparc64Mul32WithHigh32(&a, low, high, o0, o1);
      a.sllx(g3, high, 32);
      a.srl(g4, low, 0);
      a.or_(o0, g3, g4);
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
      for (uint64_t lhs : values) {
        for (uint64_t rhs : values) {
          int64_t expected = int64_t{static_cast<int32_t>(lhs)} *
                             int64_t{static_cast<int32_t>(rhs)};
          EXPECT_EQ(entry(lhs, rhs), static_cast<uint64_t>(expected));
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerOverflowConditions) {
  using namespace compiler;  // NOLINT(build/namespaces)
  const uint64_t values[] = {0,
                             1,
                             UINT64_MAX,
                             0x7fffffff,
                             0x80000000,
                             UINT64_C(0x7fffffffffffffff),
                             UINT64_C(0x8000000000000000)};
  for (bool word32 : {false, true}) {
    for (bool subtract : {false, true}) {
      for (FlagsCondition condition : {kOverflow, kNotOverflow}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        if (subtract) {
          a.subcc(o0, o0, o1);
        } else {
          a.addcc(o0, o0, o1);
        }
        ArchOpcode opcode = word32 ? (subtract ? kSparcSubWithOverflow32
                                               : kSparcAddWithOverflow32)
                                   : (subtract ? kSparcSub : kSparcAdd64);
        ReturnBranchResult(&a, opcode, condition);
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
        for (uint64_t lhs : values) {
          for (uint64_t rhs : values) {
            int64_t left =
                word32 ? static_cast<int32_t>(lhs) : static_cast<int64_t>(lhs);
            int64_t right =
                word32 ? static_cast<int32_t>(rhs) : static_cast<int64_t>(rhs);
            int64_t result;
            bool overflowed =
                subtract ? __builtin_sub_overflow(left, right, &result)
                         : __builtin_add_overflow(left, right, &result);
            if (word32) overflowed = result < INT32_MIN || result > INT32_MAX;
            EXPECT_EQ(entry(lhs, rhs),
                      static_cast<uint64_t>(
                          condition == kOverflow ? overflowed : !overflowed));
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeCompilerFloatConditions) {
  using namespace compiler;  // NOLINT(build/namespaces)
  const FlagsCondition conditions[] = {kEqual,
                                       kNotEqual,
                                       kUnsignedLessThan,
                                       kUnsignedGreaterThanOrEqual,
                                       kUnsignedLessThanOrEqual,
                                       kUnsignedGreaterThan,
                                       kFloatLessThanOrUnordered,
                                       kFloatGreaterThanOrEqual,
                                       kFloatLessThanOrEqual,
                                       kFloatGreaterThanOrUnordered,
                                       kFloatLessThan,
                                       kFloatGreaterThanOrEqualOrUnordered,
                                       kFloatLessThanOrEqualOrUnordered,
                                       kFloatGreaterThan,
                                       kUnorderedEqual,
                                       kUnorderedNotEqual,
                                       kIsNaN,
                                       kIsNotNaN};
  const double values[] = {-1.5,
                           1.5,
                           -0.0,
                           0.0,
                           std::numeric_limits<double>::infinity(),
                           -std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()};
  for (bool single : {false, true}) {
    for (size_t n = 0; n < std::size(conditions); ++n) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      if (single) {
        a.LoadF32(d14, MemOperand(o0));
        a.LoadF32(d15, MemOperand(o1));
        a.fcmps(d14, d15);
      } else {
        a.LoadF64(d30, MemOperand(o0));
        a.LoadF64(d31, MemOperand(o1));
        a.fcmpd(d30, d31);
      }
      ReturnBranchResult(&a, kSparcCmpDouble, conditions[n]);
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(const void*, const void*)>();
      for (double left : values) {
        for (double right : values) {
          bool unordered = std::isnan(left) || std::isnan(right);
          bool lt = left < right;
          bool le = left <= right;
          bool gt = left > right;
          bool ge = left >= right;
          const bool expected[] = {left == right,
                                   left != right,
                                   lt,
                                   !lt,
                                   le,
                                   !le,
                                   lt || unordered,
                                   ge,
                                   le,
                                   gt || unordered,
                                   lt,
                                   ge || unordered,
                                   le || unordered,
                                   gt,
                                   left == right,
                                   left != right,
                                   unordered,
                                   !unordered};
          float fleft = static_cast<float>(left);
          float fright = static_cast<float>(right);
          EXPECT_EQ(entry(single ? static_cast<const void*>(&fleft) : &left,
                          single ? static_cast<const void*>(&fright) : &right),
                    static_cast<uint64_t>(expected[n]))
              << "single=" << single
              << " condition=" << static_cast<int>(conditions[n]);
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeSinglePrecisionArithmetic) {
  for (int operation = 0; operation < 7; ++operation) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.LoadF32(d14, MemOperand(o0));
    a.LoadF32(d15, MemOperand(o0, 4));
    switch (operation) {
      case 0:
        a.fadds(d14, d14, d15);
        break;
      case 1:
        a.fsubs(d14, d14, d15);
        break;
      case 2:
        a.fmuls(d14, d14, d15);
        break;
      case 3:
        a.fdivs(d14, d14, d15);
        break;
      case 4:
        a.fnegs(d14, d14);
        break;
      case 5:
        a.fabss(d14, d14);
        break;
      case 6:
        a.fsqrts(d14, d14);
        break;
    }
    a.MovFloatToInt(o0, d14);
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint32_t (*)(const float*)>();
    for (float left : {-2.25f, 2.25f, -0.0f, 0.0f, 0.1f,
                       std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
      for (float right : {-1.5f, 1.5f, 0.0f, 0.1f}) {
        float expected = 0;
        switch (operation) {
          case 0:
            expected = left + right;
            break;
          case 1:
            expected = left - right;
            break;
          case 2:
            expected = left * right;
            break;
          case 3:
            expected = left / right;
            break;
          case 4:
            expected = -left;
            break;
          case 5:
            expected = std::fabs(left);
            break;
          case 6:
            expected = std::sqrt(left);
            break;
        }
        const float input[] = {left, right};
        uint32_t result = entry(input);
        if (std::isnan(expected)) {
          EXPECT_TRUE(std::isnan(base::bit_cast<float>(result)));
        } else {
          EXPECT_EQ(result, base::bit_cast<uint32_t>(expected)) << operation;
        }
      }
    }
  }
}

uint64_t FlushNativeCompilerFrame(const uintptr_t* frame) {
  asm volatile("flushw" ::: "memory");
  uintptr_t native_sp;
  asm volatile("mov %%sp, %0" : "=r"(native_sp));
  if (((native_sp + kSparcStackBias) & 15) != 0) return 0;
  if ((reinterpret_cast<uintptr_t>(frame) & 15) != 0) return 0;
  return frame[1] != 0 && (frame[1] & 3) == 0 ? 42 : 0;
}

TEST(AssemblerSparc64BufferTest, NativeCompilerCFrameStackArguments) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.set_has_frame(true);
  a.AllocateStackSpace(8 * kSystemPointerSize);
  for (int n = 0; n < 6; ++n) {
    a.StoreU64(kCArgRegs[n], MemOperand(sp, n * kSystemPointerSize));
  }
  const int arg_offset =
      CommonFrameConstants::kCallerSPOffset + kSparcMinimumFrameSize;
  for (int n = 0; n < 2; ++n) {
    a.LoadU64(g1, MemOperand(fp, arg_offset + n * kSystemPointerSize));
    a.StoreU64(g1, MemOperand(sp, (n + 6) * kSystemPointerSize));
  }
  a.Move(o0, fp);
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&FlushNativeCompilerFrame));
  a.CallCFunction(g1, 1, SetIsolateDataSlots::kNo);
  for (int n = 0; n < 8; ++n) {
    a.LoadU64(g1, MemOperand(sp, n * kSystemPointerSize));
    a.add(o0, o0, g1);
  }
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  using Entry = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                             uint64_t, uint64_t, uint64_t);
  EXPECT_EQ(code.entry<Entry>()(1, 2, 4, 8, 16, 32, 64, 128), 297u);
}

TEST(AssemblerSparc64BufferTest, NativeCompilerCFramePairReturn) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.mov(o1, 43);
  a.mov(o0, 42);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  auto pair = code.entry<NativeResultPair (*)()>()();
  EXPECT_EQ(pair.first, 42u);
  EXPECT_EQ(pair.second, 43u);
}

TEST(AssemblerSparc64BufferTest, NativeTailFrameStateAndDynamicDrop) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.Push(g0);
  a.LoadStackAddress(l0);
  a.mov(l1, fp);
  a.LoadLiteral(g1, 0x123456789abcdef0);
  a.Push(g1);
  a.PushCommonFrame();
  a.AllocateStackSpace(3 * kSystemPointerSize);
  a.RestoreFrameStateForTailCall();
  a.LoadStackAddress(g1);
  a.SubS64(g2, l0, 6 * kSystemPointerSize);
  a.cmp(g1, g2);
  Label failed, done;
  a.b(not_equal, ConditionCodes::kXcc, &failed);
  a.nop();
  a.cmp(fp, l1);
  a.b(not_equal, ConditionCodes::kXcc, &failed);
  a.nop();
  a.mov(g3, 6);
  a.Drop(g3);
  a.LoadStackAddress(g1);
  a.cmp(g1, l0);
  a.b(not_equal, ConditionCodes::kXcc, &failed);
  a.nop();
  a.mov(o0, 1);
  a.Jump(&done);
  a.bind(&failed);
  a.mov(o0, 0);
  a.bind(&done);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EXPECT_EQ(code.entry<uint64_t (*)(uint64_t)>()(0), 1u);
}

TEST(AssemblerSparc64BufferTest, NativeDeoptimizerSavedFloatLanes) {
  RegisterValues values{};
  const uint32_t patterns[] = {0,          0x80000000, 1,         0x7f800000,
                               0x7fc12345, 0x7f812345, 0xff812345};
  for (int n = 0; n < DoubleRegister::kNumRegisters; ++n) {
    for (uint32_t bits : patterns) {
      uint64_t raw = (uint64_t{bits} << 32) | 0xdeadbeef;
      values.SetDoubleRegister(n, Float64::FromBits(raw));
      EXPECT_EQ(values.GetDoubleRegister(n).get_bits(), raw);
      EXPECT_EQ(values.GetFloatRegister(n).get_bits(), bits);
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeDeoptimizerJumpPatch) {
  for (bool backward : {false, true}) {
    Assembler a(AssemblerOptions{});
    Label target, patch;
    if (backward) {
      a.ba(&patch);
      a.nop();
      a.bind(&target);
      a.mov(o0, 42);
      a.retl();
      a.nop();
    }
    a.bind(&patch);
    a.nop();
    a.nop();
    a.nop();
    a.mov(o0, 0);
    a.retl();
    a.nop();
    if (!backward) {
      a.bind(&target);
      a.mov(o0, 42);
      a.retl();
      a.nop();
    }
    const Address start = reinterpret_cast<Address>(a.buffer_start());
    Deoptimizer::PatchToJump(start + patch.pos(), start + target.pos());
    EXPECT_EQ(a.instr_at(patch.pos() + 8), 0x01000000u);
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EXPECT_EQ(code.entry<uint64_t (*)()>()(), 42u);
  }
}

TEST(AssemblerSparc64BufferTest, DeoptimizerZapPreservesEmbeddedObjects) {
  Assembler a(AssemblerOptions{});
  const uint64_t literal = UINT64_C(0x123456789abcdef0);
  a.LoadLiteral(o0, literal);
  a.nop();
  a.nop();
  std::array<uint8_t, 128> relocation;
  RelocInfoWriter writer;
  writer.Reposition(relocation.data() + relocation.size(), a.buffer_start());
  const Address start = reinterpret_cast<Address>(a.buffer_start());
  RelocInfo info(start + 16, RelocInfo::FULL_EMBEDDED_OBJECT);
  writer.Write(&info);
  RelocIterator iterator(
      base::Vector<uint8_t>(a.buffer_start(), a.pc_offset()),
      base::Vector<const uint8_t>(
          writer.pos(), relocation.data() + relocation.size() - writer.pos()),
      kNullAddress);
  ASSERT_FALSE(iterator.done());
  Deoptimizer::ZapCode(start, start + a.pc_offset(), iterator);
  for (int offset = 0; offset < a.pc_offset(); offset += 4) {
    if (offset == 16 || offset == 20) continue;
    EXPECT_EQ(a.instr_at(offset), 0x91d02001u) << offset;
  }
  EXPECT_EQ(base::ReadUnalignedValue<uint64_t>(start + 16), literal);
}

struct StackSpillObservation {
  uintptr_t parent_sp;
  uintptr_t parent_fp;
  uintptr_t parent_pc;
  uint32_t found;
  bool marker_valid;
  bool argument_valid;
};

constexpr uint64_t kStackSpillSeed = UINT64_C(0x1234567800000000);

void CheckCppgcRegisterSpills(::heap::base::Stack* stack, void* argument,
                              const void* marker) {
  auto* observation = reinterpret_cast<StackSpillObservation*>(stack);
  observation->argument_valid = argument == observation;
  uintptr_t top = reinterpret_cast<uintptr_t>(marker);
  observation->marker_valid =
      (top & 15) == 0 && top + kSparcMinimumFrameSize == observation->parent_sp;
  const auto* slots =
      reinterpret_cast<const uintptr_t*>(observation->parent_sp);
  uint32_t found = 0;
  for (int n = 0; n < 14; ++n) {
    int reg_code = n < 8 ? l0.code() + n : i0.code() + n - 8;
    if (slots[n] == kStackSpillSeed + reg_code * 0x101) found |= 1u << n;
  }
  if (slots[14] == observation->parent_fp) found |= 1u << 14;
  if (slots[15] == observation->parent_pc) found |= 1u << 15;
  observation->found = found;
}

TEST(AssemblerSparc64BufferTest, NativeCppgcWindowRegisterSpills) {
  Assembler a(AssemblerOptions{});
  a.save(sp, sp, -kSparcMinimumFrameSize);
  a.mov(g5, i0);
  a.add(g1, sp, kSparcStackBias);
  a.stx(g1, g5, offsetof(StackSpillObservation, parent_sp));
  a.stx(i6, g5, offsetof(StackSpillObservation, parent_fp));
  a.stx(i7, g5, offsetof(StackSpillObservation, parent_pc));
  for (int n = 0; n < 16; ++n) {
    a.stx(g0, sp, kSparcStackBias + n * kSystemPointerSize);
  }
  for (int n = 0; n < 14; ++n) {
    Register reg =
        Register::from_code(n < 8 ? l0.code() + n : i0.code() + n - 8);
    a.LoadLiteral(reg, kStackSpillSeed + reg.code() * 0x101);
  }
  a.mov(o0, g5);
  a.mov(o1, g5);
  a.LoadLiteral(o2, reinterpret_cast<uintptr_t>(&CheckCppgcRegisterSpills));
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(
                        &::heap::base::PushAllRegistersAndIterateStack));
  a.jmpl(o7, g1, 0);
  a.nop();
  a.ret();
  a.restore();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  StackSpillObservation observation{};
  code.entry<void (*)(StackSpillObservation*)>()(&observation);
  EXPECT_TRUE(observation.marker_valid);
  EXPECT_TRUE(observation.argument_valid);
  EXPECT_EQ(observation.found, 0xffffu);
}

extern "C" const Instr sparc64_reference_begin[];
extern "C" const Instr sparc64_reference_end[];

TEST(AssemblerSparc64BufferTest, DisassemblerGnuInstructionCoverage) {
  disasm::NameConverter converter;
  disasm::Disassembler decoder(converter);
  size_t words = (reinterpret_cast<uintptr_t>(sparc64_reference_end) -
                  reinterpret_cast<uintptr_t>(sparc64_reference_begin)) /
                 sizeof(Instr);
  for (size_t n = 0; n < words; ++n) {
    base::EmbeddedVector<char, 256> text;
    uint8_t* pc = const_cast<uint8_t*>(
        reinterpret_cast<const uint8_t*>(sparc64_reference_begin + n));
    EXPECT_EQ(decoder.InstructionDecode(text, pc), kInstrSize);
    EXPECT_NE(text[0], '\0');
    EXPECT_NE(std::string(text.begin()).find(".word"), 0u) << n;
  }
}

class DisassemblerAddressConverter final : public disasm::NameConverter {
 public:
  const char* NameOfAddress(uint8_t*) const override { return "target"; }
};

TEST(AssemblerSparc64BufferTest, DisassemblerOperandsAndPredicates) {
  Assembler a(AssemblerOptions{});
  a.fsubd(d16, d17, d31);
  a.ldd(d16, o0, 0);
  a.fcmps(d14, d15, 3);
  a.b(equal, ConditionCodes::kXcc, 16);
  a.fb(kFLess, 3, -4, true);
  a.ldsw(o0, o1, g1);
  a.stx(o0, o1, -4096);
  a.sllx(o0, o1, 63);
  a.casx(o2, o0, o1);
  a.fxtod(d31, d16);
  a.fxtos(d15, d31);
  a.fdtos(d15, d31);
  a.fstod(d31, d15);
  a.dd(0xffffffffu);
  const char* expected[] = {"fsubd %f34, %f62, %f32",  "ldd [%o0 + 0], %f32",
                            "fcmps %fcc3, %f28, %f30", "be,pt %xcc, target",
                            "fbl,a,pt %fcc3, target",  "ldsw [%o1 + %g1], %o0",
                            "stx %o0, [%o1 + -4096]",  "sllx %o1, 63, %o0",
                            "casx [%o0], %o1, %o2",    "fxtod %f32, %f62",
                            "fxtos %f62, %f30",        "fdtos %f62, %f30",
                            "fstod %f30, %f62",        ".word 0xffffffff"};
  DisassemblerAddressConverter converter;
  disasm::Disassembler decoder(
      converter, disasm::Disassembler::kContinueOnUnimplementedOpcode);
  for (size_t n = 0; n < std::size(expected); ++n) {
    base::EmbeddedVector<char, 256> text;
    uint8_t* pc = a.buffer_start() + n * kInstrSize;
    EXPECT_EQ(decoder.InstructionDecode(text, pc), kInstrSize);
    EXPECT_STREQ(text.begin(), expected[n]);
    EXPECT_EQ(decoder.ConstantPoolSizeAt(pc), -1);
  }
}

uintptr_t ReadEntryField(uint8_t* root, IsolateFieldId id) {
  return base::ReadUnalignedValue<uintptr_t>(reinterpret_cast<Address>(root) +
                                             IsolateData::GetOffset(id));
}

void WriteEntryField(uint8_t* root, IsolateFieldId id, uintptr_t value) {
  base::WriteUnalignedValue<uintptr_t>(
      reinterpret_cast<Address>(root) + IsolateData::GetOffset(id), value);
}

class EntryRootStorage {
 public:
  EntryRootStorage() : data_((sizeof(IsolateData) + 7) / 8) {}
  uint8_t* root() {
    return reinterpret_cast<uint8_t*>(data_.data()) + kRootRegisterBias;
  }

 private:
  std::vector<uint64_t> data_;
};

struct EntryFrameProbe {
  using Entry = uint64_t (*)(uint8_t*, EntryFrameProbe*);
  uintptr_t c_entry_fp = 0x1111222233334440;
  uintptr_t fast_fp = 0x2222333344445550;
  uintptr_t fast_pc = 0x3333444455556660;
  uintptr_t handler = 0x4444555566667770;
  uintptr_t entry_sp = 0;
  StackFrame::Type type = StackFrame::ENTRY;
  Entry nested = nullptr;
};

constexpr uint64_t kEntryProbeResult = UINT64_C(0x123456789abcdef1);

uint64_t ProbeJSEntryFrame(uint8_t* root, const uintptr_t* frame,
                           EntryFrameProbe* probe) {
  asm volatile("flushw" ::: "memory");
  uintptr_t native_sp;
  asm volatile("mov %%sp, %0" : "=r"(native_sp));
  if (((native_sp + kSparcStackBias) & 15) != 0) return 1;
  if (frame[0] != UINTPTR_MAX) return 2;
  if (frame[-1] !=
          static_cast<uintptr_t>(StackFrame::TypeToMarker(probe->type)) ||
      frame[-2] != frame[-1])
    return 3;
  if (frame[EntryFrameConstants::kNextExitFrameFPOffset / 8] !=
          probe->c_entry_fp ||
      frame[EntryFrameConstants::kNextFastCallFrameFPOffset / 8] !=
          probe->fast_fp ||
      frame[EntryFrameConstants::kNextFastCallFramePCOffset / 8] !=
          probe->fast_pc)
    return 4;
  if (ReadEntryField(root, IsolateFieldId::kCEntryFP) != 0 ||
      ReadEntryField(root, IsolateFieldId::kFastCCallCallerFP) != 0 ||
      ReadEntryField(root, IsolateFieldId::kFastCCallCallerPC) != 0)
    return 5;
  uintptr_t expected_entry_sp = probe->entry_sp == 0
                                    ? reinterpret_cast<uintptr_t>(frame)
                                    : probe->entry_sp;
  if (ReadEntryField(root, IsolateFieldId::kJSEntrySP) != expected_entry_sp)
    return 6;
  uintptr_t handler = ReadEntryField(root, IsolateFieldId::kHandler);
  if (handler != reinterpret_cast<uintptr_t>(frame) - 8 * kSystemPointerSize)
    return 7;
  const auto* chain = reinterpret_cast<const uintptr_t*>(handler);
  if (chain[0] != probe->handler || chain[1] != 0) return 8;
  if (probe->nested != nullptr) {
    EntryFrameProbe nested;
    nested.c_entry_fp = 0;
    nested.fast_fp = 0;
    nested.fast_pc = 0;
    nested.handler = handler;
    nested.entry_sp = expected_entry_sp;
    nested.type = StackFrame::CONSTRUCT_ENTRY;
    if (probe->nested(root, &nested) != kEntryProbeResult) return 9;
    if (ReadEntryField(root, IsolateFieldId::kHandler) != handler ||
        ReadEntryField(root, IsolateFieldId::kJSEntrySP) != expected_entry_sp ||
        ReadEntryField(root, IsolateFieldId::kCEntryFP) != 0 ||
        ReadEntryField(root, IsolateFieldId::kFastCCallCallerFP) != 0 ||
        ReadEntryField(root, IsolateFieldId::kFastCCallCallerPC) != 0)
      return 10;
  }
  WriteEntryField(root, IsolateFieldId::kCEntryFP, 0x8880);
  WriteEntryField(root, IsolateFieldId::kFastCCallCallerFP, 0x9990);
  WriteEntryField(root, IsolateFieldId::kFastCCallCallerPC, 0xaaa0);
  return kEntryProbeResult;
}

void GenerateEntryFrameProbe(MacroAssembler* a, StackFrame::Type type,
                             bool unwind) {
  a->EnterJSEntryFrame(type);
  a->set_has_frame(true);
  a->PushStackHandler();
  a->Move(o0, kRootRegister);
  a->Move(o1, fp);
  a->Move(o2, i1);
  a->LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeJSEntryFrame));
  a->CallCFunction(g1, 3, SetIsolateDataSlots::kNo);
  if (unwind) {
    a->LoadU64(g5, a->AsMemOperand(IsolateFieldId::kHandler));
    a->LoadU64(g1, MemOperand(g5));
    a->StoreU64(g1, a->AsMemOperand(IsolateFieldId::kHandler));
    a->AddS64(g5, g5, StackHandlerConstants::kSize);
    a->Move(sp, g5);
    a->Move(fp, 0);
  } else {
    a->PopStackHandler();
  }
  a->LeaveJSEntryFrame();
}

void InitializeEntryRoots(uint8_t* root, const EntryFrameProbe& probe) {
  WriteEntryField(root, IsolateFieldId::kCEntryFP, probe.c_entry_fp);
  WriteEntryField(root, IsolateFieldId::kFastCCallCallerFP, probe.fast_fp);
  WriteEntryField(root, IsolateFieldId::kFastCCallCallerPC, probe.fast_pc);
  WriteEntryField(root, IsolateFieldId::kHandler, probe.handler);
  WriteEntryField(root, IsolateFieldId::kJSEntrySP, probe.entry_sp);
}

void CheckRestoredEntryRoots(uint8_t* root, const EntryFrameProbe& probe) {
  EXPECT_EQ(ReadEntryField(root, IsolateFieldId::kCEntryFP), probe.c_entry_fp);
  EXPECT_EQ(ReadEntryField(root, IsolateFieldId::kFastCCallCallerFP),
            probe.fast_fp);
  EXPECT_EQ(ReadEntryField(root, IsolateFieldId::kFastCCallCallerPC),
            probe.fast_pc);
  EXPECT_EQ(ReadEntryField(root, IsolateFieldId::kHandler), probe.handler);
  EXPECT_EQ(ReadEntryField(root, IsolateFieldId::kJSEntrySP), probe.entry_sp);
}

TEST(AssemblerSparc64BufferTest, NativeJSEntryArguments) {
  for (StackFrame::Type type :
       {StackFrame::ENTRY, StackFrame::CONSTRUCT_ENTRY}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterJSEntryFrame(type);
    a.PushStackHandler();
    a.add(o0, i1, i2);
    a.add(o0, o0, i3);
    a.add(o0, o0, i4);
    a.add(o0, o0, i5);
    a.PopStackHandler();
    a.LeaveJSEntryFrame();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EntryRootStorage storage;
    EntryFrameProbe probe;
    InitializeEntryRoots(storage.root(), probe);
    using Entry = uint64_t (*)(uint8_t*, uint64_t, uint64_t, uint64_t, uint64_t,
                               uint64_t);
    EXPECT_EQ(code.entry<Entry>()(storage.root(), 1, 2, 4, 8, 16), 31u);
    CheckRestoredEntryRoots(storage.root(), probe);
  }
}

TEST(AssemblerSparc64BufferTest, NativeJSEntryMetadataAndUnwind) {
  for (StackFrame::Type type :
       {StackFrame::ENTRY, StackFrame::CONSTRUCT_ENTRY}) {
    for (bool inner : {false, true}) {
      for (bool unwind : {false, true}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        GenerateEntryFrameProbe(&a, type, unwind);
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        EntryRootStorage storage;
        EntryFrameProbe probe;
        probe.type = type;
        if (inner) probe.entry_sp = 0x777788889999aaa0;
        InitializeEntryRoots(storage.root(), probe);
        EXPECT_EQ(code.entry<EntryFrameProbe::Entry>()(storage.root(), &probe),
                  kEntryProbeResult)
            << "type=" << static_cast<int>(type) << " inner=" << inner
            << " unwind=" << unwind;
        CheckRestoredEntryRoots(storage.root(), probe);
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeNestedJSEntry) {
  MacroAssembler inner(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
  GenerateEntryFrameProbe(&inner, StackFrame::CONSTRUCT_ENTRY, true);
  ExecutableAssemblerBuffer inner_code;
  ASSERT_TRUE(inner_code.Initialize(inner));
  MacroAssembler outer(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
  GenerateEntryFrameProbe(&outer, StackFrame::ENTRY, false);
  ExecutableAssemblerBuffer outer_code;
  ASSERT_TRUE(outer_code.Initialize(outer));
  EntryRootStorage storage;
  EntryFrameProbe probe;
  probe.nested = inner_code.entry<EntryFrameProbe::Entry>();
  InitializeEntryRoots(storage.root(), probe);
  EXPECT_EQ(outer_code.entry<EntryFrameProbe::Entry>()(storage.root(), &probe),
            kEntryProbeResult);
  CheckRestoredEntryRoots(storage.root(), probe);
}

constexpr uintptr_t kExitOldContext = UINT64_C(0x1111222233334441);
constexpr uintptr_t kExitNewContext = UINT64_C(0x3333444455556661);
constexpr uint64_t kExitPairSecond = UINT64_C(0x9876543210fedcba);

bool ExitCallMetadataIsValid(uint8_t* root) {
  asm volatile("flushw" ::: "memory");
  uintptr_t native_sp;
  asm volatile("mov %%sp, %0" : "=r"(native_sp));
  if (((native_sp + kSparcStackBias) & 15) != 0) return false;
  auto* frame = reinterpret_cast<const uintptr_t*>(
      ReadEntryField(root, IsolateFieldId::kCEntryFP));
  if (frame == nullptr) return false;
  uintptr_t saved_sp =
      frame[ExitFrameConstants::kSPOffset / kSystemPointerSize];
  uintptr_t pc =
      *reinterpret_cast<const uintptr_t*>(saved_sp - kSystemPointerSize);
  if (pc == 0 || (pc & 3) != 0) return false;
  if (ReadEntryField(root, IsolateFieldId::kContext) != kExitOldContext)
    return false;
  if (frame[0] == 0 || frame[1] == 0) return false;
  return true;
}

NativeResultPair ProbeFunctionCallbackInfo(
    const v8::FunctionCallbackInfo<v8::Value>* info) {
  auto* root =
      reinterpret_cast<uint8_t*>(info->GetIsolate()) + kRootRegisterBias;
  if (!ExitCallMetadataIsValid(root) || info->IsConstructCall())
    return {UINT64_MAX, 0};
  auto* frame = reinterpret_cast<const uintptr_t*>(
      ReadEntryField(root, IsolateFieldId::kCEntryFP));
  if (reinterpret_cast<uintptr_t>(info) !=
      reinterpret_cast<uintptr_t>(frame) +
          ApiCallbackExitFrameConstants::kFunctionCallbackInfoOffset)
    return {UINT64_MAX, 1};
  uintptr_t saved_sp =
      frame[ExitFrameConstants::kSPOffset / kSystemPointerSize];
  uintptr_t pc =
      *reinterpret_cast<const uintptr_t*>(saved_sp - kSystemPointerSize);
  uintptr_t return_pc = reinterpret_cast<uintptr_t>(
      __builtin_extract_return_addr(__builtin_return_address(0)));
  if (pc != return_pc) return {UINT64_MAX, 2};
  for (int n = 0; n < info->Length(); ++n) {
    if (frame[ApiCallbackExitFrameConstants::kFirstJSArgumentOffset /
                  kSystemPointerSize +
              n] != Smi::FromInt(100 + n).ptr())
      return {UINT64_MAX, 3};
  }
  info->GetReturnValue().Set(42);
  return {static_cast<uint64_t>(info->Length()), 0};
}

TEST(AssemblerSparc64BufferTest, NativeFunctionCallbackInfoLayout) {
  using FC = ApiCallbackExitFrameConstants;
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.set_has_frame(true);
  a.Move(kRootRegister, o0);
  a.Move(kContextRegister, kExitOldContext);
  a.Move(l0, o1);
  a.add(o4, o1, kJSArgcReceiverSlots);
  a.PushArguments(o2, o4, o5, ArgumentsElementType::kRaw);
  a.Move(g3, kExitOldContext);
  a.Push(g3);
  a.LoadRoot(g2, RootIndex::kUndefinedValue);
  a.LoadAddress(g1, a.AsMemOperand(IsolateFieldId::kIsolateAddress));
  a.Push(g3, kContextRegister, g2, g1);
  a.EnterExitFrame(g1, FC::getExtraSlotsCountFrom<ExitFrameConstants>(),
                   StackFrame::API_CALLBACK_EXIT);
  a.StoreU64(l0, MemOperand(fp, FC::kFCIArgcOffset));
  a.LoadAddress(o0, MemOperand(fp, FC::kFunctionCallbackInfoOffset));
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeFunctionCallbackInfo));
  a.StoreReturnAddressAndCall(g1, 1);
  a.LoadU64(o1, MemOperand(fp, FC::kReturnValueOffset));
  a.LeaveExitFrame(g1);
  a.Drop(FC::kFunctionCallbackInfoApiArgsLength + kJSArgcReceiverSlots);
  a.Drop(l0);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EntryRootStorage storage;
  using Entry = NativeResultPair (*)(uint8_t*, uint64_t, const uintptr_t*);
  for (int count : {0, 1, 7, 64, 255}) {
    std::vector<uintptr_t> values(count);
    for (int n = 0; n < count; ++n) values[n] = Smi::FromInt(100 + n).ptr();
    NativeResultPair result =
        code.entry<Entry>()(storage.root(), count, values.data());
    EXPECT_EQ(result.first, static_cast<uint64_t>(count));
    EXPECT_EQ(result.second, Smi::FromInt(42).ptr());
    EXPECT_EQ(ReadEntryField(storage.root(), IsolateFieldId::kCEntryFP), 0u);
  }
}

struct DirectCEntryProbe {
  uintptr_t function = 0;
  uintptr_t expected_pc = 0;
  uintptr_t replacement_pc = 0;
  uintptr_t observed_pc = 0;
  uintptr_t before_sp = 0;
  uintptr_t after_sp = 0;
  uintptr_t before_native_fp = 0;
  uintptr_t after_native_fp = 0;
  uintptr_t context = 0;
  uintptr_t path = 0;
};

bool CheckDirectCEntry(uint8_t* isolate, DirectCEntryProbe* probe) {
  uint8_t* root = isolate + kRootRegisterBias;
  if (!ExitCallMetadataIsValid(root)) return false;
  auto* frame = reinterpret_cast<const uintptr_t*>(
      ReadEntryField(root, IsolateFieldId::kCEntryFP));
  auto* pc = reinterpret_cast<uintptr_t*>(
      frame[ExitFrameConstants::kSPOffset / kSystemPointerSize] -
      kSystemPointerSize);
  probe->observed_pc = *pc;
  if (*pc != probe->expected_pc) return false;
  if (probe->replacement_pc != 0) *pc = probe->replacement_pc;
  WriteEntryField(root, IsolateFieldId::kContext, kExitNewContext);
  return true;
}

NativeResultPair ProbeDirectCEntry0() {
  asm volatile("flushw" ::: "memory");
  return {123, kExitPairSecond};
}

NativeResultPair ProbeDirectCEntry6(uint8_t* isolate, DirectCEntryProbe* probe,
                                    uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5) {
  if (!CheckDirectCEntry(isolate, probe)) return {UINT64_MAX, 6};
  return {a2 + a3 + a4 + a5, kExitPairSecond};
}

NativeResultPair ProbeDirectCEntry8(uint8_t* isolate, DirectCEntryProbe* probe,
                                    uint64_t a2, uint64_t a3, uint64_t a4,
                                    uint64_t a5, uint64_t a6, uint64_t a7) {
  if (!CheckDirectCEntry(isolate, probe)) return {UINT64_MAX, 8};
  return {a2 + a3 + a4 + a5 + a6 + a7, kExitPairSecond};
}

TEST(AssemblerSparc64BufferTest, NativeDirectCEntryArgumentsAndMovableReturn) {
  using Probe = DirectCEntryProbe;
  MacroAssembler last_argument(nullptr, MaybeAssemblerZone{},
                               AssemblerOptions{}, CodeObjectRequired::kNo);
  last_argument.flushw();
  last_argument.LoadU64(o0, MemOperand(sp, 249 * kSystemPointerSize));
  last_argument.Move(o1, kExitPairSecond);
  last_argument.Ret();
  ExecutableAssemblerBuffer last_argument_code;
  ASSERT_TRUE(last_argument_code.Initialize(last_argument));
  for (int count : {0, 6, 8, 256}) {
    for (bool padding : {false, true}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      a.EnterCFunctionFrame();
      a.set_has_frame(true);
      a.Move(kRootRegister, o0);
      a.Move(l0, o1);
      a.Move(l2, o2);
      a.Move(kContextRegister, kExitOldContext);
      if (padding) a.Push(g0);
      a.LoadStackAddress(g1);
      a.StoreU64(g1, MemOperand(l0, offsetof(Probe, before_sp)));
      a.StoreU64(kNativeFramePointer,
                 MemOperand(l0, offsetof(Probe, before_native_fp)));
      a.EnterExitFrame(g1, std::max(0, count - kRegisterPassedArguments),
                       StackFrame::EXIT);
      a.LoadAddress(o0, a.AsMemOperand(IsolateFieldId::kIsolateAddress));
      a.Move(o1, l0);
      for (int n = 2; n < kRegisterPassedArguments; ++n)
        a.Move(kCArgRegs[n], n);
      for (int n = kRegisterPassedArguments; n < count; ++n) {
        a.Move(g1, n);
        a.StoreU64(g1, MemOperand(sp, (n - kRegisterPassedArguments + 1) *
                                          kSystemPointerSize));
      }
      a.LoadU64(kDirectCEntryTargetRegister,
                MemOperand(l0, offsetof(Probe, function)));
      a.Move(kDirectCEntryArgumentCountRegister, l2);
      a.CallBuiltin(Builtin::kDirectCEntry);
      int ordinary_pc = a.pc_offset();
      a.Move(g3, 1);
      Label returned;
      a.Jump(&returned);
      int moved_pc = a.pc_offset();
      a.Move(g3, 2);
      a.bind(&returned);
      a.StoreU64(g3, MemOperand(l0, offsetof(Probe, path)));
      a.LeaveExitFrame(g1);
      a.StoreU64(kContextRegister, MemOperand(l0, offsetof(Probe, context)));
      a.LoadStackAddress(g1);
      a.StoreU64(g1, MemOperand(l0, offsetof(Probe, after_sp)));
      a.StoreU64(kNativeFramePointer,
                 MemOperand(l0, offsetof(Probe, after_native_fp)));
      a.LeaveFrame(StackFrame::MANUAL);
      a.RetCFunction();
      int direct_entry = a.pc_offset();
      a.DirectCEntry();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      EntryRootStorage storage;
      using Entry = NativeResultPair (*)(uint8_t*, Probe*, uintptr_t);
      auto entry = code.entry<Entry>();
      base::WriteUnalignedValue<uintptr_t>(
          reinterpret_cast<Address>(storage.root()) +
              IsolateData::BuiltinEntrySlotOffset(Builtin::kDirectCEntry),
          reinterpret_cast<uintptr_t>(entry) + direct_entry);
      for (bool move_return : {false, true}) {
        if (move_return && count != 6 && count != 8) continue;
        Probe probe;
        if (count == 0)
          probe.function = reinterpret_cast<uintptr_t>(&ProbeDirectCEntry0);
        if (count == 6)
          probe.function = reinterpret_cast<uintptr_t>(&ProbeDirectCEntry6);
        if (count == 8)
          probe.function = reinterpret_cast<uintptr_t>(&ProbeDirectCEntry8);
        if (count == 256)
          probe.function = reinterpret_cast<uintptr_t>(
              last_argument_code.entry<void (*)()>());
        probe.expected_pc = reinterpret_cast<uintptr_t>(entry) + ordinary_pc;
        if (move_return)
          probe.replacement_pc = reinterpret_cast<uintptr_t>(entry) + moved_pc;
        NativeResultPair result = entry(storage.root(), &probe, count);
        uint64_t expected = count == 0   ? 123
                            : count == 6 ? 14
                            : count == 8 ? 27
                                         : 255;
        EXPECT_EQ(result.first, expected) << "argc=" << count;
        EXPECT_EQ(result.second, kExitPairSecond);
        EXPECT_EQ(probe.path, move_return ? 2u : 1u);
        EXPECT_EQ(probe.before_sp, probe.after_sp);
        EXPECT_EQ(probe.before_native_fp, probe.after_native_fp);
        EXPECT_EQ(probe.context,
                  count == 6 || count == 8 ? kExitNewContext : kExitOldContext);
        if (count == 6 || count == 8) {
          EXPECT_EQ(probe.observed_pc, probe.expected_pc);
        }
        EXPECT_EQ(ReadEntryField(storage.root(), IsolateFieldId::kCEntryFP),
                  0u);
      }
    }
  }
}

NativeResultPair ProbeExitFrameCall(uint64_t argc, const uint64_t* argv,
                                    uint8_t* isolate) {
  uint8_t* root = isolate + kRootRegisterBias;
  if (!ExitCallMetadataIsValid(root)) return {UINT64_MAX, 1};
  auto* frame = reinterpret_cast<const uintptr_t*>(
      ReadEntryField(root, IsolateFieldId::kCEntryFP));
  uintptr_t saved_sp =
      frame[ExitFrameConstants::kSPOffset / kSystemPointerSize];
  uintptr_t pc =
      *reinterpret_cast<const uintptr_t*>(saved_sp - kSystemPointerSize);
  uintptr_t actual_return = reinterpret_cast<uintptr_t>(
      __builtin_extract_return_addr(__builtin_return_address(0)));
  if (pc != actual_return) return {UINT64_MAX, 2};
  intptr_t extra =
      (reinterpret_cast<uintptr_t>(frame) - saved_sp) / kSystemPointerSize - 2;
  const auto* slots = reinterpret_cast<const uintptr_t*>(saved_sp);
  for (intptr_t n = 0; n < extra; ++n) {
    if (slots[n] != 0xabc000u + static_cast<uintptr_t>(n))
      return {UINT64_MAX, 3};
  }
  WriteEntryField(root, IsolateFieldId::kContext, kExitNewContext);
  uint64_t sum = argc;
  for (uint64_t n = 0; n < argc; ++n) sum += argv[-static_cast<intptr_t>(n)];
  return {sum, kExitPairSecond};
}

TEST(AssemblerSparc64BufferTest, NativeExitFrameMetadataAndPair) {
  const StackFrame::Type types[] = {StackFrame::EXIT, StackFrame::BUILTIN_EXIT,
                                    StackFrame::API_NAMED_ACCESSOR_EXIT,
                                    StackFrame::API_INDEXED_ACCESSOR_EXIT,
                                    StackFrame::API_CALLBACK_EXIT};
  for (StackFrame::Type type : types) {
    for (int space : {0, 3}) {
      for (bool padding : {false, true}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        a.EnterCFunctionFrame();
        a.set_has_frame(true);
        a.mov(kRootRegister, o0);
        a.mov(l0, o1);
        a.mov(l1, o2);
        a.LoadLiteral(kContextRegister, kExitOldContext);
        if (padding) a.Push(g0);
        a.LoadStackAddress(g1);
        a.StoreU64(g1, MemOperand(i3, 8));
        a.EnterExitFrame(g1, space, type);
        a.LoadU64(g1, MemOperand(fp, ExitFrameConstants::kFrameTypeOffset));
        a.StoreU64(g1, MemOperand(i3, 24));
        a.LoadU64(g1, MemOperand(fp));
        a.StoreU64(g1, MemOperand(i3, 32));
        for (int n = 0; n < space; ++n) {
          a.Move(g1, 0xabc000 + n);
          a.StoreU64(g1, MemOperand(sp, (n + 1) * kSystemPointerSize));
        }
        a.Move(o0, l1);
        a.Move(o1, l0);
        a.LoadAddress(o2, a.AsMemOperand(IsolateFieldId::kIsolateAddress));
        a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeExitFrameCall));
        a.StoreReturnAddressAndCall(g1, 3);
        a.LeaveExitFrame(g1);
        a.StoreU64(kContextRegister, MemOperand(i3));
        a.LoadStackAddress(g1);
        a.StoreU64(g1, MemOperand(i3, 16));
        a.LeaveFrame(StackFrame::MANUAL);
        a.RetCFunction();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        EntryRootStorage storage;
        uint64_t args[] = {1, 2, 4, 8};
        uint64_t state[5]{};
        using Entry = NativeResultPair (*)(uint8_t*, const uint64_t*, uint64_t,
                                           uint64_t*);
        NativeResultPair result =
            code.entry<Entry>()(storage.root(), args + 3, 4, state);
        EXPECT_EQ(result.first, 19u);
        EXPECT_EQ(result.second, kExitPairSecond);
        EXPECT_EQ(state[0], kExitNewContext);
        EXPECT_EQ(state[1], state[2]);
        EXPECT_EQ(state[3],
                  static_cast<uintptr_t>(StackFrame::TypeToMarker(type)));
        EXPECT_EQ(state[4], state[1] + (padding ? kSystemPointerSize : 0));
        EXPECT_EQ(ReadEntryField(storage.root(), IsolateFieldId::kCEntryFP),
                  0u);
      }
    }
  }
}

uint64_t ProbeExitCStackArgs(uint8_t* isolate, uint64_t a, uint64_t b,
                             uint64_t c, uint64_t d, uint64_t e, uint64_t f,
                             uint64_t g) {
  if (!ExitCallMetadataIsValid(isolate + kRootRegisterBias)) return UINT64_MAX;
  return a + b + c + d + e + f + g;
}

TEST(AssemblerSparc64BufferTest, NativeExitFrameCStackArguments) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.set_has_frame(true);
  a.mov(kRootRegister, o0);
  a.LoadLiteral(kContextRegister, kExitOldContext);
  a.EnterExitFrame(g1, 2, StackFrame::EXIT);
  a.LoadAddress(o0, a.AsMemOperand(IsolateFieldId::kIsolateAddress));
  for (int n = 1; n < 6; ++n) a.mov(kCArgRegs[n], 1 << (n - 1));
  a.mov(g1, 32);
  a.StoreU64(g1, MemOperand(sp, 8));
  a.mov(g1, 64);
  a.StoreU64(g1, MemOperand(sp, 16));
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&ProbeExitCStackArgs));
  a.StoreReturnAddressAndCall(g1, 8);
  a.LeaveExitFrame(g1);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EntryRootStorage storage;
  EXPECT_EQ(code.entry<uint64_t (*)(uint8_t*)>()(storage.root()), 127u);
}

TEST(AssemblerSparc64BufferTest, NativeJumpToPendingHandler) {
  struct HandlerMode {
    bool has_context;
    bool fp_only;
    bool zero_fp;
  };
  for (HandlerMode mode :
       {HandlerMode{false, false, false}, HandlerMode{false, true, false},
        HandlerMode{true, false, false}, HandlerMode{true, true, false},
        HandlerMode{false, false, true}}) {
    bool has_context = mode.has_context;
    bool fp_only = mode.fp_only;
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterCFunctionFrame();
    a.mov(kRootRegister, o0);
    a.LoadLiteral(kContextRegister, kExitOldContext);
    a.Push(kContextRegister);
    a.LoadStackAddress(l0);
    a.mov(l1, fp);
    a.StoreU64(mode.zero_fp ? g0 : fp,
               a.AsMemOperand(IsolateFieldId::kPendingHandlerFP));
    a.StoreU64(fp_only ? g0 : l0,
               a.AsMemOperand(IsolateFieldId::kPendingHandlerSP));
    a.LoadLiteral(g1, has_context ? kExitNewContext : 0);
    a.StoreU64(g1, a.AsMemOperand(IsolateFieldId::kPendingHandlerContext));
    a.StoreU64(fp, a.AsMemOperand(IsolateFieldId::kCEntryFP));
    a.mov(o0, 42);
    a.JumpToPendingHandler();
    int handler_offset = a.pc_offset();
    a.LoadStackAddress(g1);
    a.sub(o2, g1, l0);
    a.sub(o3, fp, mode.zero_fp ? g0 : l1);
    a.or_(o2, o2, o3);
    a.LoadU64(o3, a.AsMemOperand(IsolateFieldId::kCEntryFP));
    a.or_(o2, o2, o3);
    Label valid;
    a.cmp(o2, 0);
    a.b(equal, ConditionCodes::kXcc, &valid);
    a.nop();
    a.mov(o0, -1);
    a.bind(&valid);
    a.LoadU64(o1, MemOperand(l1, StandardFrameConstants::kContextOffset));
    a.mov(fp, l1);
    a.LeaveFrame(StackFrame::MANUAL);
    a.RetCFunction();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EntryRootStorage storage;
    auto entry = code.entry<NativeResultPair (*)(uint8_t*)>();
    WriteEntryField(storage.root(), IsolateFieldId::kPendingHandlerEntrypoint,
                    reinterpret_cast<uintptr_t>(entry) + handler_offset);
    NativeResultPair result = entry(storage.root());
    EXPECT_EQ(result.first, 42u);
    EXPECT_EQ(result.second, has_context ? kExitNewContext : kExitOldContext);
  }
}

constexpr uint64_t kCopiedArgsReceiver = UINT64_C(0x123456789abcdef1);

uint64_t CheckCopiedArguments(const uint64_t* stack, const uint64_t* values,
                              uint64_t argc, uint64_t unchanged) {
  asm volatile("flushw" ::: "memory");
  if (unchanged != 1 || stack[0] != kCopiedArgsReceiver) return UINT64_MAX;
  for (uint64_t n = 1; n < argc; ++n) {
    if (stack[n] != values[n - 1]) return UINT64_MAX;
  }
  return argc - 1;
}

TEST(AssemblerSparc64BufferTest, NativePushRawAndHandleArguments) {
  for (ArgumentsElementType type :
       {ArgumentsElementType::kRaw, ArgumentsElementType::kHandle}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterCFunctionFrame();
    a.set_has_frame(true);
    a.Move(l1, o0);
    a.Move(l2, o1);
    a.Move(l3, o2);
    a.PushArguments(o0, o1, l0, type);
    Label changed, ready;
    a.cmp(o0, l1);
    a.b(not_equal, ConditionCodes::kXcc, &changed);
    a.nop();
    a.cmp(o1, l2);
    a.b(not_equal, ConditionCodes::kXcc, &changed);
    a.nop();
    a.Move(l4, 1);
    a.Jump(&ready);
    a.bind(&changed);
    a.Move(l4, 0);
    a.bind(&ready);
    a.LoadLiteral(g1, kCopiedArgsReceiver);
    a.Push(g1);
    a.LoadStackAddress(o0);
    a.Move(o1, l3);
    a.Move(o2, l2);
    a.Move(o3, l4);
    a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&CheckCopiedArguments));
    a.CallCFunction(g1, 4, SetIsolateDataSlots::kNo);
    a.LeaveFrame(StackFrame::MANUAL);
    a.RetCFunction();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    using Entry = uint64_t (*)(const void*, uint64_t, const uint64_t*);
    for (size_t count : {0, 1, 2, 7, 64, 255}) {
      std::vector<uint64_t> values(count);
      std::vector<const uint64_t*> handles(count);
      for (size_t n = 0; n < count; ++n) {
        values[n] = UINT64_C(0xfedcba9800000000) + n;
        handles[n] = &values[n];
      }
      const void* array = type == ArgumentsElementType::kHandle
                              ? static_cast<const void*>(handles.data())
                              : static_cast<const void*>(values.data());
      EXPECT_EQ(code.entry<Entry>()(array, count + 1, values.data()), count);
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeStackOverflowCheckSpillArea) {
  for (Register count : {o1, g2}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterCFunctionFrame();
    a.Move(kRootRegister, o0);
    a.Move(l0, o1);
    a.Move(count, o1);
    a.add(g4, sp, kSparcStackBias);
    a.sub(g4, g4, o2);
    a.StoreU64(g4,
               MemOperand(kRootRegister, IsolateData::real_jslimit_offset()));
    Label overflow, ready, unchanged;
    a.StackOverflowCheck(count, g3, &overflow);
    a.mov(o0, 0);
    a.Jump(&ready);
    a.bind(&overflow);
    a.mov(o0, 1);
    a.bind(&ready);
    a.cmp(count, l0);
    a.b(equal, ConditionCodes::kXcc, &unchanged);
    a.nop();
    a.mov(o0, -1);
    a.bind(&unchanged);
    a.LeaveFrame(StackFrame::MANUAL);
    a.RetCFunction();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EntryRootStorage storage;
    using Entry = uint64_t (*)(uint8_t*, uint64_t, intptr_t);
    for (int args : {0, 1, 4, 8, 10}) {
      for (int available : {-8, 0, 7, 8, 9, 32, 64, 65, 128}) {
        EXPECT_EQ(code.entry<Entry>()(storage.root(), args, available),
                  static_cast<uint64_t>(available <= args * kSystemPointerSize))
            << "args=" << args << " available=" << available;
      }
    }
  }
}

constexpr uint64_t kPaddingUndefined = UINT64_C(0x1234567800000001);

uint64_t CheckPaddedArguments(const uint64_t* stack, const uint64_t* values,
                              uint64_t actual, uint64_t expected,
                              uint64_t unchanged, uintptr_t original_sp) {
  asm volatile("flushw" ::: "memory");
  uint64_t total = std::max(actual, expected);
  if (!unchanged ||
      original_sp - reinterpret_cast<uintptr_t>(stack) != total * 8)
    return UINT64_MAX;
  for (uint64_t n = 0; n < total; ++n) {
    if (stack[n] != (n < actual ? values[n] : kPaddingUndefined))
      return UINT64_MAX;
  }
  return total;
}

TEST(AssemblerSparc64BufferTest, NativeArgumentAdaptation) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.set_has_frame(true);
  a.Move(kRootRegister, o0);
  a.Move(l0, o2);
  a.Move(l1, o3);
  a.Move(l2, o1);
  a.LoadStackAddress(l4);
  a.add(o1, o1, kSystemPointerSize);
  a.PushArguments(o1, o2, l3, ArgumentsElementType::kRaw);
  a.LoadU64(g1, MemOperand(l2));
  a.Push(g1);
  a.Move(o0, l0);
  a.Move(o1, l2);
  a.Move(o2, l1);
  a.Move(o3, 1234);
  Label overflow, changed, ready;
  a.AdaptArguments(o2, o0, &overflow);
  a.cmp(o0, l0);
  a.b(not_equal, ConditionCodes::kXcc, &changed);
  a.nop();
  a.cmp(o1, l2);
  a.b(not_equal, ConditionCodes::kXcc, &changed);
  a.nop();
  a.cmp(o3, 1234);
  a.b(not_equal, ConditionCodes::kXcc, &changed);
  a.nop();
  a.Move(o4, 1);
  a.Jump(&ready);
  a.bind(&changed);
  a.Move(o4, 0);
  a.bind(&ready);
  a.LoadStackAddress(o0);
  a.Move(o1, l2);
  a.Move(o2, l0);
  a.Move(o3, l1);
  a.Move(o5, l4);
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&CheckPaddedArguments));
  a.CallCFunction(g1, 6, SetIsolateDataSlots::kNo);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  a.bind(&overflow);
  a.Move(o0, -1);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EntryRootStorage storage;
  base::WriteUnalignedValue<uint64_t>(
      reinterpret_cast<Address>(storage.root()) +
          IsolateData::root_slot_offset(RootIndex::kUndefinedValue),
      kPaddingUndefined);
  using Entry = uint64_t (*)(uint8_t*, const uint64_t*, uint64_t, uint64_t);
  for (uint64_t actual : {1, 2, 7, 64, 255}) {
    std::vector<uint64_t> values(actual);
    for (size_t n = 0; n < actual; ++n)
      values[n] = UINT64_C(0xfedcba9800000001) + 8 * n;
    for (uint64_t expected : {0, 1, 2, 5, 64, 255}) {
      EXPECT_EQ(
          code.entry<Entry>()(storage.root(), values.data(), actual, expected),
          std::max(actual, expected))
          << "actual=" << actual << " expected=" << expected;
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeJSDispatchTableJump) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.Move(kRootRegister, o0);
  a.Move(o0, o2);
  a.JumpJSFunction(o1);
  int target = a.pc_offset();
  a.Move(o1, o3);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  EntryRootStorage storage;
  std::vector<uint64_t> object(32);
  std::vector<uint64_t> table(64 * kJSDispatchTableEntrySize /
                              sizeof(uint64_t));
  WriteEntryField(storage.root(), IsolateFieldId::kJSDispatchTable,
                  reinterpret_cast<uintptr_t>(table.data()));
  using Entry = NativeResultPair (*)(uint8_t*, uintptr_t, uint64_t, uint64_t);
  auto entry = code.entry<Entry>();
  for (uint32_t index : {1, 3, 31, 63}) {
    base::WriteUnalignedValue<uint32_t>(
        reinterpret_cast<Address>(object.data()) +
            JSFunction::kDispatchHandleOffset,
        index << kJSDispatchHandleShift);
    base::WriteUnalignedValue<uintptr_t>(
        reinterpret_cast<Address>(table.data()) +
            index * kJSDispatchTableEntrySize +
            JSDispatchEntry::kEntrypointOffset,
        reinterpret_cast<uintptr_t>(entry) + target);
    NativeResultPair result =
        entry(storage.root(),
              reinterpret_cast<uintptr_t>(object.data()) + kHeapObjectTag,
              index + 100, UINT64_C(0x123456789abcdef1));
    EXPECT_EQ(result.first, index + 100u);
    EXPECT_EQ(result.second, UINT64_C(0x123456789abcdef1));
  }
}

struct BuiltinContinuationProbe {
  uintptr_t return_pc = 0;
  uintptr_t builtin_index = 0;
  uintptr_t before_fp = 0;
  uintptr_t restored_fp = 0;
  uintptr_t restored_pc = 0;
  uintptr_t target = 0;
  std::array<uintptr_t, Register::kNumRegisters> saved{};
  std::array<uintptr_t, Register::kNumRegisters> restored{};
  std::array<uintptr_t, 64> arguments{};
};

TEST(AssemblerSparc64BufferTest, NativeBuiltinContinuationRestore) {
  using Probe = BuiltinContinuationProbe;
  const RegisterConfiguration* config = RegisterConfiguration::Default();
  constexpr uintptr_t lazy_result = UINT64_C(0xfedcba9876543211);
  constexpr uintptr_t argument_base = UINT64_C(0x1234567800000001);
  for (bool javascript : {false, true}) {
    for (bool with_result : {false, true}) {
      for (int argc : {1, 2, 7, 64}) {
        MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                         CodeObjectRequired::kNo);
        a.EnterCFunctionFrame();
        a.Move(kRootRegister, o0);
        a.StoreU64(fp, MemOperand(o1, offsetof(Probe, before_fp)));
        a.Push(o1);
        for (int n = argc - 1; n >= 0; --n) {
          a.Move(g1, argument_base + n * 16);
          a.Push(g1);
        }
        a.LoadU64(g1, MemOperand(o1, offsetof(Probe, return_pc)));
        a.sub(o7, g1, kSparcReturnAddressOffset);
        a.EnterFrame(javascript ? StackFrame::JAVASCRIPT_BUILTIN_CONTINUATION
                                : StackFrame::BUILTIN_CONTINUATION);
        a.LoadU64(g1, MemOperand(o1, offsetof(Probe, builtin_index)));
        a.Push(g0, g0, g0, g1);
        for (int n = 0; n < config->num_allocatable_general_registers(); ++n) {
          int reg = config->GetAllocatableGeneralCode(n);
          a.LoadU64(g1, MemOperand(o1, offsetof(Probe, saved) +
                                           reg * kSystemPointerSize));
          a.Push(g1);
        }
        a.Move(o0, lazy_result);
        a.ContinueToBuiltin(javascript, with_result);
        Label capture;
        int first_target = a.pc_offset();
        a.Move(g3, 1);
        a.Jump(&capture);
        int second_target = a.pc_offset();
        a.Move(g3, 2);
        a.bind(&capture);
        a.LoadU64(g4, MemOperand(fp, -kSystemPointerSize));
        a.StoreU64(g3, MemOperand(g4, offsetof(Probe, target)));
        for (int n = 0; n < config->num_allocatable_general_registers(); ++n) {
          int reg = config->GetAllocatableGeneralCode(n);
          a.StoreU64(Register::from_code(reg),
                     MemOperand(g4, offsetof(Probe, restored) +
                                        reg * kSystemPointerSize));
        }
        a.StoreU64(fp, MemOperand(g4, offsetof(Probe, restored_fp)));
        a.add(g1, o7, kSparcReturnAddressOffset);
        a.StoreU64(g1, MemOperand(g4, offsetof(Probe, restored_pc)));
        for (int n = 0; n < argc; ++n) {
          a.LoadU64(g1, MemOperand(sp, n * kSystemPointerSize));
          a.StoreU64(g1, MemOperand(g4, offsetof(Probe, arguments) +
                                            n * kSystemPointerSize));
        }
        a.Ret();
        int resume = a.pc_offset();
        a.Move(o0, 1);
        a.LeaveFrame(StackFrame::MANUAL);
        a.RetCFunction();
        ExecutableAssemblerBuffer code;
        ASSERT_TRUE(code.Initialize(a));
        using Entry = uint64_t (*)(uint8_t*, Probe*);
        auto entry = code.entry<Entry>();
        EntryRootStorage storage;
        for (auto pair : {std::pair{Builtin::kIllegal, first_target},
                          std::pair{Builtin::kToObject, second_target}}) {
          base::WriteUnalignedValue<uintptr_t>(
              reinterpret_cast<Address>(storage.root()) +
                  IsolateData::BuiltinEntrySlotOffset(pair.first),
              reinterpret_cast<uintptr_t>(entry) + pair.second);
        }
        for (Builtin builtin : {Builtin::kIllegal, Builtin::kToObject}) {
          Probe probe;
          probe.return_pc = reinterpret_cast<uintptr_t>(entry) + resume;
          probe.builtin_index = Smi::FromInt(static_cast<int>(builtin)).ptr();
          for (int n = 0; n < config->num_allocatable_general_registers();
               ++n) {
            int reg = config->GetAllocatableGeneralCode(n);
            probe.saved[reg] = UINT64_C(0x1234000000000001) + reg * 16;
          }
          if (javascript) probe.saved[o0.code()] = Smi::FromInt(argc).ptr();
          ASSERT_EQ(entry(storage.root(), &probe), 1u);
          EXPECT_EQ(probe.target, builtin == Builtin::kIllegal ? 1u : 2u);
          EXPECT_EQ(probe.restored_fp, probe.before_fp);
          EXPECT_EQ(probe.restored_pc, probe.return_pc);
          for (int n = 0; n < config->num_allocatable_general_registers();
               ++n) {
            int reg = config->GetAllocatableGeneralCode(n);
            uintptr_t expected = javascript && reg == o0.code()
                                     ? static_cast<uintptr_t>(argc)
                                     : probe.saved[reg];
            EXPECT_EQ(probe.restored[reg], expected) << "register=" << reg;
          }
          for (int n = 0; n < argc; ++n) {
            bool result_slot =
                with_result && (javascript ? n == argc - 1 : n == 0);
            EXPECT_EQ(probe.arguments[n],
                      result_slot ? lazy_result : argument_base + n * 16);
          }
        }
      }
    }
  }
}

struct WriteBarrierProbe {
  uintptr_t calls = 0;
  uintptr_t object = 0;
  uintptr_t slot = 0;
  uintptr_t value = 0;
};

TEST(AssemblerSparc64BufferTest, NativeWriteBarrierFilterAndRegisters) {
  constexpr int field_offset = 2 * kTaggedSize;
  for (int kind = 0; kind < 3; ++kind) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    a.EnterCFunctionFrame();
    a.Move(kRootRegister, o0);
    a.Move(l0, o1);
    a.Move(l1, o2);
    a.Move(l2, o3);
    a.LoadStackAddress(l4);
    a.Move(o4, 444);
    a.Move(o5, 555);
    a.Move(kContextRegister, 1234);
    a.StoreU64(o2, FieldMemOperand(o1, field_offset));
    if (kind == 0) {
      a.RecordWriteField(o1, field_offset, o2, SaveFPRegsMode::kIgnore);
    } else {
      a.LoadAddress(l3, FieldMemOperand(o1, field_offset));
      a.RecordWrite(o1, l3, o2, SaveFPRegsMode::kIgnore, kind == 2);
    }
    Label changed, done;
    for (auto pair : {std::pair{o0, kRootRegister}, std::pair{o1, l0},
                      std::pair{o2, l1}, std::pair{o3, l2}}) {
      a.cmp(pair.first, pair.second);
      a.b(not_equal, ConditionCodes::kXcc, &changed);
      a.nop();
    }
    for (auto pair : {std::pair{o4, 444}, std::pair{o5, 555},
                      std::pair{kContextRegister, 1234}}) {
      a.cmp(pair.first, pair.second);
      a.b(not_equal, ConditionCodes::kXcc, &changed);
      a.nop();
    }
    a.LoadStackAddress(g1);
    a.cmp(g1, l4);
    a.b(not_equal, ConditionCodes::kXcc, &changed);
    a.nop();
    a.Move(o0, 1);
    a.Jump(&done);
    a.bind(&changed);
    a.Move(o0, 0);
    a.bind(&done);
    a.LeaveFrame(StackFrame::MANUAL);
    a.RetCFunction();
    int stub_offset = a.pc_offset();
    a.LoadU64(g1, MemOperand(l2, offsetof(WriteBarrierProbe, calls)));
    a.add(g1, g1, 1);
    a.StoreU64(g1, MemOperand(l2, offsetof(WriteBarrierProbe, calls)));
    a.StoreU64(o1, MemOperand(l2, offsetof(WriteBarrierProbe, object)));
    a.StoreU64(o5, MemOperand(l2, offsetof(WriteBarrierProbe, slot)));
    a.LoadU64(g1, MemOperand(o5));
    a.StoreU64(g1, MemOperand(l2, offsetof(WriteBarrierProbe, value)));
    for (Register reg : WriteBarrierDescriptor::ComputeSavedRegisters(o1))
      a.Move(reg, 100 + reg.code());
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    using Entry =
        uint64_t (*)(uint8_t*, uintptr_t, uintptr_t, WriteBarrierProbe*);
    auto entry = code.entry<Entry>();
    EntryRootStorage storage;
    base::WriteUnalignedValue<uintptr_t>(
        reinterpret_cast<Address>(storage.root()) +
            IsolateData::BuiltinEntrySlotOffset(
                kind == 2 ? Builtin::kEphemeronKeyBarrierIgnoreFP
                          : Builtin::kRecordWriteIgnoreFP),
        reinterpret_cast<uintptr_t>(entry) + stub_offset);
    constexpr size_t page_size = size_t{1} << kPageSizeBits;
    std::vector<uint8_t> pages(3 * page_size);
    uintptr_t object_page =
        (reinterpret_cast<uintptr_t>(pages.data()) + page_size - 1) &
        ~(uintptr_t{page_size} - 1);
    uintptr_t value_page = object_page + page_size;
    uintptr_t object = object_page + 512 + kHeapObjectTag;
    uintptr_t heap_value = value_page + 512 + kHeapObjectTag;
    uintptr_t from_mask = static_cast<uintptr_t>(
        MemoryChunk::kPointersFromHereAreInterestingMask);
    uintptr_t to_mask =
        static_cast<uintptr_t>(MemoryChunk::kPointersToHereAreInterestingMask);
    for (uintptr_t object_flags : {uintptr_t{0}, from_mask}) {
      for (uintptr_t value_flags : {uintptr_t{0}, to_mask}) {
        new (reinterpret_cast<void*>(object_page))
            MemoryChunk(MemoryChunk::MainThreadFlags(
                            static_cast<MemoryChunk::Flag>(object_flags)),
                        nullptr);
        new (reinterpret_cast<void*>(value_page))
            MemoryChunk(MemoryChunk::MainThreadFlags(
                            static_cast<MemoryChunk::Flag>(value_flags)),
                        nullptr);
        for (uintptr_t value :
             {heap_value, Smi::zero().ptr(), Smi::FromInt(42).ptr()}) {
          WriteBarrierProbe probe;
          EXPECT_EQ(entry(storage.root(), object, value, &probe), 1u);
          bool expected = value == heap_value && object_flags && value_flags;
          EXPECT_EQ(probe.calls, static_cast<uintptr_t>(expected));
          if (expected) {
            EXPECT_EQ(probe.object, object);
            EXPECT_EQ(probe.slot, object - kHeapObjectTag + field_offset);
            EXPECT_EQ(probe.value, value);
          }
        }
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeDoubleToInt32) {
  MacroAssembler raw(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
  raw.EnterCFunctionFrame();
  Label builtin;
  raw.Move(o1, o0);
  raw.Push(o0);
  raw.call(&builtin);
  raw.nop();
  raw.Pop(o0);
  raw.LeaveFrame(StackFrame::MANUAL);
  raw.RetCFunction();
  raw.bind(&builtin);
  raw.TruncateDoubleToIOnStack();
  raw.Ret();
  ExecutableAssemblerBuffer raw_code;
  ASSERT_TRUE(raw_code.Initialize(raw));
  MacroAssembler fp(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                    CodeObjectRequired::kNo);
  fp.EnterCFunctionFrame();
  fp.TruncateDoubleToI(o0, d0);
  fp.MovDoubleToInt64(o1, d0);
  fp.LeaveFrame(StackFrame::MANUAL);
  fp.RetCFunction();
  ExecutableAssemblerBuffer fp_code;
  ASSERT_TRUE(fp_code.Initialize(fp));
  auto raw_entry = raw_code.entry<NativeResultPair (*)(uint64_t)>();
  auto fp_entry = fp_code.entry<NativeResultPair (*)(double)>();
  auto check = [&](uint64_t bits) {
    double value = base::bit_cast<double>(bits);
    uint64_t expected =
        static_cast<uint64_t>(static_cast<int64_t>(DoubleToInt32(value)));
    NativeResultPair raw_result = raw_entry(bits);
    NativeResultPair fp_result = fp_entry(value);
    EXPECT_EQ(raw_result.first, expected) << "bits=" << std::hex << bits;
    EXPECT_EQ(raw_result.second, bits);
    EXPECT_EQ(fp_result.first, expected) << "bits=" << std::hex << bits;
    EXPECT_EQ(fp_result.second, bits);
  };
  for (uint64_t sign : {UINT64_C(0), UINT64_C(1) << 63}) {
    for (uint64_t exponent = 0; exponent < 2048; ++exponent) {
      for (uint64_t fraction : {UINT64_C(0), UINT64_C(1), UINT64_C(1) << 51,
                                (UINT64_C(1) << 52) - 1}) {
        check(sign | (exponent << 52) | fraction);
      }
    }
  }
  for (double value :
       {0.25, -0.25, 1.75, -1.75, 42.5, -42.5, 2147483647.75, 2147483648.75,
        -2147483648.75, 4294967295.75, 4294967296.75, -4294967296.75}) {
    check(base::bit_cast<uint64_t>(value));
  }
  uint64_t random_bits = UINT64_C(0x9e3779b97f4a7c15);
  for (int n = 0; n < 20000; ++n) {
    random_bits ^= random_bits << 13;
    random_bits ^= random_bits >> 7;
    random_bits ^= random_bits << 17;
    check(random_bits);
  }
}

uint64_t CheckShiftedArguments(const uint64_t* stack, const uint64_t* values,
                               uint64_t actual, uint64_t extra, uint64_t total,
                               const uint64_t* first_free) {
  asm volatile("flushw" ::: "memory");
  if (total != actual + extra || first_free != stack + actual)
    return UINT64_MAX;
  for (uint64_t n = 0; n < total; ++n)
    if (stack[n] != values[n]) return UINT64_MAX;
  return total;
}

TEST(AssemblerSparc64BufferTest, NativeAllocateAndShiftArguments) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.set_has_frame(true);
  a.Move(l0, o0);
  a.Move(l1, o1);
  a.Move(l2, o2);
  a.LoadStackAddress(l4);
  a.add(o3, o1, 1);
  a.PushArguments(o0, o3, o5, ArgumentsElementType::kRaw);
  a.Move(o0, l1);
  a.Move(o1, 111);
  a.Move(o2, 222);
  a.Move(o3, 333);
  a.Move(o4, l2);
  a.AllocateSpaceAndShiftArguments(o4, o0, o5);
  a.Move(l3, o5);
  Label changed, checked;
  for (auto pair :
       {std::pair{o1, 111}, std::pair{o2, 222}, std::pair{o3, 333}}) {
    a.cmp(pair.first, pair.second);
    a.b(not_equal, ConditionCodes::kXcc, &changed);
    a.nop();
  }
  a.cmp(o4, l2);
  a.b(not_equal, ConditionCodes::kXcc, &changed);
  a.nop();
  a.LoadStackAddress(g1);
  a.sllx(g2, o0, kSystemPointerSizeLog2);
  a.add(g1, g1, g2);
  a.cmp(g1, l4);
  a.b(equal, ConditionCodes::kXcc, &checked);
  a.nop();
  a.bind(&changed);
  a.Move(o0, -1);
  a.bind(&checked);
  a.sllx(g1, l1, kSystemPointerSizeLog2);
  a.add(g1, l0, g1);
  a.Move(g2, l3);
  a.Move(g3, l2);
  Label copy, done;
  a.cmp(g3, 0);
  a.b(equal, ConditionCodes::kXcc, &done);
  a.nop();
  a.bind(&copy);
  a.LoadU64(g4, MemOperand(g1));
  a.StoreU64(g4, MemOperand(g2));
  a.add(g1, g1, kSystemPointerSize);
  a.add(g2, g2, kSystemPointerSize);
  a.sub(g3, g3, 1);
  a.cmp(g3, 0);
  a.b(not_equal, ConditionCodes::kXcc, &copy);
  a.nop();
  a.bind(&done);
  a.Move(o4, o0);
  a.LoadStackAddress(o0);
  a.Move(o1, l0);
  a.Move(o2, l1);
  a.Move(o3, l2);
  a.Move(o5, l3);
  a.LoadLiteral(g1, reinterpret_cast<uintptr_t>(&CheckShiftedArguments));
  a.CallCFunction(g1, 6, SetIsolateDataSlots::kNo);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  using Entry = uint64_t (*)(const uint64_t*, uint64_t, uint64_t);
  for (uint64_t actual : {1, 2, 7, 64, 255}) {
    for (uint64_t extra : {0, 1, 2, 7, 64, 255}) {
      std::vector<uint64_t> values(actual + extra);
      for (size_t n = 0; n < values.size(); ++n)
        values[n] = UINT64_C(0xfedcba9800000001) + n * 8;
      EXPECT_EQ(code.entry<Entry>()(values.data(), actual, extra),
                actual + extra)
          << "actual=" << actual << " extra=" << extra;
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeInterpreterFrameDispatchAndReturn) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.Move(kInterpreterDispatchTableRegister, o2);
  a.Move(kContextRegister, 0);
  a.PushStandardFrame(g0);
  a.Push(o0, o1, g0);
  a.Move(kInterpreterAccumulatorRegister, o3);
  a.sub(o7, o4, kSparcReturnAddressOffset);
  a.DispatchInterpreterBytecode();
  int handler = a.pc_offset();
  a.Ret();
  int continuation = a.pc_offset();
  a.Move(o1, kInterpreterBytecodeOffsetRegister);
  a.LeaveFrame(StackFrame::INTERPRETED);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  using Entry = NativeResultPair (*)(const uint8_t*, uintptr_t,
                                     const uintptr_t*, uint64_t, uintptr_t);
  auto entry = code.entry<Entry>();
  std::array<uintptr_t, 256> table{};
  for (interpreter::Bytecode bytecode :
       {interpreter::Bytecode::kLdaZero, interpreter::Bytecode::kLdaSmi,
        interpreter::Bytecode::kReturn}) {
    table[static_cast<uint8_t>(bytecode)] =
        reinterpret_cast<uintptr_t>(entry) + handler;
    for (int offset : {7, 15, 32}) {
      std::array<uint8_t, 64> bytes{};
      bytes[offset] = static_cast<uint8_t>(bytecode);
      constexpr uint64_t accumulator = UINT64_C(0xfedcba9876543211);
      NativeResultPair result =
          entry(bytes.data(), Smi::FromInt(offset).ptr(), table.data(),
                accumulator, reinterpret_cast<uintptr_t>(entry) + continuation);
      EXPECT_EQ(result.first, accumulator);
      EXPECT_EQ(result.second, static_cast<uint64_t>(offset));
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeBytecodeOffsetAdvance) {
  using interpreter::Bytecode;
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.Move(l1, o0);
  a.Move(l0, o1);
  a.add(g1, l1, l0);
  a.LoadU8(o1, MemOperand(g1));
  Label returned, done;
  a.AdvanceBytecodeOffsetOrReturn(l1, l0, o1, o2, o3, &returned);
  a.Move(o1, 0);
  a.Jump(&done);
  a.bind(&returned);
  a.Move(o1, 1);
  a.bind(&done);
  a.Move(o0, l0);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  using Entry = NativeResultPair (*)(const uint8_t*, uint64_t, const uint8_t*);
  constexpr int count = interpreter::Bytecodes::kBytecodeCount;
  std::array<uint8_t, 3 * count> sizes{};
  for (int scale = 0; scale < 3; ++scale) {
    sizes[scale * count + static_cast<int>(Bytecode::kLdaZero)] = 1;
    sizes[scale * count + static_cast<int>(Bytecode::kLdaSmi)] =
        1 + (1 << scale);
  }
  for (Bytecode bytecode :
       {Bytecode::kLdaZero, Bytecode::kLdaSmi, Bytecode::kJumpLoop,
        Bytecode::kReturn, Bytecode::kSuspendGenerator}) {
    for (int prefix : {-1, 0, 1, 2, 3}) {
      std::array<uint8_t, 32> bytes{};
      constexpr int start = 7;
      bool has_prefix = prefix >= 0;
      int opcode_offset = start + has_prefix;
      if (has_prefix) bytes[start] = static_cast<uint8_t>(prefix);
      bytes[opcode_offset] = static_cast<uint8_t>(bytecode);
      bool returns = bytecode == Bytecode::kReturn ||
                     bytecode == Bytecode::kSuspendGenerator;
      uint64_t expected = opcode_offset;
      if (bytecode == Bytecode::kJumpLoop) {
        expected = start;
      } else if (!returns) {
        int scale = has_prefix ? 1 + (prefix & 1) : 0;
        expected += sizes[scale * count + static_cast<int>(bytecode)];
      }
      NativeResultPair result =
          code.entry<Entry>()(bytes.data(), start, sizes.data());
      EXPECT_EQ(result.first, expected) << "prefix=" << prefix;
      EXPECT_EQ(result.second, static_cast<uint64_t>(returns));
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerByteReverse) {
  for (int bytes : {4, 8}) {
    for (bool alias : {false, true}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      Register output = alias ? o0 : o1;
      compiler::EmitSparc64ReverseBytes(&a, output, o0, bytes);
      a.Move(o0, output);
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t)>();
      if (bytes == 4) {
        EXPECT_EQ(entry(0x12345678), UINT64_C(0x78563412));
        EXPECT_EQ(entry(0x89abcdef), UINT64_C(0xffffffffefcdab89));
        EXPECT_EQ(entry(UINT64_C(0xffffeeee01020304)), UINT64_C(0x04030201));
      } else {
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef)),
                  UINT64_C(0xefcdab8967452301));
        EXPECT_EQ(entry(UINT64_C(0x8000000000000001)),
                  UINT64_C(0x0100000000000080));
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerLongIntegerBranch) {
  for (bool word32 : {false, true}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label target;
    a.cmp(o0, 0);
    compiler::EmitSparc64Branch(
        &a, word32 ? compiler::kSparcCmp32 : compiler::kSparcCmp64,
        compiler::kSignedLessThan, &target);
    a.mov(o0, 17);
    a.Ret();
    for (int n = 0; n < ((1 << 20) + 64) / kInstrSize; ++n) a.nop();
    a.bind(&target);
    a.mov(o0, 23);
    a.Ret();
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry = code.entry<uint64_t (*)(int64_t)>();
    EXPECT_EQ(entry(-1), 23u);
    EXPECT_EQ(entry(0), 17u);
    EXPECT_EQ(entry(1), 17u);
    EXPECT_EQ(entry(INT64_C(0x80000000)), word32 ? 23u : 17u);
  }
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerLongFloatBranch) {
  MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                   CodeObjectRequired::kNo);
  a.EnterCFunctionFrame();
  a.MovInt64ToDouble(d0, o0);
  a.MovInt64ToDouble(d1, o1);
  a.fcmpd(d0, d1);
  Label target;
  compiler::EmitSparc64Branch(&a, compiler::kSparcCmpDouble,
                              compiler::kFloatLessThan, &target);
  a.mov(o0, 17);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  for (int n = 0; n < ((1 << 20) + 64) / kInstrSize; ++n) a.nop();
  a.bind(&target);
  a.mov(o0, 23);
  a.LeaveFrame(StackFrame::MANUAL);
  a.RetCFunction();
  ExecutableAssemblerBuffer code;
  ASSERT_TRUE(code.Initialize(a));
  auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
  EXPECT_EQ(entry(base::bit_cast<uint64_t>(1.0), base::bit_cast<uint64_t>(2.0)),
            23u);
  EXPECT_EQ(entry(base::bit_cast<uint64_t>(2.0), base::bit_cast<uint64_t>(1.0)),
            17u);
  EXPECT_EQ(
      entry(base::bit_cast<uint64_t>(-0.0), base::bit_cast<uint64_t>(0.0)),
      17u);
  EXPECT_EQ(entry(UINT64_C(0x7ff8000000000001), 0), 17u);
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerLongJumpAndSwitch) {
  for (bool backward : {false, true}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label target, entry;
    if (backward) {
      a.Jump(&entry);
      a.bind(&target);
      a.mov(o0, 42);
      a.Ret();
    } else {
      a.JumpIfEqual(o0, 7, &target);
      a.JumpIfLessThan(o0, 0, &target);
      a.mov(o0, 17);
      a.Ret();
    }
    for (int n = 0; n < ((1 << 23) + 64) / kInstrSize; ++n) a.nop();
    if (backward) {
      a.bind(&entry);
      a.Jump(&target);
    } else {
      a.bind(&target);
      a.mov(o0, 42);
      a.Ret();
    }
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    auto entry_fn = code.entry<uint64_t (*)(int64_t)>();
    EXPECT_EQ(entry_fn(7), 42u);
    EXPECT_EQ(entry_fn(-1), 42u);
    EXPECT_EQ(entry_fn(1), backward ? 42u : 17u);
  }
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerLongDeoptPatch) {
  for (bool backward : {false, true}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label target, patch;
    if (backward) {
      a.Jump(&patch);
      a.bind(&target);
      a.mov(o0, 42);
      a.Ret();
    }
    if (!backward) {
      a.bind(&patch);
      a.nop();
      a.nop();
      a.nop();
      a.mov(o0, 0);
      a.Ret();
    }
    for (int n = 0; n < ((1 << 23) + 64) / kInstrSize; ++n) a.nop();
    if (backward) {
      a.bind(&patch);
      a.nop();
      a.nop();
      a.nop();
      a.mov(o0, 0);
      a.Ret();
    } else {
      a.bind(&target);
      a.mov(o0, 42);
      a.Ret();
    }
    Address start = reinterpret_cast<Address>(a.buffer_start());
    Deoptimizer::PatchToJump(start + patch.pos(), start + target.pos());
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EXPECT_EQ(code.entry<uint64_t (*)()>()(), 42u);
  }
}

TEST(AssemblerSparc64BufferTest, NativeOptimizerRotateRight) {
  for (bool word32 : {false, true}) {
    for (bool alias : {false, true}) {
      MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                       CodeObjectRequired::kNo);
      Register output = alias ? o0 : o2;
      compiler::EmitSparc64RotateRight(&a, output, o0, o1, word32);
      a.Move(o0, output);
      a.Ret();
      ExecutableAssemblerBuffer code;
      ASSERT_TRUE(code.Initialize(a));
      auto entry = code.entry<uint64_t (*)(uint64_t, uint64_t)>();
      if (word32) {
        EXPECT_EQ(entry(0x12345678, 0), UINT64_C(0x12345678));
        EXPECT_EQ(entry(0x12345678, 4), UINT64_C(0xffffffff81234567));
        EXPECT_EQ(entry(0x12345678, 8), UINT64_C(0x78123456));
        EXPECT_EQ(entry(0x12345678, 31), UINT64_C(0x2468acf0));
        EXPECT_EQ(entry(0x12345678, 32), UINT64_C(0x12345678));
        EXPECT_EQ(entry(1, 1), UINT64_C(0xffffffff80000000));
      } else {
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef), 0),
                  UINT64_C(0x0123456789abcdef));
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef), 4),
                  UINT64_C(0xf0123456789abcde));
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef), 8),
                  UINT64_C(0xef0123456789abcd));
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef), 63),
                  UINT64_C(0x02468acf13579bde));
        EXPECT_EQ(entry(UINT64_C(0x0123456789abcdef), 64),
                  UINT64_C(0x0123456789abcdef));
      }
    }
  }
}

TEST(AssemblerSparc64BufferTest, NativeLabelOffsetAcrossBufferGrowth) {
  for (bool backward : {false, true}) {
    MacroAssembler a(nullptr, MaybeAssemblerZone{}, AssemblerOptions{},
                     CodeObjectRequired::kNo);
    Label target, entry;
    if (backward) {
      a.Jump(&entry);
      a.bind(&target);
    } else {
      a.LoadLabelOffset(o0, &target);
      a.Ret();
    }
    for (int n = 0; n < ((1 << 21) + 64) / kInstrSize; ++n) a.nop();
    if (backward) {
      a.bind(&entry);
      a.LoadLabelOffset(o0, &target);
      a.Ret();
    } else {
      a.bind(&target);
    }
    ExecutableAssemblerBuffer code;
    ASSERT_TRUE(code.Initialize(a));
    EXPECT_EQ(code.entry<uint64_t (*)()>()(),
              static_cast<uint64_t>(target.pos()));
  }
}

}  // namespace
}  // namespace v8::internal
