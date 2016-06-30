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
 * Please contact Institute of Computer Software, Nanjing University,
 * 163 Xianlin Avenue, Nanjing, Jiangsu Province, 210046, China,
 * or visit moon.nju.edu.cn if you need additional information or have any
 * questions.
*/

#ifndef SHARE_VM_RUNTIME_DSU_HPP
#define SHARE_VM_RUNTIME_DSU_HPP

#include "utilities/accessFlags.hpp"
#include "runtime/dsuFlags.hpp"
#include "utilities/growableArray.hpp"
#include "memory/iterator.hpp"
#include "runtime/thread.hpp"
//#include "classfile/classLoader.hpp"

class Dictionary;
class DSUMethod;
class DSUClass;
class DSUClassLoader;
class DSU;
class DSUStreamProvider;
class DSUPathEntryStreamProvider;
class DSUDynamicPatchBuilder;
class ClassPathEntry;

class DoNothinCodeBlobClosure : public CodeBlobClosure {
public:
  // Called for each code blob.
  virtual void do_code_blob(CodeBlob* cb) {}
};

class StackRepairClosure: public OopClosure {
  Thread * _thread;
public:
  void set_thread(Thread *thread) {
    _thread = thread;
  }
  virtual void do_oop(oop* p);
  virtual void do_oop(narrowOop* p) {
    //  ShouldNotReachHere();
  }
};

typedef enum {
  DSU_ERROR_NONE = 0,
  DSU_ERROR_INVALID_THREAD = 10,
  DSU_ERROR_INVALID_THREAD_GROUP = 11,
  DSU_ERROR_INVALID_PRIORITY = 12,
  DSU_ERROR_THREAD_NOT_SUSPENDED = 13,
  DSU_ERROR_THREAD_SUSPENDED = 14,
  DSU_ERROR_THREAD_NOT_ALIVE = 15,
  DSU_ERROR_INVALID_OBJECT = 20,
  DSU_ERROR_INVALID_CLASS = 21,
  DSU_ERROR_CLASS_NOT_PREPARED = 22,
  DSU_ERROR_INVALID_METHODID = 23,
  DSU_ERROR_INVALID_LOCATION = 24,
  DSU_ERROR_INVALID_FIELDID = 25,
  DSU_ERROR_NO_MORE_FRAMES = 31,
  DSU_ERROR_OPAQUE_FRAME = 32,
  DSU_ERROR_TYPE_MISMATCH = 34,
  DSU_ERROR_INVALID_SLOT = 35,
  DSU_ERROR_DUPLICATE = 40,
  DSU_ERROR_NOT_FOUND = 41,
  DSU_ERROR_INVALID_MONITOR = 50,
  DSU_ERROR_NOT_MONITOR_OWNER = 51,
  DSU_ERROR_INTERRUPT = 52,
  DSU_ERROR_INVALID_CLASS_FORMAT = 60,
  DSU_ERROR_CIRCULAR_CLASS_DEFINITION = 61,
  DSU_ERROR_FAILS_VERIFICATION = 62,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_METHOD_ADDED = 63,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_SCHEMA_CHANGED = 64,
  DSU_ERROR_INVALID_TYPESTATE = 65,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_HIERARCHY_CHANGED = 66,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_METHOD_DELETED = 67,
  DSU_ERROR_UNSUPPORTED_VERSION = 68,
  DSU_ERROR_NAMES_DONT_MATCH = 69,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_CLASS_MODIFIERS_CHANGED = 70,
  DSU_ERROR_UNSUPPORTED_REDEFINITION_METHOD_MODIFIERS_CHANGED = 71,
  DSU_ERROR_UNMODIFIABLE_CLASS = 79,
  DSU_ERROR_NOT_AVAILABLE = 98,
  DSU_ERROR_MUST_POSSESS_CAPABILITY = 99,
  DSU_ERROR_NULL_POINTER = 100,
  DSU_ERROR_ABSENT_INFORMATION = 101,
  DSU_ERROR_INVALID_EVENT_TYPE = 102,
  DSU_ERROR_ILLEGAL_ARGUMENT = 103,
  DSU_ERROR_NATIVE_METHOD = 104,
  DSU_ERROR_CLASS_LOADER_UNSUPPORTED = 106,
  DSU_ERROR_OUT_OF_MEMORY = 110,
  DSU_ERROR_ACCESS_DENIED = 111,
  DSU_ERROR_WRONG_PHASE = 112,
  DSU_ERROR_INTERNAL = 113,
  DSU_ERROR_UNATTACHED_THREAD = 115,
  DSU_ERROR_INVALID_ENVIRONMENT = 116,
  DSU_ERROR_BAD_REQUEST_TYPE = 120,
  DSU_ERROR_INVALID_MEMBER  = 121,
  DSU_ERROR_COMPUTE_SET_FIELDS  = 122,
  DSU_ERROR_SET_TRANSFORMER  = 123,
  DSU_ERROR_REWRITER  = 124,
  DSU_ERROR_LINK_OLD_CLASS  = 125,
  DSU_ERROR_LINK_NEW_CLASS  = 126,
  DSU_ERROR_TEMP_VERSION  = 127,
  DSU_ERROR_MIX_VERSION  = 128,
  DSU_ERROR_GET_UPDATING_TYPE  = 130,
  DSU_ERROR_PREPARE_SUPER_FAILED = 131,
  DSU_ERROR_PREPARE_INTERFACES_FAILED = 132,
  DSU_ERROR_RESOLVE_CLASS_LOADER_FAILED = 133,
  DSU_ERROR_RESOLVE_TRANSFORMER_FAILED =  134,
  DSU_ERROR_CREATE_MIX_VERSION_FAILED = 135,
  DSU_ERROR_CREATE_TEMP_VERSION_FAILED = 136,
  DSU_ERROR_RESOLVE_NEW_CLASS = 137,
  DSU_ERROR_RESOLVE_OLD_CLASS = 138,
  DSU_ERROR_OLD_CLASS_NOT_LOADED = 139,
  DSU_ERROR_PREPARE_DSU = 140,
  DSU_ERROR_PREPARE_DSUCLASSLOADER = 141,
  DSU_ERROR_PREPARE_DSUCLASS = 142,
  DSU_ERROR_COLLECT_CLASSES_TO_RELINK = 143,
  DSU_ERROR_UPDATE_DSU = 144,
  DSU_ERROR_UPDATE_DSUCLASSLOADER = 145,
  DSU_ERROR_UPDATE_DSUCLASS = 146,
  DSU_ERROR_RESOLVE_OLD_FIELD_ANNOTATION = 147,
  DSU_ERROR_NO_UPDATED_CLASS = 148,
  DSU_ERROR_TO_BE_ADDED = 149,
  DSU_ERROR_MAX = 150
} DSUError;

