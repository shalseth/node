// Copyright 2026 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_INL_H_
#define V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_INL_H_

#include "src/codegen/flush-instruction-cache.h"
#include "src/codegen/sparc64/assembler-sparc64.h"
#include "src/common/code-memory-access-inl.h"
#include "src/objects/objects-inl.h"

namespace v8::internal {

bool CpuFeatures::SupportsOptimizer() { return true; }

void WritableRelocInfo::apply(intptr_t delta) {
  DCHECK(IsInternalReference(rmode_));
  Assembler::set_target_address_at(
      pc_, constant_pool_,
      Assembler::target_address_at(pc_, constant_pool_) + delta,
      &jit_allocation_, SKIP_ICACHE_FLUSH);
}

Address RelocInfo::target_address() {
  DCHECK(IsCodeTargetMode(rmode_) || IsWasmCall(rmode_) ||
         IsWasmStubCall(rmode_));
  return Assembler::target_address_at(pc_, constant_pool_);
}

Address RelocInfo::target_address_address() {
  DCHECK(HasTargetAddressAddress());
  return pc_;
}

int RelocInfo::target_address_size() { return kSystemPointerSize; }

Address RelocInfo::constant_pool_entry_address() { UNREACHABLE(); }

Tagged<HeapObject> RelocInfo::target_object(PtrComprCageBase cage_base) {
  DCHECK(IsFullEmbeddedObject(rmode_));
  return Cast<HeapObject>(
      Tagged<Object>(Assembler::target_address_at(pc_, constant_pool_)));
}

DirectHandle<HeapObject> RelocInfo::target_object_handle(Assembler* origin) {
  DCHECK(IsFullEmbeddedObject(rmode_) || IsCodeTargetMode(rmode_));
  return DirectHandle<HeapObject>::FromSlot(reinterpret_cast<Address*>(
      Assembler::target_address_at(pc_, constant_pool_)));
}

void WritableRelocInfo::set_target_object(Tagged<HeapObject> target,
                                          ICacheFlushMode icache_flush_mode) {
  DCHECK(IsFullEmbeddedObject(rmode_));
  Assembler::set_target_address_at(pc_, constant_pool_, target.ptr(),
                                   &jit_allocation_, icache_flush_mode);
}

Address RelocInfo::target_external_reference() {
  DCHECK(IsExternalReference(rmode_));
  return Assembler::target_address_at(pc_, constant_pool_);
}

void WritableRelocInfo::set_target_external_reference(
    Address target, ICacheFlushMode icache_flush_mode) {
  DCHECK(IsExternalReference(rmode_));
  Assembler::set_target_address_at(pc_, constant_pool_, target,
                                   &jit_allocation_, icache_flush_mode);
}

Address RelocInfo::target_internal_reference() {
  DCHECK(IsInternalReference(rmode_));
  return Assembler::target_address_at(pc_, constant_pool_);
}

Address RelocInfo::target_internal_reference_address() {
  DCHECK(IsInternalReference(rmode_));
  return pc_;
}

Address RelocInfo::target_off_heap_target() {
  DCHECK(IsOffHeapTarget(rmode_));
  return Assembler::target_address_at(pc_, constant_pool_);
}

Builtin RelocInfo::target_builtin_at(Assembler* origin) { UNREACHABLE(); }

WasmCodePointer RelocInfo::wasm_code_pointer_table_entry() const {
  DCHECK_EQ(rmode_, WASM_CODE_POINTER_TABLE_ENTRY);
  return WasmCodePointer{Assembler::uint32_constant_at(pc_, constant_pool_)};
}

void WritableRelocInfo::set_wasm_code_pointer_table_entry(
    WasmCodePointer target, ICacheFlushMode icache_flush_mode) {
  DCHECK_EQ(rmode_, WASM_CODE_POINTER_TABLE_ENTRY);
  Assembler::set_uint32_constant_at(pc_, constant_pool_, target.value(),
                                    &jit_allocation_, icache_flush_mode);
}

JSDispatchHandle RelocInfo::js_dispatch_handle() {
  DCHECK_EQ(rmode_, JS_DISPATCH_HANDLE);
  return JSDispatchHandle(Assembler::uint32_constant_at(pc_, constant_pool_));
}

}  // namespace v8::internal

#endif  // V8_CODEGEN_SPARC64_ASSEMBLER_SPARC64_INL_H_
