/*
 * Copyright (C) 2012  Tianxiao Gu. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 * Please contact Instituite of Computer Software, Nanjing University,
 * 163 Xianlin Avenue, Nanjing, Jiangsu Provience, 210046, China,
 * or visit moon.nju.edu.cn if you need additional information or have any
 * questions.
*/

#ifndef SHARE_VM_RUNTIME_DSUFLAGS_HPP
#define SHARE_VM_RUNTIME_DSUFLAGS_HPP

#include "prims/jvm.h"
#include "utilities/top.hpp"


class DSUState {
public:
  enum {
    dsu_none = 0,
    dsu_will_be_added ,
    dsu_will_be_recompiled,
    dsu_will_be_swapped,
    dsu_will_be_redefined,
    dsu_will_be_deleted,
    dsu_has_been_added,
    dsu_has_been_recompiled,
    dsu_has_been_swapped,
    dsu_has_been_redefined,
    dsu_has_been_deleted,
  };
};

class TransfomationLevel {
public:
  enum {
    no_transformation      = 0,
    replace_klass          = 1,
    default_transformation = 2,
    custom_transformation  = 3,
  };

};

enum {
  DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK       = 0x00000001, // this member is subjected to stale object check.
  DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK       = 0x00000002, // this member is subjected to mixed object check.
  DSU_FLAGS_MEMBER_NEEDS_TYPE_NARROW_CHECK        = 0x00000004, // this member is subjected to type narrowed object check.
  DSU_FLAGS_MEMBER_IS_RESTRICTED_METHOD           = 0x00000008, // this member is an restricted method
  DSU_FLAGS_CLASS_IS_STALE_CLASS                  = 0x00000010, // this class is a stale class that has been redefined.
  DSU_FLAGS_CLASS_IS_TYPE_NARROWED_CLASS          = 0x00000020, // this class is a type narrowed class has a super type been removed after update
  DSU_FLAGS_CLASS_IS_SUPER_TYPE_OF_STALE_CLASS    = 0x00000040, // all valid new type of the stale class including its new version.
  DSU_FLAGS_CLASS_IS_TYPE_NARROWING_RELEVANT_TYPE = 0x00000080, // a super type has been removed from an undefined class.
  DSU_FLAGS_CLASS_IS_NEW_REDEFINED_CLASS          = 0x00000100, // this class is the new version that has been redefined
  DSU_FLAGS_CLASS_IS_INPLACE_NEW_CLASS            = 0x00000200, // this class is the instanceKlass of the inplace object.
  DSU_FLAGS_CLASS_IS_TRANSFORMER_CLASS            = 0x00000400  // this class is the instanceKlass of the inplace object.
};

// DSUFlags are used by DSU
class DSUFlags VALUE_OBJ_CLASS_SPEC {
private:
  jint _flags;

public:

  // Atomic update of flags
  void atomic_set_bits(jint bits);
  void atomic_clear_bits(jint bits);

  void set_flags(jint flags)   { _flags = flags; }
  jint get_flags() const       { return _flags;  }