// TODO: not used
typedef enum{
  /*The default policy, see Jvolve*/
  DSU_SYSTEM_SAFE_POINT = 0,
  /*Passive System Safepoint, which suspend threads which have reached safepoint*/
  DSU_PASSIVE_SYSTEM_SAFE_POINT = 1,
  /*Concurrent Version for different threads*/
  DSU_THREAD_SAFE_POINT = 2,
} DSUPolicy;

typedef enum {
  DSU_REQUEST_INIT = 0,
  DSU_REQUEST_DISCARDED = 1,
  DSU_REQUEST_INTERRUPTED = 2,
  DSU_REQUEST_EMPTY = 3,
  DSU_REQUEST_FINISHED = 4,
  DSU_REQUEST_FAILED = 5,
  DSU_REQUEST_SYSTEM_MODIFIED = 6,
} DSURequestState;

typedef enum {
  DSU_CLASS_UNKNOWN = -1,  //
  DSU_CLASS_UNKNOWN_INDEX = 0,

  DSU_CLASS_NONE = 0,  // 0, no changes between the old and new version...
  DSU_CLASS_NONE_INDEX = DSU_CLASS_UNKNOWN_INDEX + 1,

  DSU_CLASS_MC  = 1,   // 1, relink new classes
  DSU_CLASS_MC_INDEX = DSU_CLASS_NONE_INDEX + 1,

  DSU_CLASS_BC  = DSU_CLASS_MC << 1,   // swap method body
  DSU_CLASS_BC_INDEX = DSU_CLASS_MC_INDEX + 1,

  DSU_CLASS_INDIRECT_BC = DSU_CLASS_BC << 1,   // no changes, but has to update vtable due to an swapped super class.
  DSU_CLASS_INDIRECT_BC_INDEX = DSU_CLASS_BC_INDEX + 1,

  DSU_CLASS_SMETHOD = DSU_CLASS_INDIRECT_BC << 1,   // only add or del static methods
  DSU_CLASS_SMETHOD_INDEX = DSU_CLASS_INDIRECT_BC_INDEX + 1,

  DSU_CLASS_SFIELD = DSU_CLASS_SMETHOD << 1,   // only add or del static fields
  DSU_CLASS_SFIELD_INDEX = DSU_CLASS_SMETHOD_INDEX + 1,

  DSU_CLASS_SBOTH = DSU_CLASS_SMETHOD + DSU_CLASS_SFIELD, // add or del both static methods and fields
  DSU_CLASS_SBOTH_INDEX = DSU_CLASS_SFIELD_INDEX + 1,

  DSU_CLASS_METHOD = DSU_CLASS_SFIELD << 1,   // add
  DSU_CLASS_METHOD_INDEX = DSU_CLASS_SBOTH_INDEX + 1,

  DSU_CLASS_FIELD = DSU_CLASS_METHOD << 1,    //
  DSU_CLASS_FIELD_INDEX = DSU_CLASS_METHOD_INDEX + 1,

  DSU_CLASS_BOTH = DSU_CLASS_FIELD + DSU_CLASS_METHOD,   //
  DSU_CLASS_BOTH_INDEX = DSU_CLASS_FIELD_INDEX + 1,

  DSU_CLASS_DEL = DSU_CLASS_FIELD << 1, //
  DSU_CLASS_DEL_INDEX = DSU_CLASS_BOTH_INDEX + 1,

  DSU_CLASS_ADD = DSU_CLASS_DEL << 1,   //
  DSU_CLASS_ADD_INDEX = DSU_CLASS_DEL_INDEX + 1,

  DSU_CLASS_STUB = DSU_CLASS_ADD << 1,   //
  DSU_CLASS_STUB_INDEX = DSU_CLASS_ADD_INDEX + 1,

  DSU_CLASS_COUNT = DSU_CLASS_STUB_INDEX + 1,
} DSUClassUpdatingType;

typedef enum {
  DSU_METHOD_NONE = 0,
  DSU_METHOD_MC = 1,
  DSU_METHOD_BC = 2,
  DSU_METHOD_DEL = 3,
  DSU_METHOD_ADD = 4,
} DSUMethodUpdatingType;



// A DSUObject is used for following updating.
// Each DSUObject is parsed from the dynamic patch
// or just parsed by comparing jvmtiClassDefinitions
class DSUObject : public CHeapObj<mtInternal> {
  friend class Javelus;
public:
  DSUObject();
  ~DSUObject();

protected:
  bool _validated;
  void set_validated(bool v) { _validated = v; }
public:
  bool validated() const { return _validated; }
  virtual bool validate(TRAPS) = 0;
  virtual void print();
};



// A DSUMethod contains updating information about a method.
// The method must be declared in the old program.
class DSUMethod : public DSUObject {
private:
  DSUMethodUpdatingType _updating_type;
  Method* _method;
  int _matched_new_method_index;
  DSUClass * _dsu_class;
  DSUMethod * _next;
public:
  DSUMethod();
  ~DSUMethod();

  virtual bool validate(TRAPS);

