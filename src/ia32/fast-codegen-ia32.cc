// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include "v8.h"

#include "codegen-inl.h"
#include "fast-codegen.h"

namespace v8 {
namespace internal {

#define __ ACCESS_MASM(masm())

void FastCodeGenerator::EmitLoadReceiver(Register reg) {
  // Offset 2 is due to return address and saved frame pointer.
  int index = 2 + function()->scope()->num_parameters();
  __ mov(reg, Operand(ebp, index * kPointerSize));
}


void FastCodeGenerator::EmitReceiverMapCheck() {
  Comment cmnt(masm(), ";; MapCheck(this)");
  if (FLAG_print_ir) {
    PrintF("MapCheck(this)\n");
  }

  ASSERT(info()->has_receiver() && info()->receiver()->IsHeapObject());
  Handle<HeapObject> object = Handle<HeapObject>::cast(info()->receiver());
  Handle<Map> map(object->map());

  EmitLoadReceiver(edx);
  __ CheckMap(edx, map, bailout(), false);
}


void FastCodeGenerator::EmitGlobalMapCheck() {
  Comment cmnt(masm(), ";; GlobalMapCheck");
  if (FLAG_print_ir) {
    PrintF(";; GlobalMapCheck()");
  }

  ASSERT(info()->has_global_object());
  Handle<Map> map(info()->global_object()->map());

  __ mov(ebx, CodeGenerator::GlobalObject());
  __ CheckMap(ebx, map, bailout(), true);
}


void FastCodeGenerator::EmitGlobalVariableLoad(Handle<Object> cell) {
  ASSERT(cell->IsJSGlobalPropertyCell());
  __ mov(eax, Immediate(cell));
  __ mov(eax, FieldOperand(eax, JSGlobalPropertyCell::kValueOffset));
  if (FLAG_debug_code) {
    __ cmp(eax, Factory::the_hole_value());
    __ Check(not_equal, "DontDelete cells can't contain the hole");
  }
}


void FastCodeGenerator::EmitThisPropertyStore(Handle<String> name) {
  LookupResult lookup;
  info()->receiver()->Lookup(*name, &lookup);

  ASSERT(lookup.holder() == *info()->receiver());
  ASSERT(lookup.type() == FIELD);
  Handle<Map> map(Handle<HeapObject>::cast(info()->receiver())->map());
  int index = lookup.GetFieldIndex() - map->inobject_properties();
  int offset = index * kPointerSize;

  // Negative offsets are inobject properties.
  if (offset < 0) {
    offset += map->instance_size();
    __ mov(ecx, edx);  // Copy receiver for write barrier.
  } else {
    offset += FixedArray::kHeaderSize;
    __ mov(ecx, FieldOperand(edx, JSObject::kPropertiesOffset));
  }
  // Perform the store.
  __ mov(FieldOperand(ecx, offset), eax);
  // Preserve value from write barrier in case it's needed.
  __ mov(ebx, eax);
  __ RecordWrite(ecx, offset, ebx, edi);
}


void FastCodeGenerator::Generate(CompilationInfo* compilation_info) {
  ASSERT(info_ == NULL);
  info_ = compilation_info;

  // Save the caller's frame pointer and set up our own.
  Comment prologue_cmnt(masm(), ";; Prologue");
  __ push(ebp);
  __ mov(ebp, esp);
  __ push(esi);  // Context.
  __ push(edi);  // Closure.
  // Note that we keep a live register reference to esi (context) at this
  // point.

  // Receiver (this) is allocated to edx if there are this properties.
  if (info()->has_this_properties()) EmitReceiverMapCheck();

  // If there is a global variable access check if the global object
  // is the same as at lazy-compilation time.
  if (info()->has_globals()) EmitGlobalMapCheck();

  VisitStatements(function()->body());

  Comment return_cmnt(masm(), ";; Return(<undefined>)");
  __ mov(eax, Factory::undefined_value());

  Comment epilogue_cmnt(masm(), ";; Epilogue");
  __ mov(esp, ebp);
  __ pop(ebp);
  __ ret((scope()->num_parameters() + 1) * kPointerSize);

  __ bind(&bailout_);
}


#undef __


} }  // namespace v8::internal