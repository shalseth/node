// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <algorithm>
#include <atomic>
#include <cstring>

#include "src/codegen/flush-instruction-cache.h"
#include "src/codegen/sparc64/assembler-sparc64-inl.h"
#include "src/codegen/sparc64/assembler-sparc64.h"
#include "src/execution/isolate.h"

namespace v8::internal {

const int RelocInfo::kApplyMask = ModeMask(INTERNAL_REFERENCE);
bool RelocInfo::IsCodedSpecially() { return false; }
bool RelocInfo::IsInConstantPool() { return false; }

uint32_t RelocInfo::wasm_call_tag() const {
  DCHECK(IsWasmCall(rmode_) || IsWasmStubCall(rmode_));
  return Assembler::uint32_constant_at(pc_, constant_pool_);
}

Assembler::Assembler(const AssemblerOptions& options,
                     std::unique_ptr<AssemblerBuffer> buffer)
    : AssemblerBase(options, std::move(buffer)) {
  reloc_info_writer.Reposition(buffer_start_ + buffer_->size(), pc_);
}

void Assembler::EnsureSpaceFor(int needed) {
  CHECK_GE(needed, 0);
  if (buffer_space() < needed) GrowBuffer(needed);
}

void Assembler::GrowBuffer(int needed) {
  const int old_size = buffer_->size();
  const int code_size = pc_offset();
  const int reloc_size =
      static_cast<int>(buffer_start_ + old_size - reloc_info_writer.pos());
  const int last_pc_offset =
      static_cast<int>(reloc_info_writer.last_pc() - buffer_start_);
  const int64_t new_size =
      std::max<int64_t>(std::min<int64_t>(2LL * old_size, old_size + MB),
                        int64_t{code_size} + reloc_size + needed);
  CHECK_LE(new_size, 512 * MB);
  auto replacement = buffer_->Grow(static_cast<int>(new_size));
  uint8_t* start = replacement->start();
  memcpy(start, buffer_start_, code_size);
  memcpy(start + new_size - reloc_size, reloc_info_writer.pos(), reloc_size);
  buffer_ = std::move(replacement);
  buffer_start_ = start;
  pc_ = start + code_size;
  reloc_info_writer.Reposition(start + new_size - reloc_size,
                               start + last_pc_offset);
}

void Assembler::emit(Instr instr) { dd(instr); }

void Assembler::db(uint8_t data) {
  EnsureSpaceFor(sizeof(data));
  *pc_++ = data;
}

void Assembler::dd(uint32_t data) {
  EnsureSpaceFor(sizeof(data));
  base::WriteUnalignedValue<uint32_t>(reinterpret_cast<Address>(pc_), data);
  pc_ += sizeof(data);
}

void Assembler::dq(uint64_t data) {
  EnsureSpaceFor(sizeof(data));
  base::WriteUnalignedValue<uint64_t>(reinterpret_cast<Address>(pc_), data);
  pc_ += sizeof(data);
}

void Assembler::Align(int alignment) {
  DCHECK_GE(alignment, kInstrSize);
  DCHECK(base::bits::IsPowerOfTwo(alignment));
  DCHECK_EQ(pc_offset() % kInstrSize, 0);
  while ((pc_offset() & (alignment - 1)) != 0) nop();
}

bool Assembler::LinkLabel(Label* label, FixupKind kind) {
  if (label->is_bound()) return false;
  int previous = label->is_linked() ? label->pos() : -1;
  label_fixups_.emplace(pc_offset(), LabelFixup{previous, kind});
  label->link_to(pc_offset());
  return true;
}

void Assembler::PatchBranch(int offset, int target, FixupKind kind) {
  if (kind == FixupKind::kLabelOffset) {
    instr_at_put(offset, (instr_at(offset) & ~Instr{0x3fffff}) |
                             (static_cast<uint32_t>(target) >> 10));
    instr_at_put(offset + kInstrSize,
                 (instr_at(offset + kInstrSize) & ~Instr{0x3ff}) |
                     (static_cast<uint32_t>(target) & 0x3ff));
    return;
  }
  int bits = kind == FixupKind::kBranch19   ? 19
             : kind == FixupKind::kBranch22 ? 22
                                            : 30;
  int64_t displacement = int64_t{target} - offset;
  CHECK_EQ(displacement % kInstrSize, 0);
  displacement /= kInstrSize;
  CHECK_GE(displacement, -(int64_t{1} << (bits - 1)));
  CHECK_LT(displacement, int64_t{1} << (bits - 1));
  Instr mask = (Instr{1} << bits) - 1;
  instr_at_put(offset, (instr_at(offset) & ~mask) |
                           (static_cast<Instr>(displacement) & mask));
}

void Assembler::b(Condition cond, ConditionCodes cc, Label* label, bool annul) {
  const int offset = pc_offset();
  bool linked = LinkLabel(label, FixupKind::kBranch19);
  AssemblerSparc64Base::b(cond, cc, 0, annul);
  if (!linked) PatchBranch(offset, label->pos(), FixupKind::kBranch19);
}

void Assembler::ba(Label* label, bool annul) {
  const int offset = pc_offset();
  bool linked = LinkLabel(label, FixupKind::kBranch22);
  AssemblerSparc64Base::ba(0, annul);
  if (!linked) PatchBranch(offset, label->pos(), FixupKind::kBranch22);
}

void Assembler::call(Label* label) {
  const int offset = pc_offset();
  bool linked = LinkLabel(label, FixupKind::kCall30);
  AssemblerSparc64Base::call(0);
  if (!linked) PatchBranch(offset, label->pos(), FixupKind::kCall30);
}

void Assembler::fb(FloatCondition condition, int fcc, Label* label,
                   bool annul) {
  const int offset = pc_offset();
  bool linked = LinkLabel(label, FixupKind::kBranch19);
  AssemblerSparc64Base::fb(condition, fcc, 0, annul);
  if (!linked) PatchBranch(offset, label->pos(), FixupKind::kBranch19);
}

void Assembler::bind(Label* label) {
  CHECK(!label->is_bound());
  int reference = label->is_linked() ? label->pos() : -1;
  while (reference >= 0) {
    auto fixup = label_fixups_.find(reference);
    CHECK(fixup != label_fixups_.end());
    PatchBranch(reference, pc_offset(), fixup->second.kind);
    reference = fixup->second.previous;
    label_fixups_.erase(fixup);
  }
  label->bind_to(pc_offset());
}

void Assembler::LoadLabelOffset(Register rd, Label* label) {
  const int offset = pc_offset();
  bool linked = LinkLabel(label, FixupKind::kLabelOffset);
  sethi(rd, 0);
  or_(rd, rd, 0);
  if (!linked) PatchBranch(offset, label->pos(), FixupKind::kLabelOffset);
}

void Assembler::LoadLiteral(Register rd, uint64_t value,
                            RelocInfo::Mode rmode) {
  CHECK_NE(rd, g0);
  Align(kSystemPointerSize);
  rdpc(rd);
  ldx(rd, rd, 4 * kInstrSize);
  ba(4 * kInstrSize);
  nop();
  RecordRelocInfo(rmode);
  dq(value);
}

void Assembler::RecordRelocInfo(RelocInfo::Mode mode, intptr_t data) {
  if (RelocInfo::IsNoInfo(mode) || !ShouldRecordRelocInfo(mode)) return;
  relocations_.push_back({pc_offset(), mode, data});
}

void Assembler::EmitRelocations() {
  EnsureSpaceFor(static_cast<int>(relocations_.size()) *
                 RelocInfoWriter::kMaxSize);
  for (const auto& relocation : relocations_) {
    Address pc = reinterpret_cast<Address>(buffer_start_) + relocation.offset;
    if (RelocInfo::IsInternalReference(relocation.mode)) {
      Address offset = base::ReadUnalignedValue<Address>(pc);
      base::WriteUnalignedValue<Address>(
          pc, reinterpret_cast<Address>(buffer_start_) + offset);
    }
    RelocInfo info(pc, relocation.mode, relocation.data);
    reloc_info_writer.Write(&info);
  }
  relocations_.clear();
}

void Assembler::GetCode(Isolate* isolate, CodeDesc* desc) {
  GetCode(isolate->main_thread_local_isolate(), desc);
}

void Assembler::GetCode(LocalIsolate* isolate, CodeDesc* desc,
                        SafepointTableBuilderBase* safepoints,
                        int handler_table_offset) {
  CHECK(label_fixups_.empty());
  DataAlign(InstructionStream::kMetadataAlignment);
  AllocateAndInstallRequestedHeapNumbers(isolate);
  const int comments_size = WriteCodeComments();
  EmitRelocations();
  const int code_end = pc_offset();
  const int comments_offset = code_end - comments_size;
  const int handler_offset = handler_table_offset == kNoHandlerTable
                                 ? comments_offset
                                 : handler_table_offset;
  const int safepoint_offset = safepoints == nullptr
                                   ? handler_offset
                                   : safepoints->safepoint_table_offset();
  const int relocation_offset =
      static_cast<int>(reloc_info_writer.pos() - buffer_start_);
  CodeDesc::Initialize(desc, this, safepoint_offset, handler_offset,
                       comments_offset, comments_offset, code_end,
                       relocation_offset);
}

void Assembler::set_target_address_at(Address pc, Address constant_pool,
                                      Address target,
                                      WritableJitAllocation* jit_allocation,
                                      ICacheFlushMode icache_flush_mode) {
  if (pc % alignof(Address) != 0) {
    // Serialized Wasm buffers need not preserve executable code alignment.
    CHECK_EQ(icache_flush_mode, SKIP_ICACHE_FLUSH);
    if (jit_allocation != nullptr) {
      jit_allocation->WriteUnalignedValue(pc, target);
    } else {
      base::WriteUnalignedValue<Address>(pc, target);
    }
    return;
  }
  if (jit_allocation != nullptr) {
    jit_allocation->WriteValue(pc, target, kRelaxedStore);
  } else {
    std::atomic_ref<Address>(*reinterpret_cast<Address*>(pc))
        .store(target, std::memory_order_relaxed);
  }
  if (icache_flush_mode == FLUSH_ICACHE_IF_NEEDED) {
    FlushInstructionCache(pc, sizeof(Address));
  }
}

void Assembler::PatchInHeapNumberRequest(Address pc,
                                         Handle<HeapNumber> object) {
  set_target_address_at(pc, kNullAddress, object.address(), nullptr,
                        SKIP_ICACHE_FLUSH);
}

}  // namespace v8::internal