  void set_dsu_class(DSUClass * klass) {
    _dsu_class = klass;
  }

  void set_next(DSUMethod* next) {
    _next = next;
  }

  DSUMethod* next() const {
    return _next;
  }

  DSUClass * dsu_class() const {
    return _dsu_class;
  }

  DSUMethodUpdatingType updating_type() const { return _updating_type; }
  Method* method() const;
  int matched_new_method_index() const { return _matched_new_method_index; }

  void set_updating_type(DSUMethodUpdatingType updating_type) { _updating_type = updating_type; }
  void set_method (Method* method) { _method = method; }
  void set_matched_new_method_index(int index) { _matched_new_method_index = index; }

  virtual void print();
};

// A DSUClass contains updating information about a class.
// The class may be
class DSUClass : public DSUObject {
public:
  enum MatchFieldPair {
    matched_field_old_offset  = 0, // offset is a u4
    matched_field_new_offset  = 4,
    matched_field_flags       = 8, // the first bit indicates whether the old field is a mixed new field and the second bit is for new field
    matched_field_type        = 9, // BasicType
    next_matched_field        = 10
  };

  enum InplaceField {
    inplace_field_offset      = 0,
    inplace_field_length      = 4, // may be continual fields, so use u4
    next_inplace_field        = 8
  };



  enum TransformerArgs {
    transformer_field_offset  = 0,
    transformer_field_flag    = 4,
    transformer_field_type    = 5,
    next_transformer_arg      = 6
  };

private:
  DSUClass*            _next;
  DSUClassLoader*      _dsu_class_loader;
  DSUClassUpdatingType _updating_type;

  //indicate all data required by updating has been prepared.
  // 1). detailed updating type
  // 2). restricted method
  // 3).
  bool _prepared;

  // Stream Provider
  DSUStreamProvider*   _stream_provider;

  // Symbol* not java.lang.String
  Symbol* _name;

  // We have three property to describe an instanceKlass involved in DSU.
  // 1). Whether instances of the instanceKlass is subject to stale object check
  // 2). Whether the instanceKlass is for the inplace object, or the regular and phantom object
  // 3). Whether the instanceKlass is contains old or new code.
  // In fact, there would be 2*2*2=8 compositions.
  // Here, we only care about 4 of them, others are easy to fetch from these four.
  // 1). stale_inplace_old_class: indicate the object is stale and should be updated
  // 2). new_regular_new_class: indicate the object is a regular or a phantom object.
  // 3). new_inplace_new_class: indicate the object is an inplace object.
  // 4). stale_new_class: indicate the object is a stale object being updating,
  //       especially during invoking transformers.
  //       stale_new_class may be either a stale_inplace_new_class or a stale_regular_new_class
  //
  // alias of stale_inplace_old_class
  InstanceKlass* _old_version;
  // alias of new_regular_new_class
  InstanceKlass* _new_version;
  InstanceKlass* _new_inplace_new_class;
  InstanceKlass* _stale_new_class;


  // transformers
  Method*    _object_transformer_method;
  Array<u1>* _object_transformer_args;
  Method*    _class_transformer_method;
  Array<u1>* _class_transformer_args;

  // list of DSUMethod
  DSUMethod* _first_method;
  DSUMethod* _last_method;


  // Cached the value
  int     _min_object_size_in_bytes;
  int     _min_vtable_size;
  InstanceKlass* _old_ycsc;
  InstanceKlass* _new_ycsc;

  jint    _match_static_count;
  u1*     _match_static_fields;

  GrowableArray<Symbol*>* _match_reflections;
  GrowableArray<Symbol*>* _resolved_reflections_name_and_sig;
  GrowableArray<jobject>* _resolved_reflections;

  //    A           A 
  //   / \         / \
  //  B   C   ==> B   C
  //  |               |
  //  D               D'
  // B is a type narrowing relevant class and also a super class of a stale class
  // C is a super class of a stale class.
  // Both B and C are subject to stale object check
  GrowableArray<InstanceKlass*>* _type_narrowing_relevant_classes;
  GrowableArray<InstanceKlass*>* _super_classes_of_stale_class;
public:
  DSUClass();
  ~DSUClass();

//  void initialize();

  virtual bool validate(TRAPS);

  void set_name(Symbol* name) {
    _name = name;
  }

  DSUClassLoader* dsu_class_loader() const {
    assert(_dsu_class_loader != NULL, "sanity check");
    return _dsu_class_loader;
  }

  void append_match_reflection(Symbol* old_name,
    Symbol* old_sig,
    Symbol* new_name,
    Symbol* new_sig,
    Symbol* new_class_name);

  void append_resolved_reflection(
    Symbol* name,
    Symbol* sig,
    jobject old_reflection);

  Method* find_new_matched_method(Symbol* name, Symbol* signature);
  oop find_resolved_reflection(Symbol* old_name, Symbol* old_signature);

  bool has_resolved_reflection() const {
    return _resolved_reflections != NULL && _resolved_reflections->length() > 0;
  }

  // set flags for fields;
  void compute_and_set_fields(InstanceKlass* old_version,
    InstanceKlass* new_version, TRAPS);
  void compute_and_cache_common_info(InstanceKlass* old_version,
    InstanceKlass* new_version, TRAPS);

  static void compute_youngest_common_super_class(InstanceKlass* old_version,
    InstanceKlass* new_version,
    InstanceKlass* &old_ycsc,
    InstanceKlass* &new_ycsc, TRAPS);

  static void collect_super_classes_and_interfaces(InstanceKlass* klass, GrowableArray<InstanceKlass*>* supertypes);


  void check_and_set_flags_for_type_narrowing_check(InstanceKlass* old_version, InstanceKlass* new_version, TRAPS);
  // set flags for methods;
  static void set_check_flags_for_methods(InstanceKlass* new_version, int min_vtable_length, TRAPS);
  static void set_check_flags_for_fields(InstanceKlass* new_version, int min_object_size, TRAPS);
  void set_restricted_methods();

