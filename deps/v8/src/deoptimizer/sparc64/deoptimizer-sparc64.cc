// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/assembler-inl.h"
#include "src/codegen/flush-instruction-cache.h"
#include "src/codegen/reloc-info-inl.h"
#include "src/common/code-memory-access-inl.h"
#include "src/deoptimizer/deoptimizer.h"
#include "src/execution/isolate-data.h"

namespace v8::internal {

static_assert(IsolateData::BuiltinEntrySlotOffset(
                  Builtin::kDeoptimizationEntry_Eager) >= -4096);
static_assert(IsolateData::BuiltinEntrySlotOffset(
                  Builtin::kDeoptimizationEntry_Lazy) <= 4095);

const int Deoptimizer::kEagerDeoptExitSize = 3 * kInstrSize;
const int Deoptimizer::kLazyDeoptExitSize = 3 * kInstrSize;
const int Deoptimizer::kAdaptShadowStackOffsetToSubtract = 0;

void Deoptimizer::ZapCode(Address start, Address end, RelocIterator& it) {
  CHECK_EQ(start % kInstrSize, 0);
  CHECK_EQ(end % kInstrSize, 0);
  RwxMemoryWriteScope scope("Zap deoptimized SPARC64 code");
  Address cursor = start;
  auto zap_until = [&](Address limit) {
    while (cursor < limit) {
      base::WriteUnalignedValue<Instr>(cursor, 0x91d02001u);  // ta 1
      cursor += kInstrSize;
    }
  };
  for (; !it.done(); it.next()) {
    RelocInfo* info = it.rinfo();
    if (!RelocInfo::IsGCRelocMode(info->rmode())) continue;
    Address slot = info->target_address_address_for_gc();
    if (slot >= end) break;
    if (slot < cursor) continue;
    zap_until(slot);
    cursor = slot + info->target_address_size_for_gc();
  }
  zap_until(end);
  FlushInstructionCache(start, end - start);
}

void Deoptimizer::PatchToJump(Address pc, Address new_pc) {
  intptr_t offset = static_cast<intptr_t>(new_pc) - static_cast<intptr_t>(pc);
  CHECK_EQ(offset % kInstrSize, 0);
  RwxMemoryWriteScope scope("Patch SPARC64 deopt trampoline");
  Assembler masm(AssemblerOptions{},
                 ExternalAssemblerBuffer(reinterpret_cast<uint8_t*>(pc), 128));
  if (offset >= -(intptr_t{1} << 23) && offset < (intptr_t{1} << 23)) {
    masm.ba(static_cast<int32_t>(offset));
    masm.nop();
  } else {
    intptr_t call_offset = offset - kInstrSize;
    CHECK_GE(call_offset, -(intptr_t{1} << 31));
    CHECK_LT(call_offset, intptr_t{1} << 31);
    masm.mov(g4, o7);
    masm.call(static_cast<int32_t>(call_offset));
    masm.mov(o7, g4);
  }
  FlushInstructionCache(pc, masm.pc_offset());
}

Float32 RegisterValues::GetFloatRegister(unsigned n) const {
  V8_ASSUME(n < arraysize(simd128_registers_));
  // The even single lane is the first word of the big-endian saved double.
  return base::ReadUnalignedValue<Float32>(
      reinterpret_cast<Address>(simd128_registers_ + n));
}

Float64 RegisterValues::GetDoubleRegister(unsigned n) const {
  V8_ASSUME(n < arraysize(simd128_registers_));
  return base::ReadUnalignedValue<Float64>(
      reinterpret_cast<Address>(simd128_registers_ + n));
}

void RegisterValues::SetDoubleRegister(unsigned n, Float64 value) {
  V8_ASSUME(n < arraysize(simd128_registers_));
  base::WriteUnalignedValue<Float64>(
      reinterpret_cast<Address>(simd128_registers_ + n), value);
}

void FrameDescription::SetCallerPc(unsigned offset, intptr_t value) {
  SetFrameSlot(offset, value);
}

void FrameDescription::SetCallerFp(unsigned offset, intptr_t value) {
  SetFrameSlot(offset, value);
}

void FrameDescription::SetCallerConstantPool(unsigned, intptr_t) {
  UNREACHABLE();
}

void FrameDescription::SetPc(intptr_t pc) { pc_ = pc; }

}  // namespace v8::internal
