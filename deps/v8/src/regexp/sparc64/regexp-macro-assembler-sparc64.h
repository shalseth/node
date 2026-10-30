// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_REGEXP_SPARC64_REGEXP_MACRO_ASSEMBLER_SPARC64_H_
#define V8_REGEXP_SPARC64_REGEXP_MACRO_ASSEMBLER_SPARC64_H_

#include "src/base/strings.h"
#include "src/codegen/assembler-arch.h"
#include "src/codegen/macro-assembler.h"
#include "src/regexp/regexp-macro-assembler.h"

namespace v8 {
namespace internal {

class V8_EXPORT_PRIVATE RegExpMacroAssemblerSPARC64
    : public NativeRegExpMacroAssembler {
 public:
  RegExpMacroAssemblerSPARC64(Isolate* isolate, Zone* zone, Mode mode,
                              int registers_to_save);
  ~RegExpMacroAssemblerSPARC64() override;
  void AbortedCodeGeneration() override;
  void AdvanceCurrentPosition(int by) override;
  void AdvanceRegister(int reg, int by) override;
  void Backtrack() override;
  void Bind(Label* label) override;
  void CheckAtStart(int cp_offset, Label* on_at_start) override;
  void CheckCharacter(uint32_t c, Label* on_equal) override;
  void CheckCharacterAfterAnd(uint32_t c, uint32_t mask,
                              Label* on_equal) override;
  void CheckCharacterGT(base::uc16 limit, Label* on_greater) override;
  void CheckCharacterLT(base::uc16 limit, Label* on_less) override;
  void CheckFixedLengthLoop(Label* on_tos_equals_current_position) override;
  void CheckNotAtStart(int cp_offset, Label* on_not_at_start) override;
  void CheckNotBackReference(int start_reg, bool read_backward,
                             Label* on_no_match) override;
  void CheckNotBackReferenceIgnoreCase(int start_reg, bool read_backward,
                                       bool unicode,
                                       Label* on_no_match) override;
  void CheckNotCharacter(uint32_t c, Label* on_not_equal) override;
  void CheckNotCharacterAfterAnd(uint32_t c, uint32_t mask,
                                 Label* on_not_equal) override;
  void CheckNotCharacterAfterMinusAnd(base::uc16 c, base::uc16 minus,
                                      base::uc16 mask,
                                      Label* on_not_equal) override;
  void CheckCharacterInRange(base::uc16 from, base::uc16 to,
                             Label* on_in_range) override;
  void CheckCharacterNotInRange(base::uc16 from, base::uc16 to,
                                Label* on_not_in_range) override;
  bool CheckCharacterInRangeArray(const ZoneList<CharacterRange>* ranges,
                                  Label* on_in_range) override;
  bool CheckCharacterNotInRangeArray(const ZoneList<CharacterRange>* ranges,
                                     Label* on_not_in_range) override;
  void CheckBitInTable(Handle<ByteArray> table, Label* on_bit_set) override;
  void SkipUntilBitInTable(int cp_offset, Handle<ByteArray> table,
                           Handle<ByteArray> nibble_table, int advance_by,
                           Label* on_match, Label* on_no_match) override;
  bool SkipUntilBitInTableUseSimd(int advance_by) override;

  void CheckPosition(int cp_offset, Label* on_outside_input) override;
  void CheckSpecialClassRanges(StandardCharacterSet type,
                               Label* on_no_match) override;
  void Fail() override;
  DirectHandle<HeapObject> GetCode(DirectHandle<String> source,
                                   RegExpFlags flags) override;
  void GoTo(Label* label) override;
  void IfRegisterGE(int reg, int comparand, Label* if_ge) override;
  void IfRegisterLT(int reg, int comparand, Label* if_lt) override;
  void IfRegisterEqPos(int reg, Label* if_eq) override;
  IrregexpImplementation Implementation() override;
  void LoadCurrentCharacterUnchecked(int cp_offset,
                                     int character_count) override;
  void PopCurrentPosition() override;
  void PopRegister(int register_index) override;
  void PushBacktrack(Label* label) override;
  void PushCurrentPosition() override;
  void PushRegister(int register_index,
                    StackCheckFlag check_stack_limit) override;
  void ReadCurrentPositionFromRegister(int reg) override;
  void ReadStackPointerFromRegister(int reg) override;
  void SetCurrentPositionFromEnd(int by) override;
  void SetRegister(int register_index, int to) override;
  bool Succeed() override;
  void WriteCurrentPositionToRegister(int reg, int cp_offset) override;
  void ClearRegisters(int reg_from, int reg_to) override;
  void WriteStackPointerToRegister(int reg) override;

  void RecordComment(std::string_view comment) override {
    masm_->RecordComment(comment);
  }
  MacroAssembler* masm() override { return masm_.get(); }

  static int64_t CheckStackGuardState(Address* return_address, Address raw_code,
                                      Address re_frame, uintptr_t extra_space);

 private:
  static constexpr int kFrameTypeOffset = -8;
  static constexpr int kIsolateOffset = -16;
  static constexpr int kDirectCallOffset = -24;
  static constexpr int kNumOutputRegistersOffset = -32;
  static constexpr int kRegisterOutputOffset = -40;
  static constexpr int kInputEndOffset = -48;
  static constexpr int kInputStartOffset = -56;
  static constexpr int kStartIndexOffset = -64;
  static constexpr int kInputStringOffset = -72;
  static constexpr int kSuccessfulCapturesOffset = -80;
  static constexpr int kStringStartMinusOneOffset = -88;
  static constexpr int kBacktrackCountOffset = -96;
  static constexpr int kRegExpStackBasePointerOffset = -104;
  static constexpr int kRegisterZeroOffset = -112;

  static constexpr int kInitialBufferSize = 1024;

  void CallCFunctionFromIrregexpCode(ExternalReference function,
                                     int num_arguments);

  void CheckPreemption();

  void CheckStackLimit();
  void AssertAboveStackLimitMinusSlack();

  void CallCheckStackGuardState(Register scratch,
                                Operand extra_space_for_variables = Operand(0));
  void CallIsCharacterInRangeArray(const ZoneList<CharacterRange>* ranges);

  MemOperand register_location(int register_index);

  static constexpr Register current_input_offset() { return l1; }

  static constexpr Register current_character() { return l2; }

  static constexpr Register end_of_input_address() { return l3; }

  static constexpr Register frame_pointer() { return fp; }

  static constexpr Register backtrack_stackpointer() { return l4; }

  static constexpr Register code_pointer() { return l0; }

  void BranchOrBacktrack(Label* to, Condition condition, Register rs,
                         const Operand& rt);

  inline void SafeCall(Label* to, Condition cond, Register rs,
                       const Operand& rt);
  inline void SafeReturn();
  inline void SafeCallTarget(Label* name);

  inline void Push(Register source);

  inline void Pop(Register target);

  void LoadRegExpStackPointerFromMemory(Register dst);
  void StoreRegExpStackPointerToMemory(Register src, Register scratch);
  void PushRegExpBasePointer(Register stack_pointer, Register scratch);
  void PopRegExpBasePointer(Register stack_pointer_out, Register scratch);

  void Branch(Label* to, Condition condition, Register lhs, Operand rhs);
  void Add(Register dst, Register lhs, Operand rhs);
  void Add(Register dst, Register lhs, Register rhs) {
    Add(dst, lhs, Operand(rhs));
  }
  void Add(Register dst, Register lhs, intptr_t rhs) {
    Add(dst, lhs, Operand(rhs));
  }
  void Sub(Register dst, Register lhs, Operand rhs);
  void Sub(Register dst, Register lhs, Register rhs) {
    Sub(dst, lhs, Operand(rhs));
  }
  void Sub(Register dst, Register lhs, intptr_t rhs) {
    Sub(dst, lhs, Operand(rhs));
  }
  void And(Register dst, Register lhs, Operand rhs);
  void Or(Register dst, Register lhs, Operand rhs);
  void Xor(Register dst, Register lhs, Operand rhs);
  void Move(Register dst, Operand src);
  void Move(Register dst, intptr_t src) { Move(dst, Operand(src)); }
  void Move(Register dst, ExternalReference src) { masm_->Move(dst, src); }
  void Move(Register dst, Handle<HeapObject> src) { masm_->Move(dst, src); }

  Isolate* isolate() const { return masm_->isolate(); }

  const std::unique_ptr<MacroAssembler> masm_;
  const NoRootArrayScope no_root_array_scope_;

  int num_registers_;

  const int num_saved_registers_;

  Label entry_label_;
  Label start_label_;
  Label success_label_;
  Label backtrack_label_;
  Label exit_label_;
  Label check_preempt_label_;
  Label stack_overflow_label_;
  Label internal_failure_label_;
  Label fallback_label_;
};

}  // namespace internal
}  // namespace v8

#endif  // V8_REGEXP_SPARC64_REGEXP_MACRO_ASSEMBLER_SPARC64_H_
