/*
 * Copyright (c) 1999, 2013, Oracle and/or its affiliates. All rights reserved.
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

#ifndef SHARE_VM_CI_CIFLAGS_HPP
#define SHARE_VM_CI_CIFLAGS_HPP

#include "ci/ciClassList.hpp"
#include "memory/allocation.hpp"
#include "prims/jvm.h"
#include "utilities/accessFlags.hpp"
#include "runtime/dsuFlags.hpp"

// ciFlags
//
// This class represents klass or method flags.
class ciFlags VALUE_OBJ_CLASS_SPEC {
private:
  friend class ciInstanceKlass;
  friend class ciField;
  friend class ciMethod;

  jint _flags;

  ciFlags()                  { _flags = 0; }
  ciFlags(AccessFlags flags) { _flags = flags.as_int(); }

public:
  // Java access flags
  bool is_public      () const         { return (_flags & JVM_ACC_PUBLIC      ) != 0; }
  bool is_private     () const         { return (_flags & JVM_ACC_PRIVATE     ) != 0; }
  bool is_protected   () const         { return (_flags & JVM_ACC_PROTECTED   ) != 0; }
  bool is_static      () const         { return (_flags & JVM_ACC_STATIC      ) != 0; }
  bool is_final       () const         { return (_flags & JVM_ACC_FINAL       ) != 0; }
  bool is_synchronized() const         { return (_flags & JVM_ACC_SYNCHRONIZED) != 0; }
  bool is_super       () const         { return (_flags & JVM_ACC_SUPER       ) != 0; }
  bool is_volatile    () const         { return (_flags & JVM_ACC_VOLATILE    ) != 0; }
  bool is_transient   () const         { return (_flags & JVM_ACC_TRANSIENT   ) != 0; }
  bool is_native      () const         { return (_flags & JVM_ACC_NATIVE      ) != 0; }
  bool is_interface   () const         { return (_flags & JVM_ACC_INTERFACE   ) != 0; }
  bool is_abstract    () const         { return (_flags & JVM_ACC_ABSTRACT    ) != 0; }
  bool is_strict      () const         { return (_flags & JVM_ACC_STRICT      ) != 0; }
  bool is_stable      () const         { return (_flags & JVM_ACC_FIELD_STABLE) != 0; }

  // Conversion
  jint   as_int()                      { return _flags; }

  void print_klass_flags(outputStream* st = tty);
  void print_member_flags(outputStream* st = tty);
  void print(outputStream* st = tty);
};

class ciDSUFlags VALUE_OBJ_CLASS_SPEC {
private:
  jint _flags;

public:

  ciDSUFlags() { _flags = 0; }
  ciDSUFlags(DSUFlags flags) { _flags = flags.as_int(); }

  bool needs_stale_object_check () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK) != 0; }
  bool needs_mixed_object_check () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) != 0; }
  bool needs_type_narrow_check  () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_TYPE_NARROW_CHECK)  != 0; }
  bool is_restricted_method     () const    { return (_flags & DSU_FLAGS_MEMBER_IS_RESTRICTED_METHOD)     != 0; }

  bool is_stale_class                      ()  const { return (_flags & DSU_FLAGS_CLASS_IS_STALE_CLASS) !=0; }
  bool is_type_narrowed_class              ()  const { return (_flags & DSU_FLAGS_CLASS_IS_TYPE_NARROWED_CLASS) !=0; }
  bool is_super_type_of_stale_class        ()  const { return (_flags & DSU_FLAGS_CLASS_IS_SUPER_TYPE_OF_STALE_CLASS) !=0; }
  bool is_type_narrowing_relevant_type     ()  const { return (_flags & DSU_FLAGS_CLASS_IS_TYPE_NARROWING_RELEVANT_TYPE) !=0; }
  bool is_new_redefined_class              ()  const { return (_flags & DSU_FLAGS_CLASS_IS_NEW_REDEFINED_CLASS) !=0; }
  bool is_inplace_new_class                ()  const { return (_flags & DSU_FLAGS_CLASS_IS_INPLACE_NEW_CLASS) !=0; }

  void print(outputStream* st = tty);
};

#endif // SHARE_VM_CI_CIFLAGS_HPP