  bool needs_stale_object_check () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK) != 0; }
  bool needs_mixed_object_check () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) != 0; }
  bool needs_type_narrow_check  () const    { return (_flags & DSU_FLAGS_MEMBER_NEEDS_TYPE_NARROW_CHECK)  != 0; }
  bool is_restricted_method     () const    { return (_flags & DSU_FLAGS_MEMBER_IS_RESTRICTED_METHOD)     != 0; }

  void set_needs_stale_object_check ()      { atomic_set_bits(DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK); }
  void set_needs_mixed_object_check ()      { atomic_set_bits(DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK); }
  void set_needs_type_narrow_check  ()      { atomic_set_bits(DSU_FLAGS_MEMBER_NEEDS_TYPE_NARROW_CHECK);  }
  void set_is_restricted_method     ()      { atomic_set_bits(DSU_FLAGS_MEMBER_IS_RESTRICTED_METHOD);     }

  void clear_needs_stale_object_check ()    { atomic_clear_bits(DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK); }
  void clear_needs_mixed_object_check ()    { atomic_clear_bits(DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK); }
  void clear_needs_type_narrow_check  ()    { atomic_clear_bits(DSU_FLAGS_MEMBER_NEEDS_TYPE_NARROW_CHECK);  }
  void clear_is_restricted_method     ()    { atomic_clear_bits(DSU_FLAGS_MEMBER_IS_RESTRICTED_METHOD);     }


  bool is_stale_class()                        const { return (_flags & DSU_FLAGS_CLASS_IS_STALE_CLASS) !=0;}
  bool is_type_narrowed_class()                const { return (_flags & DSU_FLAGS_CLASS_IS_TYPE_NARROWED_CLASS) !=0;}
  bool is_super_type_of_stale_class()          const { return (_flags & DSU_FLAGS_CLASS_IS_SUPER_TYPE_OF_STALE_CLASS) !=0;}
  bool is_type_narrowing_relevant_type()       const { return (_flags & DSU_FLAGS_CLASS_IS_TYPE_NARROWING_RELEVANT_TYPE) !=0;}
  bool is_new_redefined_class()                const { return (_flags & DSU_FLAGS_CLASS_IS_NEW_REDEFINED_CLASS) !=0;}
  bool is_inplace_new_class()                  const { return (_flags & DSU_FLAGS_CLASS_IS_INPLACE_NEW_CLASS) !=0;}
  bool is_transformer_class()                  const { return (_flags & DSU_FLAGS_CLASS_IS_TRANSFORMER_CLASS) !=0;}

  void set_is_stale_class()                          { atomic_set_bits(DSU_FLAGS_CLASS_IS_STALE_CLASS);}
  void set_is_type_narrowed_class()                  { atomic_set_bits(DSU_FLAGS_CLASS_IS_TYPE_NARROWED_CLASS);}
  void set_is_super_type_of_stale_class()            { atomic_set_bits(DSU_FLAGS_CLASS_IS_SUPER_TYPE_OF_STALE_CLASS);}
  void set_is_type_narrowing_relevant_type()         { atomic_set_bits(DSU_FLAGS_CLASS_IS_TYPE_NARROWING_RELEVANT_TYPE);}
  void set_is_new_redefined_class()                  { atomic_set_bits(DSU_FLAGS_CLASS_IS_NEW_REDEFINED_CLASS);}
  void set_is_inplace_new_class()                    { atomic_set_bits(DSU_FLAGS_CLASS_IS_INPLACE_NEW_CLASS);}
  void set_is_transformer_class()                    { atomic_set_bits(DSU_FLAGS_CLASS_IS_TRANSFORMER_CLASS);}

  void clear_is_stale_class()                        { atomic_clear_bits(DSU_FLAGS_CLASS_IS_STALE_CLASS);}
  void clear_is_type_narrowed_class()                { atomic_clear_bits(DSU_FLAGS_CLASS_IS_TYPE_NARROWED_CLASS);}
  void clear_is_super_type_of_stale_class()          { atomic_clear_bits(DSU_FLAGS_CLASS_IS_SUPER_TYPE_OF_STALE_CLASS);}
  void clear_is_type_narrowing_relevant_type()       { atomic_clear_bits(DSU_FLAGS_CLASS_IS_TYPE_NARROWING_RELEVANT_TYPE);}
  void clear_is_new_redefined_class()                { atomic_clear_bits(DSU_FLAGS_CLASS_IS_NEW_REDEFINED_CLASS);}
  void clear_is_inplace_new_class()                  { atomic_clear_bits(DSU_FLAGS_CLASS_IS_INPLACE_NEW_CLASS);}
  void clear_is_transformer_class()                  { atomic_clear_bits(DSU_FLAGS_CLASS_IS_TRANSFORMER_CLASS);}

  // Conversion
  jshort as_short()                    { return (jshort)_flags; }
  jint   as_int()                      { return _flags; }

 // Printing/debugging
#if INCLUDE_JVMTI
  void print_on(outputStream* st) const;
#else
  void print_on(outputStream* st) const PRODUCT_RETURN;
#endif

  inline friend  DSUFlags dsuFlags_from(jint flags);
};

inline DSUFlags dsuFlags_from(jint flags) {
  DSUFlags df;
  df._flags = flags;
  return df;
}

class DSUStreamProvider: public CHeapObj<mtInternal> {
public:
  DSUStreamProvider();
  ~DSUStreamProvider();
  virtual ClassFileStream* open_stream(const char * name, TRAPS) { return NULL; };
  virtual void print();
  virtual bool is_shared() { return false; }
};

#endif
