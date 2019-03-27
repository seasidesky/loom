/*
 * Copyright (c) 1997, 2017, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "code/debugInfo.hpp"
#include "oops/compressedOops.inline.hpp"
#include "oops/oop.hpp"
#include "runtime/frame.inline.hpp"
#include "runtime/handles.inline.hpp"
#include "runtime/stackValue.hpp"
#if INCLUDE_ZGC
#include "gc/z/zBarrier.inline.hpp"
#endif

address StackValue::stack_value_address(const frame* fr, const RegisterMap* reg_map, ScopeValue* sv) {
  if (!sv->is_location() || reg_map->in_cont())
    return NULL;
  Location loc = ((LocationValue *)sv)->location();
  if (loc.type() == Location::invalid)
    return NULL;
  address value_addr = loc.is_register()     // see create_stack_value
      ? reg_map->location(VMRegImpl::as_VMReg(loc.register_number()))
      : ((address)fr->unextended_sp()) + loc.stack_offset();
  assert(value_addr == NULL || reg_map->thread()->is_in_usable_stack(value_addr), INTPTR_FORMAT, p2i(value_addr)); 
  return value_addr;
}

StackValue* StackValue::create_stack_value(const frame* fr, const RegisterMap* reg_map, ScopeValue* sv) {
  if (sv->is_location()) {
    // Stack or register value
    Location loc = ((LocationValue *)sv)->location();

#ifdef SPARC
    // %%%%% Callee-save floats will NOT be working on a Sparc until we
    // handle the case of a 2 floats in a single double register.
    assert( !(loc.is_register() && loc.type() == Location::float_in_dbl), "Sparc does not handle callee-save floats yet" );
#endif // SPARC

    // First find address of value

    address value_addr;
    if (!reg_map->in_cont()) {
      value_addr = loc.is_register()
      // Value was in a callee-save register
        ? reg_map->location(VMRegImpl::as_VMReg(loc.register_number()))
      // Else value was directly saved on the stack. The frame's original stack pointer,
      // before any extension by its callee (due to Compiler1 linkage on SPARC), must be used.
        : ((address)fr->unextended_sp()) + loc.stack_offset();

      assert(reg_map->thread()->is_in_usable_stack(value_addr), INTPTR_FORMAT, p2i(value_addr));
    } else {
      if (loc.type() == Location::invalid)
        return new StackValue();
      value_addr = loc.is_register()
      // Value was in a callee-save register
        ? Continuation::reg_to_location(*fr, reg_map, VMRegImpl::as_VMReg(loc.register_number()), loc.type() == Location::oop || loc.type() == Location::narrowoop)
      // Else value was directly saved on the stack. The frame's original stack pointer,
      // before any extension by its callee (due to Compiler1 linkage on SPARC), must be used.
        : Continuation::usp_offset_to_location(*fr, reg_map, loc.stack_offset(), loc.type() == Location::oop || loc.type() == Location::narrowoop);
      
      assert(Continuation::is_in_usable_stack(value_addr, reg_map), INTPTR_FORMAT, p2i(value_addr)); 
    }

    // Then package it right depending on type
    // Note: the transfer of the data is thru a union that contains
    // an intptr_t. This is because an interpreter stack slot is
    // really an intptr_t. The use of a union containing an intptr_t
    // ensures that on a 64 bit platform we have proper alignment
    // and that we store the value where the interpreter will expect
    // to find it (i.e. proper endian). Similarly on a 32bit platform
    // using the intptr_t ensures that when a value is larger than
    // a stack slot (jlong/jdouble) that we capture the proper part
    // of the value for the stack slot in question.
    //
    switch( loc.type() ) {
    case Location::float_in_dbl: { // Holds a float in a double register?
      // The callee has no clue whether the register holds a float,
      // double or is unused.  He always saves a double.  Here we know
      // a double was saved, but we only want a float back.  Narrow the
      // saved double to the float that the JVM wants.
      assert( loc.is_register(), "floats always saved to stack in 1 word" );
      union { intptr_t p; jfloat jf; } value;
      value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
      value.jf = (jfloat) *(jdouble*) value_addr;
      return new StackValue(value.p); // 64-bit high half is stack junk
    }
    case Location::int_in_long: { // Holds an int in a long register?
      // The callee has no clue whether the register holds an int,
      // long or is unused.  He always saves a long.  Here we know
      // a long was saved, but we only want an int back.  Narrow the
      // saved long to the int that the JVM wants.
      assert( loc.is_register(), "ints always saved to stack in 1 word" );
      union { intptr_t p; jint ji;} value;
      value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
      value.ji = (jint) *(jlong*) value_addr;
      return new StackValue(value.p); // 64-bit high half is stack junk
    }
#ifdef _LP64
    case Location::dbl:
      // Double value in an aligned adjacent pair
      return new StackValue(*(intptr_t*)value_addr);
    case Location::lng:
      // Long   value in an aligned adjacent pair
      return new StackValue(*(intptr_t*)value_addr);
    case Location::narrowoop: {
      assert (UseCompressedOops, "");
      union { intptr_t p; narrowOop noop;} value;
      value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
      if (loc.is_register()) {
        // The callee has no clue whether the register holds an int,
        // long or is unused.  He always saves a long.  Here we know
        // a long was saved, but we only want an int back.  Narrow the
        // saved long to the int that the JVM wants.
        value.noop =  (narrowOop) *(julong*) value_addr;
      } else {
        value.noop = *(narrowOop*) value_addr;
      }
      // Decode narrowoop and wrap a handle around the oop
      Handle h(Thread::current(), CompressedOops::decode(value.noop));
      return new StackValue(h);
    }
#endif
    case Location::oop: {
      if (reg_map->in_cont() && UseCompressedOops) {
        narrowOop noop = *(narrowOop*) value_addr;
        Handle h(Thread::current(), CompressedOops::decode(noop));
        return new StackValue(h);
      } 
      
      oop val = *(oop *)value_addr;
#ifdef _LP64
      if (Universe::is_narrow_oop_base(val)) {
         // Compiled code may produce decoded oop = narrow_oop_base
         // when a narrow oop implicit null check is used.
         // The narrow_oop_base could be NULL or be the address
         // of the page below heap. Use NULL value for both cases.
         val = (oop)NULL;
      }
#endif
#if INCLUDE_ZGC
      // Deoptimization must make sure all oop have passed load barrier
      if (UseZGC) {
        val = ZBarrier::load_barrier_on_oop_field_preloaded((oop*)value_addr, val);
      }
#endif

      Handle h(Thread::current(), val); // Wrap a handle around the oop
      return new StackValue(h);
    }
    case Location::addr: {
      ShouldNotReachHere(); // both C1 and C2 now inline jsrs
    }
    case Location::normal: {
      // Just copy all other bits straight through
      union { intptr_t p; jint ji;} value;
      value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
      value.ji = *(jint*)value_addr;
      return new StackValue(value.p);
    }
    case Location::invalid:
      return new StackValue();
    default:
      ShouldNotReachHere();
    }

  } else if (sv->is_constant_int()) {
    // Constant int: treat same as register int.
    union { intptr_t p; jint ji;} value;
    value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
    value.ji = (jint)((ConstantIntValue*)sv)->value();
    return new StackValue(value.p);
  } else if (sv->is_constant_oop()) {
    // constant oop
    return new StackValue(sv->as_ConstantOopReadValue()->value());
#ifdef _LP64
  } else if (sv->is_constant_double()) {
    // Constant double in a single stack slot
    union { intptr_t p; double d; } value;
    value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
    value.d = ((ConstantDoubleValue *)sv)->value();
    return new StackValue(value.p);
  } else if (sv->is_constant_long()) {
    // Constant long in a single stack slot
    union { intptr_t p; jlong jl; } value;
    value.p = (intptr_t) CONST64(0xDEADDEAFDEADDEAF);
    value.jl = ((ConstantLongValue *)sv)->value();
    return new StackValue(value.p);
#endif
  } else if (sv->is_object()) { // Scalar replaced object in compiled frame
    Handle ov = ((ObjectValue *)sv)->value();
    return new StackValue(ov, (ov.is_null()) ? 1 : 0);
  }

  // Unknown ScopeValue type
  ShouldNotReachHere();
  return new StackValue((intptr_t) 0);   // dummy
}


BasicLock* StackValue::resolve_monitor_lock(const frame* fr, Location location) {
  assert(location.is_stack(), "for now we only look at the stack");
  int word_offset = location.stack_offset() / wordSize;
  // (stack picture)
  // high: [     ]  word_offset + 1
  // low   [     ]  word_offset
  //
  // sp->  [     ]  0
  // the word_offset is the distance from the stack pointer to the lowest address
  // The frame's original stack pointer, before any extension by its callee
  // (due to Compiler1 linkage on SPARC), must be used.
  return (BasicLock*) (fr->unextended_sp() + word_offset);
}


#ifndef PRODUCT

void StackValue::print_on(outputStream* st) const {
  switch(_type) {
    case T_INT:
      st->print("%d (int) %f (float) %x (hex)",  *(int *)&_integer_value, *(float *)&_integer_value,  *(int *)&_integer_value);
      break;

    case T_OBJECT:
      if (_handle_value() != NULL) {
        _handle_value()->print_value_on(st);
      } else {
        st->print("NULL");
      }
      st->print(" <" INTPTR_FORMAT ">", p2i((address)_handle_value()));
     break;

    case T_CONFLICT:
     st->print("conflict");
     break;

    default:
     ShouldNotReachHere();
  }
}

#endif