  // changed classes and deleted classes have old versions
  DSUError resolve_old_version(InstanceKlass* &old_version, TRAPS);
  DSUError resolve_new_version(InstanceKlass* &new_version, TRAPS);
  DSUError resolve_new_version_at_safe_point(InstanceKlass* &new_version, TRAPS);
  DSUError resolve_new_version_by_dsu_thread(InstanceKlass* &new_version, TRAPS);
  void update_class_loader(DSUClassLoader* new_loader);

  DSUError refresh_updating_type(InstanceKlass* old_version,
    InstanceKlass* new_version, TRAPS);

  bool require_old_version()  const;
  bool old_version_resolved() const;
  bool require_new_version()  const;
  bool new_version_resolved() const;

  // resolve the transformer for the class
  // dsu_class_loader should have bound a transformer class.
  DSUError resolve_transformer(TRAPS);

  bool require_new_inplace_new_class(TRAPS)   const;
  bool require_stale_new_class(TRAPS) const;
  DSUError build_new_inplace_new_class_if_necessary(TRAPS);
  DSUError build_stale_new_class_if_necessary(TRAPS);

  DSUError prepare(TRAPS);
  bool prepared() const { return _prepared; }

  void update(TRAPS);

  //For method body changed class
  void swap_class(TRAPS);
  //For deleted old classes
  void undefine_class(TRAPS);
  //For indirect update class
  void relink_class(TRAPS);
  //For class update class
  void redefine_class(TRAPS);
  //For added class
  void define_class(TRAPS);


  static InstanceKlass* allocate_stale_new_class_for_deleted_class(InstanceKlass* old_version, TRAPS);

  void apply_default_class_transformer(InstanceKlass* old_version,
    InstanceKlass* new_version, TRAPS);


