// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_H_
#define V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_H_

#include <map>
#include <memory>
#include <vector>

#include "src/codegen/assembler.h"
#include "src/codegen/code-desc.h"
#include "src/codegen/label.h"
#include "src/codegen/safepoint-table-base.h"
#include "src/codegen/sparc64/base-assembler-sparc64.h"

namespace v8::internal {

class Assembler : public AssemblerBase, public AssemblerSparc64Base {
 public:
  explicit Assembler(const AssemblerOptions& options,
                     std::unique_ptr<AssemblerBuffer> buffer = {});
  Assembler(const MaybeAssemblerZone&, const AssemblerOptions& options,
            std::unique_ptr<AssemblerBuffer> buffer = {})
      : Assembler(options, std::move(buffer)) {}

  static constexpr int kGap = 32;
  static constexpr int kSpecialTargetSize = kSystemPointerSize;
  static constexpr int kNoHandlerTable = 0;
  static constexpr SafepointTableBuilderBase* kNoSafepointTable = nullptr;

  using AssemblerSparc64Base::b;
  using AssemblerSparc64Base::ba;
  using AssemblerSparc64Base::call;
  using AssemblerSparc64Base::fb;
  void b(Condition cond, ConditionCodes cc, Label* label, bool annul = false);
  void ba(Label* label, bool annul = false);
  void call(Label* label);
  void fb(FloatCondition condition, int fcc, Label* label, bool annul = false);
  void bind(Label* label);
  int SizeOfCodeGeneratedSince(Label* label) const {
    DCHECK(label->is_bound());
    return pc_offset() - label->pos();
  }

  void LoadLabelOffset(Register rd, Label* label);
  void LoadLiteral(Register rd, uint64_t value,
                   RelocInfo::Mode rmode = RelocInfo::NO_INFO);
  void Align(int alignment);
  void CodeTargetAlign() { Align(8); }
  void DataAlign(int alignment);
  void db(uint8_t data);
  void dd(uint32_t data);
  void dq(uint64_t data);
  void dp(uintptr_t data) { dq(data); }
  void EnsureSpaceFor(int needed);
  int buffer_space() const {
    return static_cast<int>(reloc_info_writer.pos() - pc_);
  }

  void RecordRelocInfo(RelocInfo::Mode rmode, intptr_t data = 0);
  void RecordDeoptReason(DeoptimizeReason reason, uint32_t node_id,
                         SourcePosition position, int id);
  void GetCode(LocalIsolate* isolate, CodeDesc* desc,
               SafepointTableBuilderBase* safepoint_table_builder,
               int handler_table_offset);
  void GetCode(Isolate* isolate, CodeDesc* desc);
  void GetCode(LocalIsolate* isolate, CodeDesc* desc) {
    GetCode(isolate, desc, kNoSafepointTable, kNoHandlerTable);
  }
  void MaybeEmitOutOfLineConstantPool() {}
  void ClearInternalState() {}

  Instr instr_at(int offset) const {
    return base::ReadUnalignedValue<Instr>(
        reinterpret_cast<Address>(buffer_start_ + offset));
  }
  void instr_at_put(int offset, Instr instr) {
    base::WriteUnalignedValue<Instr>(
        reinterpret_cast<Address>(buffer_start_ + offset), instr);
  }
  static Address target_address_at(Address pc, Address constant_pool) {
    return base::ReadUnalignedValue<Address>(pc);
  }
  static void set_target_address_at(
      Address pc, Address constant_pool, Address target,
      WritableJitAllocation* jit_allocation,
      ICacheFlushMode icache_flush_mode = FLUSH_ICACHE_IF_NEEDED);
  static uint32_t uint32_constant_at(Address pc, Address constant_pool) {
    return static_cast<uint32_t>(target_address_at(pc, constant_pool));
  }
  static void set_uint32_constant_at(
      Address pc, Address constant_pool, uint32_t value,
      WritableJitAllocation* jit_allocation,
      ICacheFlushMode icache_flush_mode = FLUSH_ICACHE_IF_NEEDED) {
    set_target_address_at(pc, constant_pool, value, jit_allocation,
                          icache_flush_mode);
  }
  static int deserialization_special_target_size(Address pc) {
    return kSpecialTargetSize;
  }
  static void deserialization_set_target_internal_reference_at(
      Address pc, Address target, WritableJitAllocation& jit_allocation,
      RelocInfo::Mode mode = RelocInfo::INTERNAL_REFERENCE) {
    DCHECK_EQ(mode, RelocInfo::INTERNAL_REFERENCE);
    set_target_address_at(pc, kNullAddress, target, &jit_allocation,
                          SKIP_ICACHE_FLUSH);
  }

  RelocInfoWriter reloc_info_writer;

 private:
  enum class FixupKind { kBranch19, kBranch22, kCall30, kLabelOffset };
  struct LabelFixup {
    int previous;
    FixupKind kind;
  };
  struct DeferredRelocation {
    int offset;
    RelocInfo::Mode mode;
    intptr_t data;
  };

  void emit(Instr instr) override;
  void GrowBuffer(int needed);
  void PatchBranch(int offset, int target, FixupKind kind);
  bool LinkLabel(Label* label, FixupKind kind);
  void EmitRelocations();
  int WriteCodeComments();
  void PatchInHeapNumberRequest(Address pc, Handle<HeapNumber> object) override;

  std::map<int, LabelFixup> label_fixups_;
  std::vector<DeferredRelocation> relocations_;
};

class EnsureSpace {
 public:
  explicit EnsureSpace(Assembler* assembler) {
    assembler->EnsureSpaceFor(Assembler::kGap);
  }
};

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_H_
