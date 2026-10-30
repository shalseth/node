// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/codegen/register.h"
#include "src/execution/frame-constants.h"
#include "src/execution/frames.h"

namespace v8::internal {

Register JavaScriptFrame::fp_register() { return v8::internal::fp; }
Register JavaScriptFrame::context_register() { return kContextRegister; }
Register JavaScriptFrame::constant_pool_pointer_register() { UNREACHABLE(); }

int UnoptimizedFrameConstants::RegisterStackSlotCount(int register_count) {
  return register_count;
}

int BuiltinContinuationFrameConstants::PaddingSlotCount(int register_count) {
  return 0;
}

intptr_t MaglevFrame::StackGuardFrameSize(int register_input_count) {
  return StandardFrameConstants::kFixedFrameSizeFromFp +
         (1 + register_input_count) * kSystemPointerSize;
}

}  // namespace v8::internal
