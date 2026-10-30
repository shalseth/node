// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/regexp/sparc64/regexp-macro-assembler-sparc64.h"

#include "src/codegen/assembler-inl.h"
#include "src/codegen/macro-assembler.h"
#include "src/logging/log.h"
#include "src/objects/objects-inl.h"
#include "src/regexp/regexp-macro-assembler.h"
#include "src/regexp/regexp-stack.h"
#include "src/snapshot/embedded/embedded-data-inl.h"
#include "src/strings/unicode.h"

namespace v8 {
namespace internal {

// l0: code, l1: input offset, l2: character, l3: input end, l4: regexp
// stack, l5: V8 frame, l7: roots. Native i6 remains the window linkage.

#define __ masm_->

RegExpMacroAssemblerSPARC64::RegExpMacroAssemblerSPARC64(Isolate* isolate,
                                                         Zone* zone, Mode mode,
                                                         int registers_to_save)
    : NativeRegExpMacroAssembler(isolate, zone, mode),
      masm_(std::make_unique<MacroAssembler>(
          isolate, CodeObjectRequired::kYes,
          NewAssemblerBuffer(kInitialBufferSize))),
      no_root_array_scope_(masm_.get()),
      num_registers_(registers_to_save),
      num_saved_registers_(registers_to_save),
      entry_label_(),
      start_label_(),
      success_label_(),
      backtrack_label_(),
      exit_label_(),
      internal_failure_label_() {
  DCHECK_EQ(0, registers_to_save % 2);
  __ jmp(&entry_label_);  // We'll write the entry code later.
  __ bind(&internal_failure_label_);
  Move(o0, Operand(FAILURE));
  __ Ret();
  __ bind(&start_label_);  // And then continue from here.
}

RegExpMacroAssemblerSPARC64::~RegExpMacroAssemblerSPARC64() = default;

void RegExpMacroAssemblerSPARC64::AbortedCodeGeneration() {
  masm_->ClearInternalState();

  entry_label_.Unuse();
  start_label_.Unuse();
  success_label_.Unuse();
  backtrack_label_.Unuse();
  exit_label_.Unuse();
  check_preempt_label_.Unuse();
  stack_overflow_label_.Unuse();
  internal_failure_label_.Unuse();
  fallback_label_.Unuse();
}

void RegExpMacroAssemblerSPARC64::AdvanceCurrentPosition(int by) {
  if (by != 0) {
    Add(current_input_offset(), current_input_offset(),
        Operand(by * char_size()));
  }
}

void RegExpMacroAssemblerSPARC64::AdvanceRegister(int reg, int by) {
  DCHECK_LE(0, reg);
  DCHECK_GT(num_registers_, reg);
  if (by != 0) {
    __ LoadU64(o0, register_location(reg));
    Add(o0, o0, Operand(by));
    __ StoreU64(o0, register_location(reg));
  }
}

void RegExpMacroAssemblerSPARC64::Backtrack() {
  CheckPreemption();
  if (has_backtrack_limit()) {
    Label next;
    __ LoadU64(o0, MemOperand(frame_pointer(), kBacktrackCountOffset));
    Add(o0, o0, Operand(1));
    __ StoreU64(o0, MemOperand(frame_pointer(), kBacktrackCountOffset));
    Branch(&next, not_equal, o0, Operand(backtrack_limit()));

    if (can_fallback()) {
      __ jmp(&fallback_label_);
    } else {
      Fail();
    }

    __ bind(&next);
  }
  Pop(o0);
  Add(o0, o0, code_pointer());
  __ Jump(o0);
}

void RegExpMacroAssemblerSPARC64::Bind(Label* label) { __ bind(label); }

void RegExpMacroAssemblerSPARC64::CheckCharacter(uint32_t c, Label* on_equal) {
  BranchOrBacktrack(on_equal, equal, current_character(), Operand(c));
}

void RegExpMacroAssemblerSPARC64::CheckCharacterGT(base::uc16 limit,
                                                   Label* on_greater) {
  BranchOrBacktrack(on_greater, greater, current_character(), Operand(limit));
}

void RegExpMacroAssemblerSPARC64::CheckAtStart(int cp_offset,
                                               Label* on_at_start) {
  __ LoadU64(o1, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
  Add(o0, current_input_offset(),
      Operand(-char_size() + cp_offset * char_size()));
  BranchOrBacktrack(on_at_start, equal, o0, Operand(o1));
}

void RegExpMacroAssemblerSPARC64::CheckNotAtStart(int cp_offset,
                                                  Label* on_not_at_start) {
  __ LoadU64(o1, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
  Add(o0, current_input_offset(),
      Operand(-char_size() + cp_offset * char_size()));
  BranchOrBacktrack(on_not_at_start, not_equal, o0, Operand(o1));
}

void RegExpMacroAssemblerSPARC64::CheckCharacterLT(base::uc16 limit,
                                                   Label* on_less) {
  BranchOrBacktrack(on_less, less, current_character(), Operand(limit));
}

void RegExpMacroAssemblerSPARC64::CheckFixedLengthLoop(Label* on_equal) {
  Label backtrack_non_equal;
  __ LoadS32(o0, MemOperand(backtrack_stackpointer(), 0));
  Branch(&backtrack_non_equal, not_equal, current_input_offset(), Operand(o0));
  Add(backtrack_stackpointer(), backtrack_stackpointer(), Operand(kIntSize));
  __ bind(&backtrack_non_equal);
  BranchOrBacktrack(on_equal, equal, current_input_offset(), Operand(o0));
}

void RegExpMacroAssemblerSPARC64::CallIsCharacterInRangeArray(
    const ZoneList<CharacterRange>* ranges) {
  static const int kNumArguments = 3;
  __ PrepareCallCFunction(kNumArguments, o0);

  __ Move(o0, current_character());
  __ Move(o1, GetOrAddRangeArray(ranges));
  __ Move(o2, ExternalReference::isolate_address(isolate()));

  {
    FrameScope scope(masm_.get(), StackFrame::MANUAL);
    CallCFunctionFromIrregexpCode(
        ExternalReference::re_is_character_in_range_array(), kNumArguments);
  }
  __ Move(code_pointer(), masm_->CodeObject());
}

bool RegExpMacroAssemblerSPARC64::CheckCharacterInRangeArray(
    const ZoneList<CharacterRange>* ranges, Label* on_in_range) {
  CallIsCharacterInRangeArray(ranges);
  BranchOrBacktrack(on_in_range, not_equal, o0, Operand(g0));
  return true;
}

bool RegExpMacroAssemblerSPARC64::CheckCharacterNotInRangeArray(
    const ZoneList<CharacterRange>* ranges, Label* on_not_in_range) {
  CallIsCharacterInRangeArray(ranges);
  BranchOrBacktrack(on_not_in_range, equal, o0, Operand(g0));
  return true;
}

void RegExpMacroAssemblerSPARC64::CheckNotBackReferenceIgnoreCase(
    int start_reg, bool read_backward, bool unicode, Label* on_no_match) {
  Label fallthrough;
  __ LoadU64(o0, register_location(start_reg));  // Index of start of capture.
  __ LoadU64(o1,
             register_location(start_reg + 1));  // Index of end of capture.
  Sub(o1, o1, o0);                               // Length of capture.

  Branch(&fallthrough, equal, o1, Operand(g0));

  if (read_backward) {
    __ LoadU64(g2, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
    Add(g2, g2, o1);
    BranchOrBacktrack(on_no_match, less_equal, current_input_offset(),
                      Operand(g2));
  } else {
    Add(g2, o1, current_input_offset());
    BranchOrBacktrack(on_no_match, greater, g2, Operand(g0));
  }

  if (mode() == LATIN1) {
    Label success;
    Label fail;
    Label loop_check;

    Add(o0, o0, Operand(end_of_input_address()));
    Add(o2, end_of_input_address(), Operand(current_input_offset()));
    if (read_backward) {
      Sub(o2, o2, Operand(o1));
    }
    Add(o1, o0, Operand(o1));

    Label loop;
    __ bind(&loop);
    __ LoadU8(o3, MemOperand(o0, 0));
    Add(o0, o0, char_size());
    __ LoadU8(o4, MemOperand(o2, 0));
    Add(o2, o2, char_size());

    Branch(&loop_check, equal, o4, Operand(o3));

    Or(o3, o3, Operand(0x20));  // Convert capture character to lower-case.
    Or(o4, o4, Operand(0x20));  // Also convert input character.
    Branch(&fail, not_equal, o4, Operand(o3));
    Sub(o3, o3, Operand('a'));
    Branch(&loop_check, less_equal_unsigned, o3, Operand('z' - 'a'));
    Sub(o3, o3, Operand(224 - 'a'));
    Branch(&fail, greater_unsigned, o3, Operand(254 - 224));
    Branch(&fail, equal, o3, Operand(247 - 224));

    __ bind(&loop_check);
    Branch(&loop, less, o0, Operand(o1));
    __ jmp(&success);

    __ bind(&fail);
    GoTo(on_no_match);

    __ bind(&success);
    Sub(current_input_offset(), o2, end_of_input_address());
    if (read_backward) {
      __ LoadU64(g2,
                 register_location(start_reg));  // Index of start of capture.
      __ LoadU64(o2,
                 register_location(start_reg + 1));  // Index of end of capture.
      Add(current_input_offset(), current_input_offset(), Operand(g2));
      Sub(current_input_offset(), current_input_offset(), Operand(o2));
    }
  } else {
    DCHECK(mode() == UC16);

    int argument_count = 4;
    __ PrepareCallCFunction(argument_count, o2);

    Add(o0, o0, Operand(end_of_input_address()));
    __ Move(o2, o1);
    __ Move(l6, o1);
    Add(o1, current_input_offset(), Operand(end_of_input_address()));
    if (read_backward) {
      Sub(o1, o1, Operand(l6));
    }
    __ Move(o3, ExternalReference::isolate_address(masm_->isolate()));

    {
      AllowExternalCallThatCantCauseGC scope(masm_.get());
      ExternalReference function =
          unicode
              ? ExternalReference::re_case_insensitive_compare_unicode()
              : ExternalReference::re_case_insensitive_compare_non_unicode();
      CallCFunctionFromIrregexpCode(function, argument_count);
    }

    BranchOrBacktrack(on_no_match, equal, o0, Operand(g0));
    if (read_backward) {
      Sub(current_input_offset(), current_input_offset(), Operand(l6));
    } else {
      Add(current_input_offset(), current_input_offset(), Operand(l6));
    }
  }

  __ bind(&fallthrough);
}

void RegExpMacroAssemblerSPARC64::CheckNotBackReference(int start_reg,
                                                        bool read_backward,
                                                        Label* on_no_match) {
  Label fallthrough;

  __ LoadU64(o0, register_location(start_reg));
  __ LoadU64(o1, register_location(start_reg + 1));
  Sub(o1, o1, o0);  // Length to check.

  Branch(&fallthrough, equal, o1, Operand(g0));

  if (read_backward) {
    __ LoadU64(g2, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
    Add(g2, g2, o1);
    BranchOrBacktrack(on_no_match, less_equal, current_input_offset(),
                      Operand(g2));
  } else {
    Add(g2, o1, current_input_offset());
    BranchOrBacktrack(on_no_match, greater, g2, Operand(g0));
  }

  Add(o0, o0, Operand(end_of_input_address()));
  Add(o2, end_of_input_address(), Operand(current_input_offset()));
  if (read_backward) {
    Sub(o2, o2, Operand(o1));
  }
  Add(o1, o1, Operand(o0));

  Label loop;
  __ bind(&loop);
  if (mode() == LATIN1) {
    __ LoadU8(o3, MemOperand(o0, 0));
    Add(o0, o0, char_size());
    __ LoadU8(o4, MemOperand(o2, 0));
    Add(o2, o2, char_size());
  } else {
    DCHECK(mode() == UC16);
    __ LoadU16(o3, MemOperand(o0, 0));
    Add(o0, o0, char_size());
    __ LoadU16(o4, MemOperand(o2, 0));
    Add(o2, o2, char_size());
  }
  BranchOrBacktrack(on_no_match, not_equal, o3, Operand(o4));
  Branch(&loop, less, o0, Operand(o1));

  Sub(current_input_offset(), o2, end_of_input_address());
  if (read_backward) {
    __ LoadU64(g2,
               register_location(start_reg));  // Index of start of capture.
    __ LoadU64(o2,
               register_location(start_reg + 1));  // Index of end of capture.
    Add(current_input_offset(), current_input_offset(), Operand(g2));
    Sub(current_input_offset(), current_input_offset(), Operand(o2));
  }
  __ bind(&fallthrough);
}

void RegExpMacroAssemblerSPARC64::CheckNotCharacter(uint32_t c,
                                                    Label* on_not_equal) {
  BranchOrBacktrack(on_not_equal, not_equal, current_character(), Operand(c));
}

void RegExpMacroAssemblerSPARC64::CheckCharacterAfterAnd(uint32_t c,
                                                         uint32_t mask,
                                                         Label* on_equal) {
  And(o0, current_character(), Operand(mask));
  Operand rhs = (c == 0) ? Operand(g0) : Operand(c);
  BranchOrBacktrack(on_equal, equal, o0, rhs);
}

void RegExpMacroAssemblerSPARC64::CheckNotCharacterAfterAnd(
    uint32_t c, uint32_t mask, Label* on_not_equal) {
  And(o0, current_character(), Operand(mask));
  Operand rhs = (c == 0) ? Operand(g0) : Operand(c);
  BranchOrBacktrack(on_not_equal, not_equal, o0, rhs);
}

void RegExpMacroAssemblerSPARC64::CheckNotCharacterAfterMinusAnd(
    base::uc16 c, base::uc16 minus, base::uc16 mask, Label* on_not_equal) {
  DCHECK_GT(String::kMaxUtf16CodeUnit, minus);
  Sub(o0, current_character(), Operand(minus));
  And(o0, o0, Operand(mask));
  BranchOrBacktrack(on_not_equal, not_equal, o0, Operand(c));
}

void RegExpMacroAssemblerSPARC64::CheckCharacterInRange(base::uc16 from,
                                                        base::uc16 to,
                                                        Label* on_in_range) {
  Sub(o0, current_character(), Operand(from));
  BranchOrBacktrack(on_in_range, less_equal_unsigned, o0, Operand(to - from));
}

void RegExpMacroAssemblerSPARC64::CheckCharacterNotInRange(
    base::uc16 from, base::uc16 to, Label* on_not_in_range) {
  Sub(o0, current_character(), Operand(from));
  BranchOrBacktrack(on_not_in_range, greater_unsigned, o0, Operand(to - from));
}

void RegExpMacroAssemblerSPARC64::CheckBitInTable(Handle<ByteArray> table,
                                                  Label* on_bit_set) {
  __ Move(o0, table);
  if (mode() != LATIN1 || kTableMask != String::kMaxOneByteCharCode) {
    And(o1, current_character(), Operand(kTableSize - 1));
    Add(o0, o0, o1);
  } else {
    Add(o0, o0, current_character());
  }

  __ LoadU8(o0, FieldMemOperand(o0, OFFSET_OF_DATA_START(ByteArray)));
  BranchOrBacktrack(on_bit_set, not_equal, o0, Operand(g0));
}

void RegExpMacroAssemblerSPARC64::SkipUntilBitInTable(
    int cp_offset, Handle<ByteArray> table,
    Handle<ByteArray> nibble_table_array, int advance_by, Label* on_match,
    Label* on_no_match) {
  Register table_reg = o0;
  __ Move(table_reg, table);

  Label scalar_repeat;
  Bind(&scalar_repeat);
  CheckPosition(cp_offset, on_no_match);
  LoadCurrentCharacterUnchecked(cp_offset, 1);

  DCHECK_NE(o1, table_reg);

  if (mode() != LATIN1 || kTableMask != String::kMaxOneByteCharCode) {
    And(o1, current_character(), Operand(kTableMask));
    Add(o1, table_reg, o1);
  } else {
    Add(o1, table_reg, current_character());
  }
  __ LoadU8(o1, FieldMemOperand(o1, OFFSET_OF_DATA_START(ByteArray)));
  BranchOrBacktrack(on_match, not_equal, o1, Operand(g0));

  AdvanceCurrentPosition(advance_by);
  __ jmp(&scalar_repeat);
}

bool RegExpMacroAssemblerSPARC64::SkipUntilBitInTableUseSimd(int advance_by) {
  return false;
}

void RegExpMacroAssemblerSPARC64::CheckSpecialClassRanges(
    StandardCharacterSet type, Label* on_no_match) {
  DCHECK(CanOptimizeSpecialClassRanges(type));
  switch (type) {
    case StandardCharacterSet::kWhitespace: {
      DCHECK_EQ(mode(), LATIN1);
      Label success;
      Branch(&success, equal, current_character(), Operand(' '));
      Sub(o0, current_character(), Operand('\t'));
      Branch(&success, less_equal_unsigned, o0, Operand('\r' - '\t'));
      BranchOrBacktrack(on_no_match, not_equal, o0, Operand(0x00A0 - '\t'));
      __ bind(&success);
      break;
    }
    case StandardCharacterSet::kNotWhitespace:
      UNREACHABLE();
    case StandardCharacterSet::kDigit:
      Sub(o0, current_character(), Operand('0'));
      BranchOrBacktrack(on_no_match, greater_unsigned, o0, Operand('9' - '0'));
      break;
    case StandardCharacterSet::kNotDigit:
      Sub(o0, current_character(), Operand('0'));
      BranchOrBacktrack(on_no_match, less_equal_unsigned, o0,
                        Operand('9' - '0'));
      break;
    case StandardCharacterSet::kNotLineTerminator: {
      Xor(o0, current_character(), Operand(0x01));
      Sub(o0, o0, Operand(0x0B));
      BranchOrBacktrack(on_no_match, less_equal_unsigned, o0,
                        Operand(0x0C - 0x0B));
      if (mode() == UC16) {
        Sub(o0, o0, Operand(0x2028 - 0x0B));
        BranchOrBacktrack(on_no_match, less_equal_unsigned, o0, Operand(1));
      }
      break;
    }
    case StandardCharacterSet::kLineTerminator: {
      Xor(o0, current_character(), Operand(0x01));
      Sub(o0, o0, Operand(0x0B));
      if (mode() == LATIN1) {
        BranchOrBacktrack(on_no_match, greater_unsigned, o0,
                          Operand(0x0C - 0x0B));
      } else {
        Label done;
        BranchOrBacktrack(&done, less_equal_unsigned, o0, Operand(0x0C - 0x0B));
        Sub(o0, o0, Operand(0x2028 - 0x0B));
        BranchOrBacktrack(on_no_match, greater_unsigned, o0, Operand(1));
        __ bind(&done);
      }
      break;
    }
    case StandardCharacterSet::kWord: {
      if (mode() != LATIN1) {
        BranchOrBacktrack(on_no_match, greater_unsigned, current_character(),
                          Operand('z'));
      }
      ExternalReference map = ExternalReference::re_word_character_map();
      __ Move(o0, map);
      Add(o0, o0, current_character());
      __ LoadU8(o0, MemOperand(o0, 0));
      BranchOrBacktrack(on_no_match, equal, o0, Operand(g0));
      break;
    }
    case StandardCharacterSet::kNotWord: {
      Label done;
      if (mode() != LATIN1) {
        Branch(&done, greater_unsigned, current_character(), Operand('z'));
      }
      ExternalReference map = ExternalReference::re_word_character_map();
      __ Move(o0, map);
      Add(o0, o0, current_character());
      __ LoadU8(o0, MemOperand(o0, 0));
      BranchOrBacktrack(on_no_match, not_equal, o0, Operand(g0));
      if (mode() != LATIN1) {
        __ bind(&done);
      }
      break;
    }
    case StandardCharacterSet::kEverything:
      break;
    default:
      UNREACHABLE();
  }
}

void RegExpMacroAssemblerSPARC64::Fail() {
  Move(o0, Operand(FAILURE));
  __ jmp(&exit_label_);
}

void RegExpMacroAssemblerSPARC64::LoadRegExpStackPointerFromMemory(
    Register dst) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_stack_pointer(isolate());
  __ Move(dst, ref);
  __ LoadU64(dst, MemOperand(dst));
}

void RegExpMacroAssemblerSPARC64::StoreRegExpStackPointerToMemory(
    Register src, Register scratch) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_stack_pointer(isolate());
  __ Move(scratch, ref);
  __ StoreU64(src, MemOperand(scratch));
}

void RegExpMacroAssemblerSPARC64::PushRegExpBasePointer(Register stack_pointer,
                                                        Register scratch) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ Move(scratch, ref);
  __ LoadU64(scratch, MemOperand(scratch));
  Sub(scratch, stack_pointer, scratch);
  __ StoreU64(scratch,
              MemOperand(frame_pointer(), kRegExpStackBasePointerOffset));
}

void RegExpMacroAssemblerSPARC64::PopRegExpBasePointer(
    Register stack_pointer_out, Register scratch) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  __ LoadU64(stack_pointer_out,
             MemOperand(frame_pointer(), kRegExpStackBasePointerOffset));
  __ Move(scratch, ref);
  __ LoadU64(scratch, MemOperand(scratch));
  Add(stack_pointer_out, stack_pointer_out, scratch);
  StoreRegExpStackPointerToMemory(stack_pointer_out, scratch);
}

DirectHandle<HeapObject> RegExpMacroAssemblerSPARC64::GetCode(
    DirectHandle<String> source, RegExpFlags flags) {
  const int register_bytes = RoundUp(num_registers_ * kSystemPointerSize -
                                         kRegExpStackBasePointerOffset,
                                     16) +
                             kRegExpStackBasePointerOffset;
  Label return_a0;
  __ bind(&entry_label_);

  FrameScope scope(masm_.get(), StackFrame::MANUAL);

  __ EnterCFunctionFrame();
  __ InitializeRootRegister();
  Move(g1, Operand(StackFrame::TypeToMarker(StackFrame::IRREGEXP)));
  __ Push(g1);
  __ LoadU64(g1, MemOperand(i6, kSparcStackBias + kSparcMinimumFrameSize + 8));
  __ Push(g1);  // isolate
  __ LoadS32(g1, MemOperand(i6, kSparcStackBias + kSparcMinimumFrameSize + 4));
  __ Push(g1);  // call origin (low word of its native argument slot)
  __ sra(o5, o5, 0);
  __ sra(o1, o1, 0);
  __ Push(o5, o4, o3, o2, o1, o0);
  __ Push(g0, g0, g0, g0);

  LoadRegExpStackPointerFromMemory(backtrack_stackpointer());
  PushRegExpBasePointer(backtrack_stackpointer(), o1);

  {
    Label stack_limit_hit, stack_ok;

    ExternalReference stack_limit =
        ExternalReference::address_of_jslimit(masm_->isolate());
    Move(o0, stack_limit);
    __ LoadU64(o0, MemOperand(o0));
    __ AddS64(g2, sp, kSparcStackBias);
    Sub(o0, g2, o0);
    Operand extra_space_for_variables(register_bytes);
    Branch(&stack_limit_hit, less_equal, o0, Operand(g0));
    Branch(&stack_ok, greater_equal_unsigned, o0, extra_space_for_variables);
    Move(o0, Operand(EXCEPTION));
    __ jmp(&return_a0);

    __ bind(&stack_limit_hit);
    CallCheckStackGuardState(o0, extra_space_for_variables);
    Branch(&return_a0, not_equal, o0, Operand(g0));

    __ bind(&stack_ok);
  }
  __ AllocateStackSpace(register_bytes);
  __ LoadU64(end_of_input_address(),
             MemOperand(frame_pointer(), kInputEndOffset));
  __ LoadU64(o0, MemOperand(frame_pointer(), kInputStartOffset));
  Sub(current_input_offset(), o0, end_of_input_address());
  __ LoadU64(o1, MemOperand(frame_pointer(), kStartIndexOffset));
  Sub(o0, current_input_offset(), Operand(char_size()));
  __ sllx(g2, o1, (mode() == UC16) ? 1 : 0);
  Sub(o0, o0, g2);
  __ StoreU64(o0, MemOperand(frame_pointer(), kStringStartMinusOneOffset));

  __ Move(code_pointer(), masm_->CodeObject());

  Label load_char_start_regexp;
  {
    Label start_regexp;
    Branch(&load_char_start_regexp, not_equal, o1, Operand(g0));
    Move(current_character(), Operand('\n'));
    __ jmp(&start_regexp);

    __ bind(&load_char_start_regexp);
    LoadCurrentCharacterUnchecked(-1, 1);
    __ bind(&start_regexp);
  }

  if (num_saved_registers_ > 0) {  // Always is, if generated from a regexp.
    if (num_saved_registers_ > 8) {
      Add(o1, frame_pointer(), Operand(kRegisterZeroOffset));
      Move(o2, Operand(num_saved_registers_));
      Label init_loop;
      __ bind(&init_loop);
      __ StoreU64(o0, MemOperand(o1));
      Add(o1, o1, Operand(-kSystemPointerSize));
      Sub(o2, o2, Operand(1));
      Branch(&init_loop, not_equal, o2, Operand(g0));
    } else {
      for (int i = 0; i < num_saved_registers_; i++) {
        __ StoreU64(o0, register_location(i));
      }
    }
  }

  __ jmp(&start_label_);

  if (success_label_.is_linked()) {
    __ bind(&success_label_);
    if (num_saved_registers_ > 0) {
      __ LoadU64(o1, MemOperand(frame_pointer(), kInputStartOffset));
      __ LoadU64(o0, MemOperand(frame_pointer(), kRegisterOutputOffset));
      __ LoadU64(o2, MemOperand(frame_pointer(), kStartIndexOffset));
      Sub(o1, end_of_input_address(), o1);
      if (mode() == UC16) {
        __ srlx(o1, o1, 1);
      }
      Add(o1, o1, Operand(o2));

      DCHECK_EQ(0, num_saved_registers_ % 2);
      for (int i = 0; i < num_saved_registers_; i += 2) {
        __ LoadU64(o2, register_location(i));
        __ LoadU64(o3, register_location(i + 1));
        if (i == 0 && global_with_zero_length_check()) {
          __ Move(l6, o2);
        }
        if (mode() == UC16) {
          __ srax(o2, o2, 1);
          Add(o2, o2, o1);
          __ srax(o3, o3, 1);
          Add(o3, o3, o1);
        } else {
          Add(o2, o1, Operand(o2));
          Add(o3, o1, Operand(o3));
        }
        __ StoreU32(o2, MemOperand(o0));
        Add(o0, o0, kIntSize);
        __ StoreU32(o3, MemOperand(o0));
        Add(o0, o0, kIntSize);
      }
    }

    if (global()) {
      __ LoadU64(o0, MemOperand(frame_pointer(), kSuccessfulCapturesOffset));
      __ LoadU64(o1, MemOperand(frame_pointer(), kNumOutputRegistersOffset));
      __ LoadU64(o2, MemOperand(frame_pointer(), kRegisterOutputOffset));
      Add(o0, o0, 1);
      __ StoreU64(o0, MemOperand(frame_pointer(), kSuccessfulCapturesOffset));
      Sub(o1, o1, num_saved_registers_);
      Branch(&return_a0, less, o1, Operand(num_saved_registers_));

      __ StoreU64(o1, MemOperand(frame_pointer(), kNumOutputRegistersOffset));
      Add(o2, o2, num_saved_registers_ * kIntSize);
      __ StoreU64(o2, MemOperand(frame_pointer(), kRegisterOutputOffset));

      PopRegExpBasePointer(backtrack_stackpointer(), o2);

      Label reload_string_start_minus_one;

      if (global_with_zero_length_check()) {
        Branch(&reload_string_start_minus_one, not_equal,
               current_input_offset(), Operand(l6));
        Branch(&exit_label_, equal, current_input_offset(), Operand(g0));
        Label advance;
        __ bind(&advance);
        Add(current_input_offset(), current_input_offset(),
            Operand((mode() == UC16) ? 2 : 1));
        if (global_unicode()) CheckNotInSurrogatePair(0, &advance);
      }

      __ bind(&reload_string_start_minus_one);
      __ LoadU64(o0, MemOperand(frame_pointer(), kStringStartMinusOneOffset));

      __ Jump(&load_char_start_regexp);
    } else {
      Move(o0, Operand(SUCCESS));
    }
  }
  __ bind(&exit_label_);
  if (global()) {
    __ LoadU64(o0, MemOperand(frame_pointer(), kSuccessfulCapturesOffset));
  }

  __ bind(&return_a0);
  PopRegExpBasePointer(backtrack_stackpointer(), o1);
  __ RetCFunction();

  if (backtrack_label_.is_linked()) {
    __ bind(&backtrack_label_);
    Backtrack();
  }

  Label exit_with_exception;

  if (check_preempt_label_.is_linked()) {
    SafeCallTarget(&check_preempt_label_);
    StoreRegExpStackPointerToMemory(backtrack_stackpointer(), o1);
    CallCheckStackGuardState(o0);
    Branch(&return_a0, not_equal, o0, Operand(g0));
    LoadRegExpStackPointerFromMemory(backtrack_stackpointer());
    __ LoadU64(end_of_input_address(),
               MemOperand(frame_pointer(), kInputEndOffset));
    SafeReturn();
  }

  if (stack_overflow_label_.is_linked()) {
    SafeCallTarget(&stack_overflow_label_);
    StoreRegExpStackPointerToMemory(backtrack_stackpointer(), o1);

    static constexpr int kNumArguments = 1;
    __ PrepareCallCFunction(kNumArguments, o0);
    Move(o0, ExternalReference::isolate_address(isolate()));
    ExternalReference grow_stack = ExternalReference::re_grow_stack();
    CallCFunctionFromIrregexpCode(grow_stack, kNumArguments);
    Branch(&exit_with_exception, equal, o0, Operand(g0));
    __ Move(backtrack_stackpointer(), o0);
    SafeReturn();
  }

  if (exit_with_exception.is_linked()) {
    __ bind(&exit_with_exception);
    Move(o0, Operand(EXCEPTION));
    __ jmp(&return_a0);
  }

  if (fallback_label_.is_linked()) {
    __ bind(&fallback_label_);
    Move(o0, Operand(FALLBACK_TO_EXPERIMENTAL));
    __ jmp(&return_a0);
  }

  CodeDesc code_desc;
  masm_->GetCode(isolate(), &code_desc);
  Handle<Code> code =
      Factory::CodeBuilder(isolate(), code_desc, CodeKind::REGEXP)
          .set_self_reference(masm_->CodeObject())
          .set_empty_source_position_table()
          .Build();
  if (v8_flags.print_regexp_code) {
    Print(*code);
  }
  LOG(masm_->isolate(),
      RegExpCodeCreateEvent(Cast<AbstractCode>(code), source, flags));
  return Cast<HeapObject>(code);
}

void RegExpMacroAssemblerSPARC64::GoTo(Label* to) {
  if (to == nullptr) {
    Backtrack();
    return;
  }
  __ jmp(to);
  return;
}

void RegExpMacroAssemblerSPARC64::IfRegisterGE(int reg, int comparand,
                                               Label* if_ge) {
  __ LoadU64(o0, register_location(reg));
  BranchOrBacktrack(if_ge, greater_equal, o0, Operand(comparand));
}

void RegExpMacroAssemblerSPARC64::IfRegisterLT(int reg, int comparand,
                                               Label* if_lt) {
  __ LoadU64(o0, register_location(reg));
  BranchOrBacktrack(if_lt, less, o0, Operand(comparand));
}

void RegExpMacroAssemblerSPARC64::IfRegisterEqPos(int reg, Label* if_eq) {
  __ LoadU64(o0, register_location(reg));
  BranchOrBacktrack(if_eq, equal, o0, Operand(current_input_offset()));
}

RegExpMacroAssembler::IrregexpImplementation
RegExpMacroAssemblerSPARC64::Implementation() {
  return kSPARC64Implementation;
}

void RegExpMacroAssemblerSPARC64::PopCurrentPosition() {
  Pop(current_input_offset());
}

void RegExpMacroAssemblerSPARC64::PopRegister(int register_index) {
  Pop(o0);
  __ StoreU64(o0, register_location(register_index));
}

void RegExpMacroAssemblerSPARC64::PushBacktrack(Label* label) {
  __ LoadLabelOffset(o0, label);
  __ AddS64(o0, o0, InstructionStream::kHeaderSize - kHeapObjectTag);
  Push(o0);
  CheckStackLimit();
}

void RegExpMacroAssemblerSPARC64::PushCurrentPosition() {
  Push(current_input_offset());
  CheckStackLimit();
}

void RegExpMacroAssemblerSPARC64::PushRegister(
    int register_index, StackCheckFlag check_stack_limit) {
  __ LoadU64(o0, register_location(register_index));
  Push(o0);
  if (check_stack_limit == StackCheckFlag::kCheckStackLimit) {
    CheckStackLimit();
  } else if (V8_UNLIKELY(v8_flags.slow_debug_code)) {
    AssertAboveStackLimitMinusSlack();
  }
}

void RegExpMacroAssemblerSPARC64::ReadCurrentPositionFromRegister(int reg) {
  __ LoadU64(current_input_offset(), register_location(reg));
}

void RegExpMacroAssemblerSPARC64::WriteStackPointerToRegister(int reg) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  Move(o0, ref);
  __ LoadU64(o0, MemOperand(o0));
  Sub(o0, backtrack_stackpointer(), o0);
  __ StoreU64(o0, register_location(reg));
}

void RegExpMacroAssemblerSPARC64::ReadStackPointerFromRegister(int reg) {
  ExternalReference ref =
      ExternalReference::address_of_regexp_stack_memory_top_address(isolate());
  Move(o1, ref);
  __ LoadU64(o1, MemOperand(o1));
  __ LoadU64(backtrack_stackpointer(), register_location(reg));
  Add(backtrack_stackpointer(), backtrack_stackpointer(), o1);
}

void RegExpMacroAssemblerSPARC64::SetCurrentPositionFromEnd(int by) {
  Label after_position;
  Branch(&after_position, greater_equal, current_input_offset(),
         Operand(-by * char_size()));
  Move(current_input_offset(), -by * char_size());
  LoadCurrentCharacterUnchecked(-1, 1);
  __ bind(&after_position);
}

void RegExpMacroAssemblerSPARC64::SetRegister(int register_index, int to) {
  DCHECK(register_index >= num_saved_registers_);  // Reserved for positions!
  Move(o0, Operand(to));
  __ StoreU64(o0, register_location(register_index));
}

bool RegExpMacroAssemblerSPARC64::Succeed() {
  __ jmp(&success_label_);
  return global();
}

void RegExpMacroAssemblerSPARC64::WriteCurrentPositionToRegister(
    int reg, int cp_offset) {
  if (cp_offset == 0) {
    __ StoreU64(current_input_offset(), register_location(reg));
  } else {
    Add(o0, current_input_offset(), Operand(cp_offset * char_size()));
    __ StoreU64(o0, register_location(reg));
  }
}

void RegExpMacroAssemblerSPARC64::ClearRegisters(int reg_from, int reg_to) {
  DCHECK(reg_from <= reg_to);
  __ LoadU64(o0, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
  for (int reg = reg_from; reg <= reg_to; reg++) {
    __ StoreU64(o0, register_location(reg));
  }
}

void RegExpMacroAssemblerSPARC64::CallCheckStackGuardState(
    Register scratch, Operand extra_space) {
  DCHECK(!isolate()->IsGeneratingEmbeddedBuiltins());
  DCHECK(!masm_->options().isolate_independent_code);

  __ AllocateStackSpace(16);
  __ LoadStackAddress(o0);
  __ Move(o1, masm_->CodeObject());
  __ Move(o2, frame_pointer());
  Move(o3, extra_space);
  __ Move(g5, ExternalReference::re_check_stack_guard_state());
  __ Move(g4, 4);
  EmbeddedData d = EmbeddedData::FromBlob();
  Address entry = d.InstructionStartOf(Builtin::kDirectCEntry);
  __ LoadLiteral(g1, entry, RelocInfo::OFF_HEAP_TARGET);
  __ Call(g1);
  __ Drop(2);
  __ Move(code_pointer(), masm_->CodeObject());
}

template <typename T>
static T& frame_entry(Address re_frame, int frame_offset) {
  return *reinterpret_cast<T*>(re_frame + frame_offset);
}

template <typename T>
static T* frame_entry_address(Address re_frame, int frame_offset) {
  return reinterpret_cast<T*>(re_frame + frame_offset);
}

int64_t RegExpMacroAssemblerSPARC64::CheckStackGuardState(
    Address* return_address, Address raw_code, Address re_frame,
    uintptr_t extra_space) {
  Tagged<InstructionStream> re_code =
      SbxCast<InstructionStream>(Tagged<Object>(raw_code));
  return NativeRegExpMacroAssembler::CheckStackGuardState(
      frame_entry<Isolate*>(re_frame, kIsolateOffset),
      static_cast<int>(frame_entry<int64_t>(re_frame, kStartIndexOffset)),
      static_cast<RegExp::CallOrigin>(
          frame_entry<int64_t>(re_frame, kDirectCallOffset)),
      return_address, re_code,
      frame_entry_address<Address>(re_frame, kInputStringOffset),
      frame_entry_address<const uint8_t*>(re_frame, kInputStartOffset),
      frame_entry_address<const uint8_t*>(re_frame, kInputEndOffset),
      extra_space);
}

MemOperand RegExpMacroAssemblerSPARC64::register_location(int register_index) {
  DCHECK(register_index < (1 << 30));
  if (num_registers_ <= register_index) {
    num_registers_ = register_index + 1;
  }
  return MemOperand(frame_pointer(),
                    kRegisterZeroOffset - register_index * kSystemPointerSize);
}

void RegExpMacroAssemblerSPARC64::CheckPosition(int cp_offset,
                                                Label* on_outside_input) {
  if (cp_offset >= 0) {
    BranchOrBacktrack(on_outside_input, greater_equal, current_input_offset(),
                      Operand(-cp_offset * char_size()));
  } else {
    __ LoadU64(o1, MemOperand(frame_pointer(), kStringStartMinusOneOffset));
    Add(o0, current_input_offset(), Operand(cp_offset * char_size()));
    BranchOrBacktrack(on_outside_input, less_equal, o0, Operand(o1));
  }
}

void RegExpMacroAssemblerSPARC64::BranchOrBacktrack(Label* to,
                                                    Condition condition,
                                                    Register rs,
                                                    const Operand& rt) {
  if (condition == always) {  // Unconditional.
    if (to == nullptr) {
      Backtrack();
      return;
    }
    __ jmp(to);
    return;
  }
  if (to == nullptr) {
    Branch(&backtrack_label_, condition, rs, rt);
    return;
  }
  Branch(to, condition, rs, rt);
}

void RegExpMacroAssemblerSPARC64::SafeCall(Label* to, Condition cond,
                                           Register rs, const Operand& rt) {
  Label skip;
  Branch(&skip, static_cast<Condition>(cond ^ 8), rs, rt);
  __ call(to);
  __ nop();
  __ bind(&skip);
}

void RegExpMacroAssemblerSPARC64::SafeReturn() {
  __ LoadU64(g2, MemOperand(sp));
  __ Drop(2);
  __ add(g2, g2, code_pointer());
  __ Jump(g2);
}

void RegExpMacroAssemblerSPARC64::SafeCallTarget(Label* name) {
  __ bind(name);
  __ add(g2, o7, kSparcReturnAddressOffset);
  __ sub(g2, g2, code_pointer());
  __ AllocateStackSpace(16);
  __ StoreU64(g2, MemOperand(sp));
}

void RegExpMacroAssemblerSPARC64::Push(Register source) {
  DCHECK(source != backtrack_stackpointer());
  Add(backtrack_stackpointer(), backtrack_stackpointer(), Operand(-kIntSize));
  __ StoreU32(source, MemOperand(backtrack_stackpointer()));
}

void RegExpMacroAssemblerSPARC64::Pop(Register target) {
  DCHECK(target != backtrack_stackpointer());
  __ LoadS32(target, MemOperand(backtrack_stackpointer()));
  Add(backtrack_stackpointer(), backtrack_stackpointer(), kIntSize);
}

void RegExpMacroAssemblerSPARC64::CheckPreemption() {
  ExternalReference stack_limit =
      ExternalReference::address_of_jslimit(masm_->isolate());
  Move(o0, stack_limit);
  __ LoadU64(o0, MemOperand(o0));
  __ AddS64(g2, sp, kSparcStackBias);
  SafeCall(&check_preempt_label_, less_equal_unsigned, g2, Operand(o0));
}

void RegExpMacroAssemblerSPARC64::CheckStackLimit() {
  ExternalReference stack_limit =
      ExternalReference::address_of_regexp_stack_limit_address(
          masm_->isolate());

  Move(o0, stack_limit);
  __ LoadU64(o0, MemOperand(o0));
  SafeCall(&stack_overflow_label_, less_equal_unsigned,
           backtrack_stackpointer(), Operand(o0));
}

void RegExpMacroAssemblerSPARC64::AssertAboveStackLimitMinusSlack() {
  DCHECK(v8_flags.slow_debug_code);
  Label no_stack_overflow;
  ASM_CODE_COMMENT_STRING(masm_.get(), "AssertAboveStackLimitMinusSlack");
  auto l = ExternalReference::address_of_regexp_stack_limit_address(isolate());
  Move(o0, l);
  __ LoadU64(o0, MemOperand(o0, 0));
  Sub(o0, o0, Operand(RegExpStack::kStackLimitSlackSize));
  Branch(&no_stack_overflow, greater_unsigned, backtrack_stackpointer(),
         Operand(o0));
  __ DebugBreak();
  __ bind(&no_stack_overflow);
}

void RegExpMacroAssemblerSPARC64::LoadCurrentCharacterUnchecked(
    int cp_offset, int characters) {
  DCHECK_EQ(1, characters);
  __ AddS64(g2, current_input_offset(), cp_offset * char_size());
  __ add(g2, g2, end_of_input_address());
  if (mode() == LATIN1) {
    __ LoadU8(current_character(), MemOperand(g2));
  } else {
    __ LoadU16(current_character(), MemOperand(g2));
  }
}

void RegExpMacroAssemblerSPARC64::CallCFunctionFromIrregexpCode(
    ExternalReference function, int num_arguments) {
  FrameScope frame(masm_.get(), StackFrame::MANUAL);
  __ CallCFunction(function, num_arguments, SetIsolateDataSlots::kNo);
}
void RegExpMacroAssemblerSPARC64::Move(Register dst, Operand src) {
  if (src.is_reg()) {
    __ Move(dst, src.rm());
  } else {
    __ Move(dst, src.immediate());
  }
}

void RegExpMacroAssemblerSPARC64::Add(Register dst, Register lhs, Operand rhs) {
  if (rhs.is_reg()) {
    __ add(dst, lhs, rhs.rm());
  } else {
    __ AddS64(dst, lhs, rhs.immediate());
  }
}

void RegExpMacroAssemblerSPARC64::Sub(Register dst, Register lhs, Operand rhs) {
  if (rhs.is_reg()) {
    __ sub(dst, lhs, rhs.rm());
  } else {
    __ SubS64(dst, lhs, rhs.immediate());
  }
}

void RegExpMacroAssemblerSPARC64::And(Register dst, Register lhs, Operand rhs) {
  Move(g5, rhs);
  __ and_(dst, lhs, g5);
}

void RegExpMacroAssemblerSPARC64::Or(Register dst, Register lhs, Operand rhs) {
  Move(g5, rhs);
  __ or_(dst, lhs, g5);
}

void RegExpMacroAssemblerSPARC64::Xor(Register dst, Register lhs, Operand rhs) {
  Move(g5, rhs);
  __ xor_(dst, lhs, g5);
}

void RegExpMacroAssemblerSPARC64::Branch(Label* to, Condition condition,
                                         Register lhs, Operand rhs) {
  if (condition == always) {
    __ Jump(to);
    return;
  }
  Move(g5, rhs);
  __ cmp(lhs, g5);
  Label skip;
  __ b(static_cast<Condition>(condition ^ 8), ConditionCodes::kXcc, &skip);
  __ nop();
  __ Jump(to);
  __ bind(&skip);
}

#undef __

}  // namespace internal
}  // namespace v8
