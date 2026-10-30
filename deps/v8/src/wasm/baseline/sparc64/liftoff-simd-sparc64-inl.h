// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef V8_WASM_BASELINE_SPARC64_LIFTOFF_SIMD_SPARC64_INL_H_
#define V8_WASM_BASELINE_SPARC64_LIFTOFF_SIMD_SPARC64_INL_H_
#include "src/codegen/sparc64/simd-sparc64-inl.h"
namespace v8::internal::wasm::liftoff {
using sparc64::SimdHalf;
using sparc64::SimdOffset;
using sparc64::LoadSimd;
using sparc64::StoreSimd;
using sparc64::MoveSimd;
using sparc64::TruncateLane;
using sparc64::ReadLane;
using sparc64::ReplaceLane;
using sparc64::Splat;
using sparc64::SimdBinaryOp;
using sparc64::SimdBinary;
using sparc64::SimdLaneOffset;
using sparc64::ReadStackLane;
using sparc64::WriteStackLane;
using sparc64::ClampSimdLane;
using sparc64::SimdTransformOp;
using sparc64::TransformSimd;
using sparc64::SimdSwizzle;
using sparc64::SimdDotAdd;
}  // namespace v8::internal::wasm::liftoff
#endif  // V8_WASM_BASELINE_SPARC64_LIFTOFF_SIMD_SPARC64_INL_H_
