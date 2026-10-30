// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/backend/instruction-scheduler.h"

namespace v8::internal::compiler {

// Keep compound instruction sequences in selection order during bring-up.
bool InstructionScheduler::SchedulerSupported() { return false; }

int InstructionScheduler::GetTargetInstructionFlags(
    const Instruction* instr) const {
  return kIsBarrier;
}

int InstructionScheduler::GetInstructionLatency(const Instruction* instr) {
  return 1;
}

}  // namespace v8::internal::compiler