  void post_fix_old_version(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  void post_fix_new_version(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  void post_fix_new_inplace_new_class(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  void post_fix_stale_new_class(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  void post_fix_vtable_and_itable(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  void install_new_version(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS);

  static void check_and_update_swapped_super_classes(InstanceKlass* this_class);

  void add_type_narrowing_relevant_class(InstanceKlass* klass);
  void add_super_class_of_stale_class(InstanceKlass* klass);
  void collect_affected_types(InstanceKlass* old_version, InstanceKlass* new_version, TRAPS);
  // copy from jvmtiRedefineClasses
  void update_jmethod_ids(InstanceKlass* new_version, TRAPS);
  void mark_old_and_obsolete_methods(TRAPS);

  // after swap a class, we need to update vtable and itable of all its sub-classes.
  static void update_subklass_vtable_and_itable(InstanceKlass* old_version, TRAPS);

  // compare and determine the updating type of these classes.
  DSUError compare_and_normalize_class(InstanceKlass* old_version,
    InstanceKlass* new_version, TRAPS);

  Symbol* name()   const { return _name; }

  Method* object_transformer_method() const {
    return _object_transformer_method;
  }

  Array<u1>* object_transformer_args() const {
    return _object_transformer_args;
  }

  void set_object_transformer_method(Method* object_transformer_method) {
    this->_object_transformer_method = object_transformer_method;
  }

  void set_object_transformer_args(Array<u1>* object_transformer_args) {
    this->_object_transformer_args = object_transformer_args;
  }

  Method* class_transformer_method() const {
    return _class_transformer_method;
  }

  Array<u1>* class_transformer_args() const {
    return _class_transformer_args;
  }

  void set_class_transformer_method(Method* class_transformer_method) {
    this->_class_transformer_method = class_transformer_method;
  }

  void set_class_transformer_args(Array<u1>* class_transformer_args) {
    this->_class_transformer_args = class_transformer_args;
  }

  // cache klassOop here
  void set_old_version_raw(InstanceKlass* klass) { _old_version = klass; }
  InstanceKlass* old_version_raw() const { return _old_version; }

  DSUClassUpdatingType updating_type() const { return _updating_type; }

  InstanceKlass* old_version_class()         const { return _old_version;             }
  InstanceKlass* new_version_class()         const { return _new_version;             }
  InstanceKlass* new_inplace_new_class()     const { return _new_inplace_new_class;   }
  InstanceKlass* stale_new_class()           const { return _stale_new_class; }

  DSUClass* next()        const { return _next; }
  void set_next(DSUClass* next) { _next = next; }

  DSUMethod* first_method() const { return _first_method; }
  DSUMethod* last_method()  const { return _last_method;  }

  DSUMethod* find_method(Symbol* name, Symbol* desc);

  void methods_do(void f(DSUMethod* dsu_class, TRAPS), TRAPS);

  void add_method(DSUMethod* methods);

  void set_updating_type(DSUClassUpdatingType updating_type) { _updating_type = updating_type; }
  void set_old_version(InstanceKlass* klass) { _old_version = klass; }
  void set_new_version(InstanceKlass* klass) { _new_version = klass; }
  void set_new_inplace_new_class (InstanceKlass* klass) { _new_inplace_new_class = klass; }
  void set_stale_new_class       (InstanceKlass* klass) { _stale_new_class = klass; }

  u1*  match_static_fields() const { return _match_static_fields; }
  void set_match_static_fields(u1* match_static_fields) { _match_static_fields = match_static_fields; }

  jint match_static_count() const   { return _match_static_count; }
  void set_match_static_count(jint match_static_count) { _match_static_count = match_static_count; }

  int from_rn() const;
  int to_rn() const;

  void set_dsu_class_loader(DSUClassLoader* dsu_class_loader) {
    _dsu_class_loader = dsu_class_loader;
  }

  int min_object_size_in_bytes() const {
    return _min_object_size_in_bytes;
  }

  void set_min_object_size_in_bytes(int size) {
    _min_object_size_in_bytes = size;
  }

  int min_vtable_size() const {
    return _min_vtable_size;
  }

  void set_min_vtable_size(int size) {
    _min_vtable_size = size;
  }

  void set_stream_provider(DSUStreamProvider *stream_provider) { _stream_provider = stream_provider; }
  DSUStreamProvider* stream_provider() const { return _stream_provider; }

  // youngest common super class could be decided by compares class hierarchies of the new class and old class
  // if we use equal(A equal A' iff A == A') to map common relation, then the yscs() return the real klassOop
  // if we use equvialent (A equvialent A' iff A.real_name == A'.real_name), then the ycsc() return the real_name Symbol*
  InstanceKlass* old_ycsc() const {
    return _old_ycsc;
  }

  InstanceKlass* new_ycsc() const {
    return _new_ycsc;
  }

  void set_old_ycsc(InstanceKlass* ycsc) {
    _old_ycsc = ycsc;
  }

  void set_new_ycsc(InstanceKlass* ycsc) {
    _new_ycsc = ycsc;
  }

  DSUMethod* allocate_method(Method* method, TRAPS);


  virtual void print();

};


class DSUClassLoader : public DSUObject {
private:

  DSU*  _dsu;

  // sibling DSUClassLoader in the belonging DSU
  DSUClassLoader * _next;

  // id of this DSUClassLoader
  // Symbol*
  Symbol* _id;
  // id of the loaded classloader of this DSUClassLoader
  // Symbol*
  Symbol* _lid;
  DSUClassLoader * _loaded_loader;

  // works like class path
  DSUStreamProvider * _stream_provider;

  // the class laoder data
  ClassLoaderData* _loader_data;
  // the instanceKlass
  InstanceKlass* _helper_class;
  InstanceKlass* _transformer_class;


  // class file
  DSUClass* _first_class;
  DSUClass* _last_class;

public:

  InstanceKlass*    helper_class() const { return _helper_class; }
  InstanceKlass*    transformer_class() const { return _transformer_class; }
  oop               classloader() const { return (oop)_loader_data->class_loader(); }
  ClassLoaderData*  class_loader_data() const { return _loader_data; }


  void resolve(TRAPS);
  bool resolved();

  DSUClassLoader * loaded_loader() const{
    return _loaded_loader;
  }

  virtual bool validate(TRAPS);

  void set_loaded_loader(DSUClassLoader * loaded_loader) {
    _loaded_loader = loaded_loader;
  }

  DSUClass *first_class() const { return _first_class; }
  DSUClass *last_class() const { return _last_class; }
  void add_class(DSUClass* klass);
  void remove_class(DSUClass* klass);

  bool resolve_transformer(Symbol* transformer_name, TRAPS);

  void set_helper_class(InstanceKlass* helper_class) { _helper_class = helper_class; }
  void set_transformer_class(InstanceKlass* transformer_class) { _transformer_class = transformer_class; }
  void set_class_loader_data(ClassLoaderData* loader_data) { _loader_data = loader_data; }

  DSUClassLoader();
  ~DSUClassLoader();

  void set_next(DSUClassLoader * next) {
    _next = next;
  }

  DSUClassLoader * next() const {
    return _next;
  }

  void set_dsu(DSU * dsu) {
    _dsu = dsu;
  }

  DSU* dsu() const {
    return _dsu;
  }

  void set_id(Symbol* id) {
    this->_id = id;
  }

  Symbol* id() {
    return this->_id;
  }

  void set_lid(Symbol* lid) {
    this->_lid = lid;
  }

  Symbol* lid() {
    return this->_lid;
  }

  // prepare this DSU operation
  DSUError prepare(TRAPS);

  void set_stream_provider(DSUStreamProvider * stream_provider) { _stream_provider = stream_provider; }
  DSUStreamProvider * stream_provider() const { return _stream_provider; }

  // just create the klassOop and does not put it into any dictionary.
  DSUError  load_new_version(Symbol* name, InstanceKlass* &new_class, DSUStreamProvider * stream_provider, TRAPS);
  // allocate a new class if it does not exist.
  DSUClass* allocate_class(Symbol* name, TRAPS);

  DSUClass* find_class_by_name(Symbol* name);

  virtual void print();
};

class DSU : public DSUObject {
private:

  int  _system_dictionary_modification_number_at_prepare;
  int  _weak_reflection_number_at_prepare;

  int  _from_rn;
  int  _to_rn;

  // A linked list of class loader
  DSUClassLoader* _first_class_loader;
  DSUClassLoader* _last_class_loader;

  DSURequestState _request_state;
  const char * _dynamic_patch;

  DSU* _next;

  DSUStreamProvider* _shared_stream_provider;

  GrowableArray<DSUClass*>* _classes_in_order;
public:
  DSU();
  ~DSU();

  // Currently, we use a shared stream provider model
  void set_shared_stream_provider(DSUStreamProvider* stream_provider) { _shared_stream_provider = stream_provider; }
  DSUStreamProvider* shared_stream_provider() const { return _shared_stream_provider; }

  virtual bool validate(TRAPS);

  bool system_modified() const;
  bool reflection_modified() const;

  // prepare this DSU operation
  DSUError prepare(TRAPS);

  // update this DSU
  DSUError update(TRAPS);

  void update_ordered_classes(TRAPS);

  DSURequestState request_state() const {
    return _request_state;
  }

  bool is_finished()    const { return _request_state == DSU_REQUEST_FINISHED; }
  bool is_empty()       const { return _request_state == DSU_REQUEST_EMPTY; }
  bool is_discarded()   const { return _request_state == DSU_REQUEST_DISCARDED; }
  bool is_interrupted() const { return _request_state == DSU_REQUEST_INTERRUPTED; }
  bool is_init()        const { return _request_state == DSU_REQUEST_INIT; }
  bool is_system_modified() const { return _request_state == DSU_REQUEST_SYSTEM_MODIFIED; }

  // eager update: all objects are updated eagerly.
  // But no pointers are updated eagerly, i.e., we sill have to use MixObjects.
  // Pointers are updated during copying or moving GC, which will merge MixObjects.
  bool is_eager_update()         const { return ForceEagerUpdate; }
  // eager update pointer: all objects are updated eagerly.
  // All MixObjects are merged
  bool is_eager_update_pointer() const { return ForceEagerUpdate; }
  // lazy update: all objects are updated lazily.
  // Pointers are updated during copying or moving GC, which will merge MixObjects.
  bool is_lazy_update()          const { return !(is_eager_update() && is_eager_update_pointer()); }

  void set_request_state(DSURequestState request_state) {
    _request_state = request_state;
  }

  void classes_do(void f(DSUClass * dsu_class,TRAPS), TRAPS);

  void set_up_new_classpath(TRAPS);
private:
  static GrowableArray<DSUClass>*  _classes_to_relink;
public:

  static void collect_changed_reflections(TRAPS);
  static void check_and_append_changed_reflection(oop* reflection);
  static void check_and_append_changed_reflect_field(oop* field);
  static void check_and_append_changed_reflect_method(oop* method);
  static void check_and_append_changed_reflect_constructor(oop* constructor);
  static bool is_changed_reflect_field(oop field);
  static bool is_changed_reflect_method(oop method);
  static bool is_changed_reflect_constructor(oop constructor);
  void update_changed_reflection(TRAPS);

  static void update_changed_reflect_field(oop old_reflection);
  static void update_changed_reflect_method(oop old_reflection);
  static void update_changed_reflect_constructor(oop old_reflection);

  static void update_changed_reflect_field(oop old_reflection, oop new_reflection);
  static void update_changed_reflect_method(oop old_reflection, oop new_reflection);
  static void update_changed_reflect_constructor(oop old_reflection, oop new_reflection);

  // check classes that requires redefining
  static void check_and_append_relink_class(Klass* k_oop, TRAPS);

  static void collect_classes_to_relink(TRAPS);
  static void relink_collected_classes(TRAPS);

  static void append_classes_to_relink(InstanceKlass* ik);
  static void free_classes_to_relink();

  static void flush_dependent_code(TRAPS);

  void set_from_rn(int from_rn) {
    _from_rn = from_rn;
  }
  void set_to_rn(int to_rn) {
    _to_rn = to_rn;
  }
  int from_rn() const { return _from_rn; }
  int to_rn()   const { return _to_rn; }

  DSU* next()        const { return _next; }
  void set_next(DSU* next) { _next = next; }

  // get added first class loader
  DSUClassLoader* first_class_loader() const { return _first_class_loader; }
  DSUClassLoader* last_class_loader()  const { return _last_class_loader; }

  // allocate a class loader with given id and lid
  DSUClassLoader* allocate_class_loader(Symbol* id, Symbol* lid, TRAPS);
  // append class loader to the list, no checking
  void add_class_loader(DSUClassLoader* class_loader);
  void add_class(DSUClass* dsu_class);

  // query DSUClass
  DSUClass* find_class_by_name(Symbol* name);
  DSUClass* find_class_by_name_and_loader(Symbol* name, Handle loader);

  // id is a global Symbol* NOT is a java.lang.String object
  DSUClassLoader* find_class_loader_by_id(Symbol* id);
  DSUClassLoader* find_class_loader_by_loader(Handle loader);
  DSUClassLoader* find_or_create_class_loader_by_loader(Handle loader, TRAPS);

  virtual void print();
};

// --------------------- DSUStreamProvider -------------------

class DSUPathEntryStreamProvider: public DSUStreamProvider {
private:
  ClassPathEntry * _first_entry;
  ClassPathEntry * _last_entry;
public:
  DSUPathEntryStreamProvider();
  ~DSUPathEntryStreamProvider();
  void append_path(char* path, TRAPS);
  void add_entry(ClassPathEntry* entry);
  virtual ClassFileStream* open_stream(const char * name, TRAPS);
  virtual void print();
  virtual bool is_shared() { return true; }
};


// --------------------- DSUDynamicPatchBuilder -------------------

class DSUBuilder : public ResourceObj {
protected:
  DSU *_dsu;
  bool _succ;
public:
  DSUBuilder();
  virtual ~DSUBuilder();
  DSU* dsu() const;
  bool build(TRAPS);
  virtual bool build_all(TRAPS) = 0;
};

class DSUDynamicPatchBuilder : public DSUBuilder {
public:
  enum DynamicPatchCommand{
    UnknownCommand = -1,
    DynamicPatchFirstCommand = 0,
    ClassLoaderCommand = DynamicPatchFirstCommand,
    ClassPathCommand,
    AddClassCommand,
    ModClassCommand,
    DelClassCommand,
    TransformerCommand,
    LoaderHelperCommand,
    ReflectionCommand,
    DynamicPatchCommandCount
  };
  static const char * command_names[];
private:
  DSUClass* _current_class;
  DSUClassLoader* _current_class_loader;
  DSUClassLoader* _default_class_loader;
  DSUPathEntryStreamProvider* _shared_stream_provider;
  const char* _dynamic_patch;
  void use_class_loader(char *line, TRAPS);
  void build_default_class_loader(TRAPS);
  void use_default_class_loader();
  void append_class_path_entry(char* line, TRAPS);
  void append_added_class(char* line, TRAPS);
  void append_modified_class(char* line, TRAPS);
  void append_deleted_class(char* line, TRAPS);
  void append_transformer(char* line, TRAPS);
  void append_loader_helper(char* line, TRAPS);
  void append_reflection(char* line, TRAPS);
  DSUClassLoader* current_class_loader();
  DSUClass* current_class();
  static DynamicPatchCommand parse_command_name(const char* line, int* bytes_read);
  void parse_file(const char* file, TRAPS);
  void parse_line(char* line, TRAPS);
  void populate_sub_classes(TRAPS);
  void populate_sub_classes(DSUClass* dsu_class, InstanceKlass* ik, TRAPS);
public:
  DSUDynamicPatchBuilder(const char* dynamic_patch);
  virtual ~DSUDynamicPatchBuilder();
  const char * dynamic_patch() const { return _dynamic_patch; }
  DSUStreamProvider *stream_provider() { return _shared_stream_provider; }
  bool build_all(TRAPS);
};


// --------------------- DSUJvmtiBuilder ----------

class DSUDirectStreamProvider : public DSUStreamProvider {
private:
  jint _class_byte_count;
  const unsigned char* _class_bytes;
public:
  DSUDirectStreamProvider(jint class_byte_count, const unsigned char* class_bytes);
  ~DSUDirectStreamProvider();
  virtual ClassFileStream *open_stream(const char *class_name, TRAPS);
  virtual void print();
};

class DSUJvmtiBuilder : public DSUBuilder{
private:
  jint  _class_count;
  const jvmtiClassDefinition *_class_defs;
  void build_added_class(DSUClassUpdatingType type, const jvmtiClassDefinition *def, TRAPS);
  void build_deleted_class(DSUClassUpdatingType type, const jvmtiClassDefinition *def, TRAPS);
  void build_modified_class(DSUClassUpdatingType type, const jvmtiClassDefinition *def, TRAPS);
  void build_transformers(TRAPS);
public:
  DSUJvmtiBuilder(jint class_count, const jvmtiClassDefinition *class_defs);
  virtual ~DSUJvmtiBuilder();
  bool build_all(TRAPS);
};


// --------------------- Javelus --------------------
// Note: put methods that would be used either
// by class DSU and any other class here.
// Keep methods only used by DSU in the class DSU.

class Javelus: AllStatic {
  friend class VM_DSUOperation;
private:
  // Hashtable holding dsu loaded classes.

  // the head of dsu list
  // only can be changed at VM safe point
  static DSU*                   _first_dsu;
  static DSU*                   _last_dsu;
  // The current working DSU
  static DSU* volatile          _active_dsu;
  // the largest revision number
  static int                    _latest_rn;

  static Dictionary*            _dsu_dictionary;
  //static PlaceholderTable*      _dsu_placeholder;
  static DSUThread*             _dsu_thread;

  static Method*                _implicit_update_method;

  static InstanceKlass*         _developer_interface_klass;

  static void create_DevelopInterface_klass(TRAPS);

  static void set_active_dsu(DSU* dsu);
  static void increment_system_rn();
  static void install_dsu(DSU *dsu);
public:
  const static int MIN_REVISION_NUMBER;
  const static int MAX_REVISION_NUMBER;

public:

  static DSU* active_dsu();
  static void finish_active_dsu();
  static void discard_active_dsu();


  static DSUClassUpdatingType join(DSUClassUpdatingType this_type, DSUClassUpdatingType that_type);
  static const char * class_updating_type_name(DSUClassUpdatingType ct);

  static InstanceKlass* developer_interface_klass();


  static Dictionary* dictionary() { return _dsu_dictionary; }
  static InstanceKlass* resolve_dsu_klass(Symbol* class_name, ClassLoaderData* loader_data, Handle protection_domain, TRAPS);
  static InstanceKlass* resolve_dsu_klass_or_null(Symbol* class_name, ClassLoaderData* loader_data, Handle protection_domain, TRAPS);

  static DSUClassUpdatingType get_updating_type(InstanceKlass* ikh, TRAPS);

  static void prepare_super_class(InstanceKlass* ikh, TRAPS);
  static void prepare_interfaces(InstanceKlass* ikh, TRAPS);

  // XXX only can be done with Stop-The-World gc
  static void realloc_decreased_object(Handle obj, int old_size_in_bytes, int new_size_in_bytes);

  static void copy_fields(oop src, oop dst, InstanceKlass* ik);
  static void copy_fields(oop src, oop dst, oop mix_dst, InstanceKlass* ik);
  static void copy_field(oop src, oop dst, int src_offset, int dst_offset, BasicType type);

  static void read_value(Handle obj, int offset, BasicType type, JavaValue &result,TRAPS);
  static void read_arg(Handle obj, int offset, BasicType type, JavaCallArguments &args,TRAPS);

  static void pick_loader_by_class_name(Symbol* name, ClassLoaderData* &loader, TRAPS);

  static Klass* find_klass_in_system_dictionary(Symbol* name, ClassLoaderData* data);
  static void add_dsu_klass(InstanceKlass* k, TRAPS);
  static void add_dsu_klass_place_holder(Handle loader, Symbol* class_name, TRAPS);

  static DSUThread* get_dsu_thread() { return _dsu_thread; }

  static DSUThread* make_dsu_thread(const char * name);

  static void repatch_method(Method* method,bool print_replace, TRAPS);


  static void initialize(TRAPS);
  static void dsu_thread_init();

  static oopDesc* merge_mixed_object(oopDesc* old_obj);
  static oopDesc* merge_mixed_object(oopDesc* old_obj, oopDesc* new_obj);
  static void dsu_thread_loop();


  static void parse_old_field_annotation(InstanceKlass* the_class,
    InstanceKlass* transformer, Array<u1>* annotation, Array<u1>* &result, TRAPS);

  static void oops_do(OopClosure* f);

  static void transform_object(Handle h, TRAPS);
  //the common stuff
  static bool transform_object_common(Handle recv, TRAPS);
  static bool transform_object_common_no_lock(Handle recv, TRAPS);
  // link, relink, unlink a mixed object.
  static void link_mixed_object(Handle inplace_object, Handle phantom_object, TRAPS);
  static void relink_mixed_object(Handle inplace_object, Handle old_phantom_object, Handle new_phantom_object, TRAPS);
  static void unlink_mixed_object(Handle inplace_object, Handle old_phantim_object, TRAPS);
  static void link_mixed_object_slow(Handle inplace_object, Handle phantom_object, TRAPS);
  // run transformer objects 
  static void run_transformer(Handle inplace_object, Handle old_phantom_object, Handle new_phantom_object,
                 InstanceKlass* old_phantom_klass, InstanceKlass* new_inplace_klass, InstanceKlass* new_phantom_klass, TRAPS);

  static int  system_revision_number();
  static bool validate_DSU(DSU *dsu);

  static DSU* get_DSU(int from_rn);

  static void wakeup_DSU_thread();
  static void waitRequest();
  static void waitRequest(long time);
  static void notifyRequest();


  static bool is_modifiable_class(oop klass_mirror);

  //XXX remember that not alive does not mean is dead.
  // It may have not born yet.
  static bool is_class_not_born_at(InstanceKlass* the_class, int rn);
  static bool is_class_born_at(InstanceKlass* the_class, int rn) ;
  static bool is_class_alive(InstanceKlass* the_class, int rn) ;
  static bool is_class_dead_at(InstanceKlass* the_class, int rn) ;
  static bool is_class_dead(InstanceKlass* the_class, int rn) ;

  static bool check_application_threads();
  static void repair_application_threads();

  static void install_return_barrier_all_threads();
  static void install_return_barrier_single_thread(JavaThread *thread, intptr_t * barrier);

  static bool check_single_thread(JavaThread * thread);
  static void repair_single_thread(JavaThread * thread);

  static void invoke_return_barrier(JavaThread* thread);

  static void check_class_initialized(JavaThread *thread, InstanceKlass* klass);

  static void invoke_dsu_in_jvmti(jint class_count, const jvmtiClassDefinition* class_definitions);

};


class DSUEagerUpdate : public AllStatic {

private:
  // an array of weak global handle
  static jobject*  _candidates;
  static int       _candidates_length;
  static volatile bool _be_updating;
  static bool      _update_pointers;
  static volatile bool _has_been_updated;
public:

  static void initialize(TRAPS);

  static void configure_eager_update(GrowableArray<jobject> * candidates, bool update_pointers);

  static void install_eager_update_return_barrier(TRAPS);

  static void start_eager_update(JavaThread *thread);
  static void collect_dead_instances_at_safepoint(int dead_time, GrowableArray<jobject>* result, TRAPS);
};

/////////////////////////////////////////////////////
// Code copied from jvmtiRedefineClassesTrace.hpp
////////////////////////////////////////////////////


// DSU Trace Definition
// 
// 0x00000000 |           0 - default, no tracing message
// 0x00000001 |           1 - print timer information of VMDSUOperation
// 0x00000002 |           2 - general debug
// 0x00000004 |           4 - general trace
// 0x00000008 |           8 - parse DSU
// 0x00000010 |          16 - load DSU and DSU class loaders
// 0x00000020 |          32 - load DSU classes
// 0x00000040 |          64 - load DSU methods
// 0x00000080 |         128 - trace check and repair thread
// 0x00000100 |         256 - trace eager update
// 0x00000200 |         512 - redefine class
// 0x00000400 |        1024 - swap class
// 0x00000800 |        2048 - recompile class
// 0x00001000 |        4096 - trace transformer in sharedRuntime
// 0x00002000 |        8192 - trace resolve invalid member
// 0x         |       16383 - all

#define DSU_TRACE(level, args) \
  if ((TraceDSU & level) != 0) { \
    ResourceMark rm; \
    tty->print("[DSU]-0x%x: ", level); \
    tty->print_cr args; \
  } while (0)

#define DSU_INFO(args) \
  if ((TraceDSU & 0x00000001) != 0) { \
    ResourceMark rm; \
    tty->print("[DSU]-[Info]: "); \
    tty->print_cr args; \
  } while (0)

#define DSU_DEBUG(args) \
  if ((TraceDSU & 0x00000002) != 0) { \
    ResourceMark rm; \
    tty->print("[DSU]-[Debug]: "); \
    tty->print_cr args; \
  } while (0)

#define DSU_WARN(args) \
  { \
    ResourceMark rm; \
    tty->print("[DSU]-[Warn]: "); \
    tty->print_cr args; \
  } while (0)

#define DSU_TRACE_WITH_THREAD(level, thread, args) \
  if ((TraceDSU & level) != 0) { \
    ResourceMark rm(thread); \
    tty->print("[DSU]-0x%x: ", level); \
    tty->print_cr args; \
  } while (0)

#define DSU_TRACE_MESG(args) \
  if ((TraceDSU & 0x00000004) != 0) { \
    ResourceMark rm; \
    tty->print("[DSU]: "); \
    tty->print_cr args; \
  } while (0)

// Macro for checking if TraceRedefineClasses has a specific bit
// enabled. Returns true if the bit specified by level is set.
#define DSU_TRACE_ENABLED(level) ((TraceDSU & level) != 0)

// Macro for checking if TraceRedefineClasses has one or more bits
// set in a range of bit values. Returns true if one or more bits
// is set in the range from low..high inclusive. Assumes that low
// and high are single bit values.
//
// ((high << 1) - 1)
//     Yields a mask that removes bits greater than the high bit value.
//     This algorithm doesn't work with highest bit.
// ~(low - 1)
//     Yields a mask that removes bits lower than the low bit value.
#define DSU_TRACE_IN_RANGE(low, high) \
(((TraceDSU & ((high << 1) - 1)) & ~(low - 1)) != 0)

// Timer support macros. Only do timer operations if timer tracing
// is enabled. The "while (0)" is so we can use semi-colon at end of
// the macro.
#define DSU_TIMER_START(t) \
  if (DSU_TRACE_ENABLED(0x00000001)) { \
    t.start(); \
  } while (0)
#define DSU_TIMER_STOP(t) \
  if (DSU_TRACE_ENABLED(0x00000001)) { \
    t.stop(); \
  } while (0)

/////////////////////////////////////////////////////
// End of trace definition
////////////////////////////////////////////////////

#endif

