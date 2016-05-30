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


#include "precompiled.hpp"
#include "runtime/dsu.hpp"
#include "runtime/dsuOperation.hpp"
#include "runtime/interfaceSupport.hpp"
#include "runtime/javaCalls.hpp"
#include "memory/oopFactory.hpp"
#include "prims/jni.h"
#include "prims/jvm_misc.hpp"
#include "prims/jvmtiRedefineClasses.hpp"
#include "prims/methodComparator.hpp"
#include "runtime/vm_operations.hpp"
#include "interpreter/bytecodes.hpp"
#include "interpreter/oopMapCache.hpp"
#include "interpreter/rewriter.hpp"
#include "classfile/javaClasses.hpp"
#include "classfile/dictionary.hpp"
#include "classfile/classLoaderData.inline.hpp"
#include "runtime/handles.hpp"
#include "runtime/jniHandles.hpp"
#include "runtime/frame.hpp"
#include "runtime/vframe.hpp"
#include "runtime/vframe_hp.hpp"
#include "runtime/timer.hpp"
#include "runtime/sharedRuntime.hpp"
#include "ci/ciEnv.hpp"
#include "compiler/compileBroker.hpp"
#include "gc_interface/collectedHeap.hpp"
#include "utilities/hashtable.hpp"
#include "oops/klass.inline.hpp"

#include "memory/metadataFactory.hpp"
// -------------------- DSUObject -----------------------------------------

DSUObject::DSUObject()
: _validated(false) {}

DSUObject::~DSUObject() {}

void DSUObject::print() {
  //tty->print_cr("DSUObject state is %d",_state);
}

void DSU::print() {
  //tty->print_cr("DSU state is %d",_state);
}

void DSUClassLoader::print() {
  //tty->print_cr("DSUClassLoader state is %d",_state);
}

void DSUMethod::print() {
  //tty->print_cr("DSUMethod state is %d",_state);
}

// -------------------- DSU -----------------------------------------

DSU::DSU()
: _system_dictionary_modification_number_at_prepare(0),
  _weak_reflection_number_at_prepare(0),
  _from_rn(0),
  _to_rn(0),
  _first_class_loader(NULL),
  _last_class_loader(NULL),
  _request_state(DSU_REQUEST_INIT),
  _dynamic_patch(NULL),
  _next(NULL),
  _shared_stream_provider(NULL) {}

DSUError DSU::prepare(TRAPS) {
  DSUClassLoader* dsu_class_loader = first_class_loader();
  for(; dsu_class_loader != NULL; dsu_class_loader = dsu_class_loader->next()) {
    DSUError ret = dsu_class_loader->prepare(THREAD);
    if (ret != DSU_ERROR_NONE) {
      return ret;
    }
    if (HAS_PENDING_EXCEPTION) {
      return DSU_ERROR_PREPARE_DSU;
    }
  }

  {
    // Acquire the lock in case the system dictionary is modified.
    MutexLocker mu_r(Compile_lock, THREAD);
    this->_system_dictionary_modification_number_at_prepare = SystemDictionary::number_of_modifications();
    this->collect_classes_to_relink(CHECK_(DSU_ERROR_COLLECT_CLASSES_TO_RELINK));
  }

  this->collect_changed_reflections(CHECK_(DSU_ERROR_TO_BE_ADDED));

  int num_of_prepared_class = 0;
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    for(DSUClass *dsu_class = dsu_loader->first_class();dsu_class!=NULL;dsu_class=dsu_class->next()) {
      if (dsu_class->prepared()) {
        num_of_prepared_class++;
      }
    }
  }

  if (num_of_prepared_class == 0) {
    return DSU_ERROR_NO_UPDATED_CLASS;
  }

  return DSU_ERROR_NONE;
}



bool DSU::system_modified() const {
  return _system_dictionary_modification_number_at_prepare != SystemDictionary::number_of_modifications();
}

bool DSU::reflection_modified() const {
  return _weak_reflection_number_at_prepare != JNIHandles::weak_reflection_modification_number();
}

// helper function to iterate all DSU classes.
void update_single_class(DSUClass * dsu_class,TRAPS) {
  dsu_class->update(CHECK);
}


// TODO:
// DSUClassLoader may have some updating action.
// To be considered
void DSU::update_class_loaders(TRAPS) {
  this->classes_do(&update_single_class, CHECK);
}

DSUError DSU::update(TRAPS) {
  elapsedTimer dsu_timer;
  // start timer
  DSU_TIMER_START(dsu_timer);

  // ----------------------------------------------------
  // 1). Check whether we are reaching a DSU safe point.
  // ----------------------------------------------------
  {
    if (system_modified()) {
      DSU_WARN(("System dictionary has been modified after we prepared the DSU."));
      set_request_state(DSU_REQUEST_SYSTEM_MODIFIED);
      return DSU_ERROR_NONE;
    }

    if (reflection_modified()) {
      DSU_WARN(("Reflection has been modified after we prepared the DSU."));
      set_request_state(DSU_REQUEST_SYSTEM_MODIFIED);
      return DSU_ERROR_NONE;
    }
  }

  {
    bool sys_safe = Javelus::check_application_threads();
    if (sys_safe) {
      DSU_INFO(("At safe point, the update will be performed."));
    } else {
      // TODO, here we thinks it needs a flag to control whether install return barrier.
      //If not safe, we perform return barrier installation.
      Javelus::install_return_barrier_all_threads();

      DSU_WARN(("Not at DSU safe point, the DSU is interrupted.."));
      set_request_state(DSU_REQUEST_INTERRUPTED);
      return DSU_ERROR_NONE;
    }
  }

  //----------------------------------------------------------------------
  // 2). perform the dynamic update once we are reaching a DSU safe point
  //----------------------------------------------------------------------

  // 2.1). unlink compiled code
  flush_dependent_code(CHECK_(DSU_ERROR_UPDATE_DSU));

  {
    // TODO class updating may swap contents of java.lang.Class
    // we have to perform the update ahead of that.
    //TraceTime t("Update changed reflection.");
    update_changed_reflection(CHECK_(DSU_ERROR_TO_BE_ADDED));
  }
  // 2.2). update each class contained in this DSU
  update_class_loaders(CHECK_(DSU_ERROR_UPDATE_DSUCLASSLOADER));

  relink_collected_classes(CHECK_(DSU_ERROR_COLLECT_CLASSES_TO_RELINK));

  // 2.3). update bytecode unchanged methods
  {
    //TraceTime t("Repair application threads");
    Javelus::repair_application_threads();
  }

  // 2.4). update objects
  if ( this->is_eager_update() || this->is_eager_update_pointer() ) {
    // !!!! this growableArray is allocated on C heap;
    ResourceMark rm(THREAD);

    // Record DSU Request pausing time without collect objects.
    DSU_TIMER_STOP(dsu_timer);
    DSU_INFO(("DSU request update code time: %3.7f (s).",dsu_timer.seconds()));
    DSU_TIMER_START(dsu_timer);

    GrowableArray<jobject> * results = new GrowableArray<jobject>(1000);
    //DSUEagerUpdate::collect_dead_instances_at_safepoint(Javelus::system_revision_number(),results, thread);
    DSUEagerUpdate::collect_dead_instances_at_safepoint(this->to_rn(),results, THREAD);

    if (HAS_PENDING_EXCEPTION) {
      CLEAR_PENDING_EXCEPTION;
      DSU_WARN(("Collect dead objects eagerly fails! Total %d", results->length()));
    } else {

      DSU_DEBUG(("Javelus::system_revision_number() is %d", Javelus::system_revision_number()));

      DSUEagerUpdate::configure_eager_update(results,this->is_eager_update_pointer());
      DSUEagerUpdate::install_eager_update_return_barrier(THREAD);
      if (HAS_PENDING_EXCEPTION) {
        DSU_WARN(("install eager update barrier error!"));

      }
    }

    DSU_TIMER_STOP(dsu_timer);
    DSU_INFO(("DSU collects %d objects time: %3.7f (s).",results->length(),dsu_timer.seconds()));
    DSU_TIMER_START(dsu_timer);
  } else {
    // For lazy update, we do nothing
  }

  // 2.5). Now we are finish all updating work, do some log
  Javelus::finish_active_dsu();

  // Disable any dependent concurrent compilations
  SystemDictionary::notice_modification();

  // Set up new class path
  set_up_new_classpath(THREAD);

  // Record DSU Request pausing time without collect objects.
  DSU_TIMER_STOP(dsu_timer);
  DSU_INFO(("DSU request pause time: %3.7f (s).",dsu_timer.seconds()));
  if (DSU_TRACE_ENABLED(0x00000001)) {
      time_t tloc;
      time(&tloc);
      tty->print("[DSU]-[Info]: DSU request finished at %s", ctime(&tloc));
  }

  set_request_state(DSU_REQUEST_FINISHED);
  return DSU_ERROR_NONE;
}

void DSU::set_up_new_classpath(TRAPS) {
  ResourceMark rm(THREAD);
  bool has_not_loaded_new_version = false;
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader != NULL; dsu_loader = dsu_loader->next()) {
    ClassLoaderData* loader_data = dsu_loader->class_loader_data();
    assert(loader_data != NULL, "sanity check");
    for (DSUClass *dsu_class = dsu_loader->first_class(); dsu_class != NULL; dsu_class = dsu_class->next()) {
      if (dsu_class->require_new_version() && dsu_class->new_version_class() == NULL) {
        has_not_loaded_new_version = true;
        DSU_WARN(("Add a not loaded new class %s", dsu_class->name()->as_C_string()));
        loader_data->add_to_redefined_list(dsu_class->name());
        loader_data->set_stream_provider(dsu_loader->stream_provider());
      }
    }
  }

  if (has_not_loaded_new_version) {
    // prevent DSU from deallocated it
    set_shared_stream_provider(NULL);
  }
}

// TODO we need to do explicit sanity check.
// to be implemented;
bool DSU::validate(TRAPS) {
  return false;
}

// we need first guess a lowest updating type of each class.
// This should be done when we are building the DSU
// see DSUDynamicPatchBuilder and DSUJvmtiBuilder
// ADDED: DSU_CLASS_ADD
// MODIFIED: DSU_CLASS_NONE
// DELETED: DSU_CLASS_DEL
// only add class does not require a new version.
// TODO we will remove DSU_CLASS_STUB in future.
bool DSUClass::require_old_version() const {
  return updating_type() != DSU_CLASS_ADD;
}

bool DSUClass::old_version_resolved() const{
  return _old_version!=NULL;
}

// only del class does not require a new version.
bool DSUClass::require_new_version() const {
  return updating_type() != DSU_CLASS_DEL;
}

bool DSUClass::new_version_resolved() const{
  return _new_version!=NULL;
}

// Resolve klass and set transformers
DSUError DSUClass::resolve_old_version(InstanceKlass* &old_version, TRAPS) {

  if (!require_old_version()) {
    return DSU_ERROR_NONE;
  }

  if (old_version_resolved()) {
    old_version = this->old_version_class();
    return DSU_ERROR_NONE;
  }

  // make sure the class loader has been resolved.
  if (!dsu_class_loader()->resolved()) {
    dsu_class_loader()->resolve(CHECK_(DSU_ERROR_RESOLVE_CLASS_LOADER_FAILED));
  }

  if (!dsu_class_loader()->resolved()) {
    return DSU_ERROR_RESOLVE_CLASS_LOADER_FAILED;
  }

  assert(dsu_class_loader()->resolved(), "sanity check");

  Symbol* class_name = this->name();

  Handle loader (THREAD, dsu_class_loader()->classloader());
  Handle null_pd;

  //InstanceKlass* klass_h (THREAD, klass());
  //
  Klass* klass = SystemDictionary::find(class_name, loader, null_pd, THREAD);

  if (HAS_PENDING_EXCEPTION) {
    ResourceMark rm(THREAD);
    DSU_WARN(("Find class [%s] error.", class_name->as_C_string()));
    return DSU_ERROR_RESOLVE_OLD_CLASS;
  }

  if (klass == NULL) {
    ResourceMark rm(THREAD);
    DSU_DEBUG(("Could not find loaded class [%s]. Abort parsing of the class. ", class_name->as_C_string()));
    return DSU_ERROR_OLD_CLASS_NOT_LOADED;
  }

  old_version = InstanceKlass::cast(klass);

  if (old_version->is_in_error_state()) {
    DSU_WARN(("Resolve class %s error state.", class_name->as_C_string()));
    return DSU_ERROR_RESOLVE_OLD_CLASS;
  }

  if (!old_version->is_linked()) {
    old_version->link_class(THREAD);
    if (HAS_PENDING_EXCEPTION) {
      Symbol* ex_name = PENDING_EXCEPTION->klass()->name();

      CLEAR_PENDING_EXCEPTION;
      if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
        return DSU_ERROR_OUT_OF_MEMORY;
      } else {
        return DSU_ERROR_LINK_OLD_CLASS;
      }
    }
  }

  set_old_version(old_version);
  assert(old_version != NULL, "old version should not be null.");
  return DSU_ERROR_NONE;
}

DSUError DSUClass::resolve_new_version(InstanceKlass* &new_version, TRAPS) {
  if (!require_new_version()) {
    return DSU_ERROR_NONE;
  }

  if (new_version_resolved()) {
    return DSU_ERROR_NONE;
  }

  if (Thread::current()->is_DSU_thread()) {
    return resolve_new_version_by_dsu_thread(new_version, THREAD);
  }

  return resolve_new_version_at_safe_point(new_version, THREAD);
}


// new class is resolved out of VM safe point.
// so such classes are added into the temporal dictionary.
DSUError DSUClass::resolve_new_version_by_dsu_thread(InstanceKlass* &new_version, TRAPS) {
  // must be invoked in DSU thread
  assert(THREAD->is_DSU_thread(), "sanity check");

  InstanceKlass* k = this->new_version_class();
  if (k != NULL && k->is_klass()) {
    new_version = k;
    return DSU_ERROR_NONE;
  }

  Symbol* class_name = name();

  DSUError ret = dsu_class_loader()->load_new_version(class_name, new_version, stream_provider(), CHECK_(DSU_ERROR_RESOLVE_NEW_CLASS));

  if (ret != DSU_ERROR_NONE) {
    return ret;
  }

  if (new_version != NULL) {
    set_new_version(new_version);
    Javelus::add_dsu_klass(new_version,THREAD);
  }

  return ret;
}

// TODO, I am not sure whether we can resolve new version at safe point
// To te removed in future
DSUError DSUClass::resolve_new_version_at_safe_point(InstanceKlass* &new_version, TRAPS) {
  ShouldNotReachHere();
  return DSU_ERROR_NONE;
}

// set restricted methods in old version.
void DSUClass::set_restricted_methods() {
  DSUMethod * dsu_method = first_method();

  // as a deleted class has no new version
  // we cannot compare and normalize them
  // Just blindly set all methods as restricted and DEL here.
  if (this->updating_type() == DSU_CLASS_DEL) {
    for (; dsu_method != NULL; dsu_method = dsu_method->next()) {
      dsu_method->set_updating_type(DSU_METHOD_DEL);
      Method* method = dsu_method->method();
      method->set_is_restricted_method();
    }
    return;
  }

  for (; dsu_method != NULL; dsu_method = dsu_method->next()) {
    Method* method = dsu_method->method();
    DSU_TRACE(0x00000008, ("method %s::%s.%s is restricted. %d",
        this->name()->as_C_string(),
        method->name()->as_C_string(),
        method->signature()->as_C_string(),
        dsu_method->updating_type())
        );

    if (dsu_method->updating_type() == DSU_METHOD_BC) {
      method->set_is_restricted_method();
    } else if (dsu_method->updating_type() == DSU_METHOD_DEL) {
      method->set_is_restricted_method();
    }
  }
}

void DSUClass::set_check_flags_for_methods(InstanceKlass* old_version,
  InstanceKlass* new_version, TRAPS) {
  HandleMark hm(THREAD);

  assert(new_version != NULL, "new_version must not be null.");
  Array<Method*>* methods = new_version->methods();

  assert(methods != NULL,"methods must not be null.");
  int length = methods->length();

  for (int i = 0; i < length; i++) {
    Method* m = methods->at(i);
    methodHandle mh(THREAD, m);
    if (m->name() == vmSymbols::class_initializer_name()) {
      mh->link_method(mh, CHECK);
      continue;
    }

    if (m->is_static()) {
      mh->link_method(mh, CHECK);
      continue;
    }

    m->set_needs_stale_object_check();
    m->link_method_with_dsu_check(mh, CHECK);
  }
}

// lookup along the hierarchy,
// find the youngest match (may be changed) super class
// if no super class has been redefined.
// The ycsc is just ycmc.
void DSUClass::compute_youngest_common_super_class(InstanceKlass* old_version, InstanceKlass* new_version,
  InstanceKlass* &old_ycsc, InstanceKlass* &new_ycsc, TRAPS) {
  old_ycsc = old_version->superklass();

  while(old_ycsc != NULL) {
    new_ycsc = new_version->superklass();
    while(new_ycsc != NULL) {
      if (new_ycsc->name() == old_ycsc->name()) {
        break;
      }
      new_ycsc = new_ycsc->superklass();
    }

    if (new_ycsc != NULL) {
      break;
    }

    old_ycsc = old_ycsc->superklass();
  }

  // after calculate ycsc, we can use HandleMark now

  // before check whether this class is type narrowed due to a class,
  // we check whether it removes an interface before.
  compute_interfaces_information(old_version, new_version, CHECK);

  {
    HandleMark hm(THREAD);
    // every class has an common super class, which is java.lang.Object.
    assert(old_ycsc != NULL && new_ycsc != NULL, "should not be null");
    assert(new_ycsc->name() == old_ycsc->name(), "invariant");

    InstanceKlass* old_super = old_version->superklass();
    InstanceKlass* new_super = new_version->superklass();

    if (old_super != old_ycsc) {
      // one super type has missed, the class must be type narrowed.
      new_version->set_is_type_narrowed_class();
    }

    while(old_super != old_ycsc) {
      if (!old_super->is_stale_class()) {
        old_super->set_is_super_type_of_stale_class();
        old_super->set_is_type_narrowing_relevant_type();
      }
      old_super = old_super->superklass();
    }

    while(new_super != new_ycsc) {
      new_super->set_is_super_type_of_stale_class();
      if (new_ycsc->is_type_narrowed_class()) {
        new_super->set_is_type_narrowed_class();
      }
      new_super = new_super->superklass();
    }
  }

}

void DSUClass::compute_interfaces_information(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS) {
  ResourceMark rm(THREAD);
  HandleMark hm(THREAD);
  // we only set_is_super_type_of_stale_class for interfaces
  // used by instanceof in compiler.
  Array<Klass*>* old_interfaces = old_version->transitive_interfaces();
  Array<Klass*>* new_interfaces = new_version->transitive_interfaces();

  // same array
  if (old_interfaces == new_interfaces) {
    return;
  }

  int old_length = old_interfaces->length();
  int new_length = new_interfaces->length();

  if (old_length == 0) {
    // mark all new interfaces as super type of stale class
    for(int j=0; j<new_length; j++) {
      Klass* new_itfc = new_interfaces->at(j);
      new_itfc->set_is_super_type_of_stale_class();
    }
    return;
  }

  if (new_length == 0) {
    for(int i=0; i<old_length; i++) {
      Klass* old_itfc = old_interfaces->at(i);
      old_itfc->set_is_type_narrowing_relevant_type();
      old_itfc->set_is_super_type_of_stale_class();
    }
    return;
  }

  assert(old_length != 0, "sanity check");
  assert(new_length != 0, "sanity check");

  int count = new_length;

  int* new_states = NEW_RESOURCE_ARRAY(int, new_length);
  memset(new_states, 0, new_length*sizeof(int));

  enum{
    init = 0,
    common_interface = 1,
    match_interface = 2,
  };


  for(int i=0; i<old_length; i++) {
    Klass* old_itfc = old_interfaces->at(i);
    // in case where old interface is not stale and type narrowed.
    int j=0;
    for(; j<new_length; j++) {
      Klass* new_itfc = new_interfaces->at(j);
      if (old_itfc == new_itfc) {
        // common interface
        new_states[j] = common_interface;
        count--;
        break;
      }
      if (old_itfc->name() == new_itfc->name()) {
        // updated interface
        new_itfc->set_is_type_narrowed_class();
        new_states[j] = match_interface;
        count--;
        break;
      }
    }
    if (j == new_length) {
      // we break, so that the old_itfc may be a type narrowing relevant class
      if (old_itfc->is_stale_class()) {
        // set the corresponding new version as type narrowing relevant class
        Klass* new_of_old_itfc = (InstanceKlass::cast(old_itfc)->next_version());
        assert(new_of_old_itfc != NULL, "sanity check");
        new_of_old_itfc->set_is_type_narrowing_relevant_type();
        new_of_old_itfc->set_is_super_type_of_stale_class();
      } else {
        old_itfc->set_is_type_narrowing_relevant_type();
        old_itfc->set_is_super_type_of_stale_class();
      }
      // anyway, we have to set the new class as type narrowed class
      new_version->set_is_type_narrowed_class();
    }
  }

  if (count == 0) {
    return ;
  }

  // we have some new interface that is neither common or matched.
  for(int i=0; i<new_length; i++) {
    if (new_states[i] == 0) {
      Klass* new_itfc = new_interfaces->at(i);
      new_itfc->set_is_super_type_of_stale_class();
    }
  }
}

void DSUClass::compute_and_cache_common_info(InstanceKlass* old_version,
  InstanceKlass* new_version, TRAPS) {
  HandleMark hm(THREAD);
  // compute and set min_vtable_size
  int min_vtable_length = old_version->vtable_length();
  int min_object_size   = old_version->size_helper();
  InstanceKlass* previous_version = old_version->previous_version();
  while(previous_version != NULL) {
    int vl = previous_version->vtable_length();
    int os = previous_version->size_helper();
    previous_version = previous_version->previous_version();
    if ( vl < min_vtable_length) {
      min_vtable_length = vl;
    }
    if ( os < min_object_size) {
      min_object_size = os;
    }
  }
  this->set_min_vtable_size(min_vtable_length);
  this->set_min_object_size_in_bytes(min_object_size << LogBytesPerWord);
}

// This could be done before entering VM safe point at prepare phase.
void DSUClass::compute_and_set_fields(InstanceKlass* old_version,
  InstanceKlass* new_version, TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  // compute
  compute_and_cache_common_info(old_version, new_version, CHECK);

  // TODO
  // if (!new_version->is_initialized()) {
  //   typeArrayOop r = oopFactory::new_typeArray(T_INT, 0, CHECK);
  //   if (java_lang_Class::init_lock(old_version->java_mirror()) == NULL) {
  //     tty->print_cr("Null ");
  //   }
  //   java_lang_Class::set_init_lock(old_version->java_mirror(), r);
  // }

  bool do_print = false;/*old_version->name()->starts_with("org/apache/catalina/core/ContainerBase")*/;

  // We walk through the chain of evolution to find the minimal object size.
  const int min_object_size = this->min_object_size_in_bytes();

  const int old_object_size = old_version->size_helper() << LogBytesPerWord ;
  const int new_object_size = new_version->size_helper() << LogBytesPerWord;

  const int old_fields_count = old_version->java_fields_count();
  const int new_fields_count = new_version->java_fields_count();

  int match_instance_index = 0;
  const int match_size = (new_fields_count > old_fields_count? new_fields_count : old_fields_count) * DSUClass::next_matched_field;
  int match_static_index = match_size;

  // TODO: use a bitmap instead
  u1* match_fields = NEW_RESOURCE_ARRAY(u1, match_size);
  char* field_space = NEW_RESOURCE_ARRAY(char, new_object_size);

  if (field_space == NULL) {
    return ;
  }

  enum {
    padding     =  0,
    inplace_field =  1,
    phantom_field =  2,
  };

  //XXX !!!!
  memset(field_space, padding, new_object_size);

  const bool eager_update = this->dsu_class_loader()->dsu()->is_eager_update();
  const bool eager_update_pointers = this->dsu_class_loader()->dsu()->is_eager_update_pointer();

  ConstantPool* old_constants = old_version->constants();
  ConstantPool* new_constants = new_version->constants();

  for (int i = 0; i < new_fields_count; i++) {
    FieldInfo*  new_field = new_version->field(i);
    u2 n_flag = new_field->access_flags();
    u2 n_dsu_flag = new_field->dsu_flags();

    Symbol* n_name = new_field->name(new_constants);
    Symbol* n_sig  = new_field->signature(new_constants);

    u4 n_offset     = new_field->offset();
    BasicType n_field_type = FieldType::basic_type(n_sig);
    int n_field_size = type2aelembytes(n_field_type);
    int n_field_end  = n_offset + n_field_size;

    bool is_field_in_phantom_object = false;

    if ((n_flag & JVM_ACC_STATIC) == 0  ) {
      // this field is an instance field
      // We compare end of field with minimal object size to decide possible MixNewField.
      if (n_field_end > min_object_size /*old_object_size*/) {
        // this field is in the phantom object
        is_field_in_phantom_object = true;
        if (do_print) {
          tty->print_cr("[DSU] Set invalid and mix new Member, %s.", n_name->as_C_string());
        }
        memset(field_space+n_offset, (char)phantom_field, n_field_size);
        if (eager_update) {
          // no invalid checks but with phantom_field checks
          n_dsu_flag = (u2)(n_dsu_flag | DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK);
        } else if (eager_update_pointers) {
          // n_dsu_flag is n flag
        } else {
          n_dsu_flag = (u2)(n_dsu_flag | DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK | DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK);
        }
        new_field->set_dsu_flags(n_dsu_flag);
      } else {
        // this field is in the inplace object.
        if (do_print) {
          tty->print_cr("[DSU] Set invalid and mix old Member, %s.", n_name->as_C_string());
        }
        memset(field_space+n_offset, (char)inplace_field, n_field_size);
        if (!(eager_update || eager_update_pointers)) {
          n_dsu_flag = (u2)(n_dsu_flag | DSU_FLAGS_MEMBER_NEEDS_STALE_OBJECT_CHECK);
        }
        new_field->set_dsu_flags(n_dsu_flag);
      }// end of inplace field
    }// end of determining inplace or phantom fields.

    // Calculate match fields information
    for (int j = 0; j < old_fields_count; j++) {
      FieldInfo*  old_field = old_version->field(j);
      u2 o_flag = old_field->access_flags();
      u2 o_dsu_flag = old_field->dsu_flags();
      Symbol* o_name = old_field->name(old_constants);
      Symbol* o_sig  = old_field->signature(old_constants);

      if (o_name == n_name && o_sig == n_sig ) {
        u4 o_offset = old_field->offset();
        u1 m_flags = 0;
        if (((n_flag & JVM_ACC_STATIC) == 0 ) && ((o_flag & JVM_ACC_STATIC) == 0)) {
          // a pair of matched instance fields
          // XXX flags may change,for example private => public
          // we use negative offset to indicate the field is in the phantom object.
          if ((o_dsu_flag & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) != 0) {
            m_flags |= 0x01;
          }

          // we use negative offset to indicate the field is a MixNewField.
          if ((n_dsu_flag & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) != 0) {
            m_flags |= 0x02;
          }

          explode_int_to(o_offset, &match_fields[match_instance_index + DSUClass::matched_field_old_offset]);
          explode_int_to(n_offset, &match_fields[match_instance_index + DSUClass::matched_field_new_offset]);
          match_fields[match_instance_index + DSUClass::matched_field_flags] = m_flags;
          match_fields[match_instance_index + DSUClass::matched_field_type] = n_field_type;
          match_instance_index += DSUClass::next_matched_field;
          break;
        } else if (((n_flag & JVM_ACC_STATIC) != 0 ) && ((o_flag & JVM_ACC_STATIC) != 0)) {
          // a pair of match static fields
          match_static_index -= DSUClass::next_matched_field;
          explode_int_to(o_offset, &match_fields[match_static_index + DSUClass::matched_field_old_offset]);
          explode_int_to(n_offset, &match_fields[match_static_index + DSUClass::matched_field_new_offset]);
          match_fields[match_static_index + DSUClass::matched_field_flags] = m_flags;
          match_fields[match_static_index + DSUClass::matched_field_type] = n_field_type;
          break;
        } else {
          DSU_WARN(("Matched fields flags changed. [%s %s %s].", new_version->name()->as_C_string(),o_name->as_C_string(),o_sig->as_C_string()));
        }
      }// end of find a matched fields
    }// end of search matched fields
  }

  // find the youngest matched super class (ycsc)
  // the matched super classes may be changed
  InstanceKlass* old_ycsc = NULL;
  InstanceKlass* new_ycsc = NULL;

  compute_youngest_common_super_class(old_version, new_version, old_ycsc, new_ycsc, CHECK);

  // TODO
  if (old_ycsc == new_ycsc) {
    this->set_old_ycsc(old_ycsc);
    this->set_new_ycsc(new_ycsc);
  } else {
    this->set_old_ycsc(old_ycsc);
    this->set_new_ycsc(new_ycsc);
  }

  assert(old_ycsc != NULL, "sanity check");
  assert(new_ycsc != NULL, "sanity check");

  // Start of setting matched instance fields
  // Direct super of new class
  InstanceKlass* super_old = old_version->superklass();
  Array<u1>*     super_matched_fields = new_ycsc->matched_fields();

  int match_instance_length = 0; // total array length
  int super_match_instance_length = 0; // super array length
  if (super_matched_fields != NULL) {
    super_match_instance_length = super_matched_fields->length();
    match_instance_length += super_match_instance_length;
  }
  // match_instance_index is the array length of matched instance fields of this class.
  match_instance_length += match_instance_index; // now become total array length
  if (match_instance_length > 0) {
    // total matched entry plus one more clean entry
    Array<u1>* n_matched_fields = MetadataFactory::new_array<u1>(new_version->class_loader_data(),
      match_instance_length + DSUClass::next_matched_field, CHECK);
    assert(n_matched_fields != NULL, "sanity check");
    if (super_match_instance_length > 0) {
      memcpy(n_matched_fields->adr_at(0), super_matched_fields->adr_at(0), super_match_instance_length*sizeof(u1));
    }
    if (match_instance_index > 0) {
      memcpy(n_matched_fields->adr_at(super_match_instance_length), match_fields, match_instance_index*sizeof(u1));
    }
    //TODO we use (flags & 0x04 == 0) to indicate this is an clean tuple.
    // the matched_field_old_offset is the beginning offset.
    // the matched_field_new_offset is the range to be cleaed.
    // We clean memory from (matched_field_old_offset) to (matched_field_old_offset + matched_field_new_offset).
    // After that, we can safely copy single field from temporal object to the old object.
    u4 old_ycsc_object_size = old_ycsc->size_helper() << LogHeapWordSize;
    u4 to_be_cleaned = old_object_size - old_ycsc_object_size;
    explode_int_to(old_ycsc_object_size, n_matched_fields->adr_at(match_instance_length + DSUClass::matched_field_old_offset));
    explode_int_to(to_be_cleaned, n_matched_fields->adr_at(match_instance_length + DSUClass::matched_field_new_offset));
    n_matched_fields->at_put(match_instance_length + DSUClass::matched_field_flags, 0x04);
    n_matched_fields->at_put(match_instance_length + DSUClass::matched_field_type, 0);
    new_version->set_matched_fields(n_matched_fields);
  } // End of setting matched instance fields

  // Start of setting matched static fields
  int match_static_count = match_size - match_static_index;
  if (match_static_count > 0) {
    u1 *match_static_fields = NEW_C_HEAP_ARRAY(u1, match_static_count, mtInternal);
    memcpy(match_static_fields, match_fields+match_static_index, match_static_count*sizeof(u1));
    this->set_match_static_count(match_static_count);
    this->set_match_static_fields(match_static_fields);
  }
  // End of setting matched static fields

  const int inplace_length = new_fields_count * DSUClass::next_inplace_field;
  u1* inplace_fields = NEW_RESOURCE_ARRAY(u1, inplace_length);

  bool is_inplace = false;
  int inplace_index = 0;
  u4  inplace_offset = 0;
  for (int i=0; i<new_object_size; i++) {
    if (field_space[i] == inplace_field) {
      if (is_inplace == false) {
        is_inplace = true;
        inplace_offset = i;
      }
    } else {
      if (is_inplace == true) {
        is_inplace = false;
        explode_int_to(inplace_offset, &inplace_fields[inplace_index + DSUClass::inplace_field_offset]);
        explode_int_to((u4)(i - inplace_offset), &inplace_fields[inplace_index + DSUClass::inplace_field_length]);
        inplace_index += DSUClass::next_inplace_field;
      }
    }
  }

  int instance_header_size_in_bytes = instanceOopDesc::header_size() << LogBytesPerWord;

  InstanceKlass* super  = new_version->superklass();
  // size in bytes
  const int new_super_object_size = super->size_helper() << LogBytesPerWord;
  int super_delta = new_super_object_size - instance_header_size_in_bytes;
  Array<u1>* super_inplace_fields = super->inplace_fields();
  int inplace_count = 0;
  int super_inplace_count = 0;
  if (super_inplace_fields != NULL) {
    // we have redefined super classes
    super_inplace_count = super_inplace_fields->length();
    inplace_count += super_inplace_count;
  } else if (super_delta > 0) {
    // we have not redefined super classes
    // and super class has fields
    super_inplace_count = DSUClass::next_inplace_field;
    inplace_count += super_inplace_count;
  }
  inplace_count += inplace_index;

  if (inplace_count > 0) {
    Array<u1>* n_inplace_fields = MetadataFactory::new_array<u1>(new_version->class_loader_data(), inplace_count, CHECK);
    if (super_inplace_count > 0) {
      if (super_inplace_fields != NULL) {
        memcpy(n_inplace_fields->adr_at(0), super_inplace_fields->adr_at(0), super_inplace_count*sizeof(u1));
      } else {
        //
        explode_int_to((u4)(instance_header_size_in_bytes), n_inplace_fields->adr_at(0 + DSUClass::inplace_field_offset));
        explode_int_to((u4)(super_delta), n_inplace_fields->adr_at(0 + DSUClass::inplace_field_length));
      }
    }
    if (inplace_index > 0) {
      memcpy(n_inplace_fields->adr_at(super_inplace_count), inplace_fields, inplace_index * sizeof(u1));
    }
    new_version->set_inplace_fields(n_inplace_fields);
  }
}


DSUError DSUClass::prepare(TRAPS) {
  HandleMark hm(THREAD);
  if (prepared()) {
    // already prepared
    return DSU_ERROR_NONE;
  }

  DSUError ret;
  ResourceMark rm(THREAD);
  InstanceKlass* old_version = NULL;
  ret = resolve_old_version(old_version, THREAD);

  if (ret == DSU_ERROR_OLD_CLASS_NOT_LOADED && IgnoreUnloadedOldClass) {
    return DSU_ERROR_NONE;
  }

  if (ret != DSU_ERROR_NONE) {
    //return ret;
    return ret;
  }

  if (updating_type() == DSU_CLASS_DEL) {
    if (old_version != NULL) {
      set_restricted_methods();
      old_version->set_dsu_state(DSUState::dsu_will_be_deleted);
      _prepared = true;
    } else {
      ResourceMark rm(THREAD);
      DSU_WARN(("A deleted class [%s] cannot be resolved.", this->name()->as_C_string()));
    }
    return DSU_ERROR_NONE;
  }

  if (IgnoreAddedClass && updating_type() == DSU_CLASS_ADD) {
    // _prepared = true;
    return DSU_ERROR_NONE;
  }

  InstanceKlass* new_version = NULL;
  ret = resolve_new_version(new_version, THREAD);

  if (ret != DSU_ERROR_NONE && IgnoreUnloadedAddedClass) {
    return DSU_ERROR_NONE;
  }

  if (ret != DSU_ERROR_NONE) {
    return ret;
  }

  if (updating_type() == DSU_CLASS_ADD) {
    if (new_version != NULL) {
      _prepared = true;
    } else {
      ResourceMark rm(THREAD);
      DSU_WARN(("An added class [%s] cannot be resolved.", this->name()->as_C_string()));
    }
    return DSU_ERROR_NONE;
  }

  assert(old_version != NULL, "old version should not be null.");
  assert(new_version != NULL, "new version should not be null.");

  // prepare super classes of new version here
  if (old_version != NULL) {
    Javelus::prepare_super_class(old_version, CHECK_(DSU_ERROR_PREPARE_SUPER_FAILED));
    Javelus::prepare_interfaces(old_version, CHECK_(DSU_ERROR_PREPARE_INTERFACES_FAILED));
  }

  ret = refresh_updating_type(old_version, new_version, THREAD);

  assert(updating_type() != DSU_CLASS_UNKNOWN, "sanity check");

  if (ret != DSU_ERROR_NONE) {
    return ret;
  }

  DSUClassUpdatingType type = this->updating_type();
  switch(type) {
    case DSU_CLASS_MC:
      break;
    case DSU_CLASS_BC:
      set_restricted_methods();
      old_version->set_dsu_state(DSUState::dsu_will_be_swapped);
      break;
    case DSU_CLASS_SMETHOD:
    case DSU_CLASS_SFIELD:
    case DSU_CLASS_SBOTH:
    case DSU_CLASS_METHOD:
    case DSU_CLASS_FIELD:
    case DSU_CLASS_BOTH:
      // set flags for fields
      compute_and_set_fields(old_version, new_version,CHECK_(DSU_ERROR_TO_BE_ADDED));
      // set flags for methods
      set_restricted_methods();
      resolve_transformer(CHECK_(DSU_ERROR_RESOLVE_TRANSFORMER_FAILED));
      build_new_inplace_new_class_if_necessary(CHECK_(DSU_ERROR_TO_BE_ADDED));
      build_stale_new_class_if_necessary(CHECK_(DSU_ERROR_TO_BE_ADDED));
      old_version->set_dsu_state(DSUState::dsu_will_be_redefined);
      break;
    case DSU_CLASS_DEL:
      break;
    case DSU_CLASS_ADD:
      break;
    case DSU_CLASS_NONE:
      DSU_WARN(("An unchanged class %s.", this->name()->as_C_string()));
      // TODO, The classes may require to relink.
      // Here, we only remove it from the class loader
      // and append it during collect_classes_to_relink
      // this->dsu_class_loader()->remove_class(this);
      break;
    case DSU_CLASS_UNKNOWN:
      break;
    default:
      ShouldNotReachHere();
  }

  if (ret != DSU_ERROR_NONE) {
    return ret;
  }

  // only ret == DSU_ERROR_NONE
  _prepared = true;

  return DSU_ERROR_NONE;
}

DSUError DSUClass::resolve_transformer(TRAPS) {
  InstanceKlass* transformer = dsu_class_loader()->transformer_class();
  if (transformer == NULL) {
    return DSU_ERROR_NONE;
  }
  InstanceKlass* old_version = this->old_version_class();
  Symbol* class_name = this->name();

  Array<Method*>* methods = transformer->methods();
  int length = methods->length();
  if (length > 0) {
    // TODO really bad code
    const char * prefix = "(L";
    //const char * subfix = ")V";
    int len_pre = (int)strlen(prefix);
    //int len_sub = strlen(subfix);
    int str_length = class_name->utf8_length()  + len_pre /* + len_sub*/;
    char * buf = NEW_RESOURCE_ARRAY(char, str_length + 1);
    sprintf(buf, "%s%s", prefix, class_name->as_C_string()/*,subfix*/);
    DSU_DEBUG(("Create signature:%s, length:%d",buf,str_length));

    for (int i=0; i<length; i++) {
      Method* method = methods->at(i);
      if (method->signature()->starts_with(buf, str_length)) {
        if (method->name()->equals("updateClass")) {
          if (class_transformer_method() == NULL) {
            DSU_DEBUG(("Set class transformer during parsing."));
            set_class_transformer_method(method);
            Array<u1>* annotations = method->parameter_annotations();
            Array<u1>* result = NULL;
            Javelus::parse_old_field_annotation(old_version, transformer,
              annotations, result, CHECK_(DSU_ERROR_RESOLVE_OLD_FIELD_ANNOTATION));
            if (result != NULL) {
              set_class_transformer_args(result);
            }
          } else {
            DSU_WARN(("Duplicated class transformer!"));
          }
        } else if (method->name()->equals("updateObject")) {
          if (object_transformer_method() == NULL) {
            DSU_DEBUG(("Set object transformer during parsing."));
            set_object_transformer_method(method);
            Array<u1>* annotations = method->parameter_annotations();
            Array<u1>* result;
            Javelus::parse_old_field_annotation(old_version, transformer,
              annotations, result, CHECK_(DSU_ERROR_RESOLVE_OLD_FIELD_ANNOTATION));
            if (result = NULL) {
              set_object_transformer_args(result);
            }
          } else {
            DSU_WARN(("Duplicated object transformer."));
          }
        }
      }
    }
  }

  return DSU_ERROR_NONE;
}

bool DSUClass::require_new_inplace_new_class(TRAPS) const {
  InstanceKlass* old_version = this->old_version_class();
  if (old_version == NULL) {
    return false;
  }

  InstanceKlass* new_version = new_version_class();

  if (new_version == NULL) {
    return false;
  }

  DSUClassUpdatingType updating_type = this->updating_type();
  if (updating_type >= DSU_CLASS_FIELD && updating_type <= DSU_CLASS_BOTH) {
    int min_old_instance_size = old_version->size_helper();
    int new_instance_size = new_version->size_helper();

    InstanceKlass* previous_version = old_version->previous_version();
    while(previous_version != NULL) {
      int os = previous_version->size_helper();
      previous_version = previous_version->previous_version();
      if (os < min_old_instance_size) {
        min_old_instance_size = os;
      }
    }

    //return old_instance_size != new_instance_size;
    return min_old_instance_size < new_instance_size;
  }
  return false;
}

// a temp version is used to identify the object is in temp state.
// that means, it has been transferred to a uninitialized new object.
// then, we will invoke transformer on it.
bool DSUClass::require_stale_new_class(TRAPS) const{
  InstanceKlass* new_version = this->new_version_class();
  if (new_version == NULL) {
    return false;
  }

  DSUClassUpdatingType updating_type = this->updating_type();
  if (updating_type != DSU_CLASS_FIELD && updating_type != DSU_CLASS_BOTH) {
    return false;
  }

  // return new_version->matched_fields() != NULL || object_transformer_method() != NULL;
  return true;
}


DSUError DSUClass::build_new_inplace_new_class_if_necessary(TRAPS) {
  bool require = require_new_inplace_new_class(CHECK_(DSU_ERROR_CREATE_MIX_VERSION_FAILED));

  if (!require) {
    return DSU_ERROR_NONE;
  }

  HandleMark    hm(THREAD);
  ResourceMark  rm(THREAD);

  InstanceKlass* new_inplace_new_class = NULL;
  InstanceKlass* old_version = this->old_version_class();
  InstanceKlass* new_version = this->new_version_class();

  new_inplace_new_class = InstanceKlass::clone_instance_klass(new_version,THREAD);

  const int old_object_size = old_version->size_helper() << LogBytesPerWord ;

  //has the old object size
  new_inplace_new_class->set_layout_helper(old_version->layout_helper());

  OopMapBlock* oop_map     = new_version->start_of_nonstatic_oop_maps();
  OopMapBlock* inplace_map = new_inplace_new_class->start_of_nonstatic_oop_maps();
  const int oop_map_count  = new_version->nonstatic_oop_map_count();

  assert(!UseCompressedOops, "not implemented");

  {
    for(int i=0; i< oop_map_count; i++) {
      int off_start = oop_map[i].offset();
      int off_count = oop_map[i].count();
      int off_end   = off_start + heapOopSize * off_count;
      if (off_end > old_object_size) {
        if (off_start > old_object_size) {
          inplace_map[i].set_count(0);
        } else {
          int c1_count = (old_object_size - off_start) / heapOopSize;
          inplace_map[i].set_count(c1_count);
        }
      }
    }
  }

  assert(new_inplace_new_class->is_linked(), "mix class is linked ..");

  new_inplace_new_class->set_init_state(InstanceKlass::fully_initialized);
  new_inplace_new_class->set_copy_to_size(new_version->size_helper());

  set_new_inplace_new_class(new_inplace_new_class);

  return DSU_ERROR_NONE;
}

// Note that stale new class
DSUError DSUClass::build_stale_new_class_if_necessary(TRAPS) {
  bool require = require_stale_new_class(CHECK_(DSU_ERROR_CREATE_TEMP_VERSION_FAILED));

  if (!require) {
    return DSU_ERROR_NONE;
  }

  HandleMark    hm(THREAD);
  ResourceMark  rm(THREAD);

  // 1). create temp class
  InstanceKlass* source = NULL;
  InstanceKlass* stale_new_class = NULL;
  InstanceKlass* new_version = this->new_version_class();

  if (new_inplace_new_class() != NULL) {
    source = this->new_inplace_new_class();
  } else {
    source = new_version;
  }

  stale_new_class = InstanceKlass::clone_instance_klass(source, CHECK_(DSU_ERROR_CREATE_TEMP_VERSION_FAILED));
  stale_new_class->set_copy_to_size(new_version->size_helper());

  set_stale_new_class(stale_new_class);

  return DSU_ERROR_NONE;
}

DSUError DSUClass::refresh_updating_type(InstanceKlass* old_version, InstanceKlass* new_version, TRAPS) {
  if (updating_type() != DSU_CLASS_NONE) {
    return DSU_ERROR_NONE;
  }

  if (old_version == NULL) {
    // old class has not been resolved
    return DSU_ERROR_RESOLVE_OLD_CLASS;
  }

  if (new_version == NULL) {
    // error here, new classes must be loaded here.
    return DSU_ERROR_RESOLVE_NEW_CLASS;
  }

  DSUError ret;
  ret = compare_and_normalize_class(old_version, new_version, THREAD);

  if (ret != DSU_ERROR_NONE) {
    // handle error here
    return ret;
  }

  return DSU_ERROR_NONE;
}

DSUClassUpdatingType Javelus::join(DSUClassUpdatingType this_type, DSUClassUpdatingType that_type) {
  switch(this_type) {
  case DSU_CLASS_UNKNOWN:
  case DSU_CLASS_NONE:
    return that_type;
  case DSU_CLASS_MC:
    // no upgrade from MC
    ShouldNotReachHere();
    break;
  case DSU_CLASS_BC:
  case DSU_CLASS_INDIRECT_BC:
  case DSU_CLASS_SMETHOD:
  case DSU_CLASS_SFIELD:
  case DSU_CLASS_SBOTH:
  case DSU_CLASS_METHOD:
  case DSU_CLASS_FIELD:
  case DSU_CLASS_BOTH:
    return this_type>that_type ? this_type:that_type;
  case DSU_CLASS_ADD:
  case DSU_CLASS_DEL:
    // no upgrade
    assert(this_type == that_type, "sanity");
    return this_type;
  case DSU_CLASS_STUB:
    // STUB should be deprecated
    // no preparation for STUB
  default:
    ShouldNotReachHere();
  }
  return DSU_CLASS_UNKNOWN;
}

DSUError DSUClass::compare_and_normalize_class(InstanceKlass* old_version,
  InstanceKlass* new_version, TRAPS) {
  const DSUClassUpdatingType old_updating_type = updating_type();
  DSUClassUpdatingType new_updating_type = DSU_CLASS_NONE;

  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  assert(old_version->name() == new_version->name(), "name should be the same");
  assert(old_version->class_loader() == new_version->class_loader(), "class loader should also be the same");
  assert(old_version->super() != NULL , "javelus does not updating library classes");
  assert(new_version->super() != NULL , "javelus does not updating library classes");


  // XXX Compare the name, since body changed class could be ...
  // first check whether super is the same

  {
    InstanceKlass* old_super = old_version->superklass();
    InstanceKlass* new_super = new_version->superklass();
    if (old_super != new_super) {
      // super class has changed
      if (old_super->name() != new_super->name()
        || old_super->class_loader() != new_super->class_loader() ) {
          // really different
      }

      DSUClassUpdatingType super_updating_type = Javelus::get_updating_type(old_super, CHECK_(DSU_ERROR_GET_UPDATING_TYPE));
      if (super_updating_type == DSU_CLASS_DEL) {
        // TODO deleted super class cannot propagate to sub classes.
        // In fact, the new updating type should be DSU_CLASS_BOTH any way.
        new_updating_type = DSU_CLASS_BOTH;
      } else {
        new_updating_type = Javelus::join(new_updating_type, super_updating_type);
      }
    }
  }

  {
    Array<Klass*>* k_old_interfaces = old_version->local_interfaces();
    Array<Klass*>* k_new_interfaces = new_version->local_interfaces();
    int n_intfs = k_old_interfaces->length();
    if (n_intfs != k_new_interfaces->length()) {
      //new_updating_type = DSU_CLASS_BOTH;
      new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
    } else {
      // same size, compare elements seperatedly
      for (int i = 0; i < n_intfs; i++) {
        Klass* old_interface = k_old_interfaces->at(i);
        Klass* new_interface = k_new_interfaces->at(i);
        if (old_interface != new_interface) {
          if (old_interface->name() != new_interface->name()
            || old_interface->class_loader() != new_interface->class_loader() ) {
              // really different
          }
          DSUClassUpdatingType interface_updating_type = Javelus::get_updating_type(InstanceKlass::cast(old_interface), CHECK_(DSU_ERROR_GET_UPDATING_TYPE));
          if (interface_updating_type == DSU_CLASS_DEL) {
            // TODO deleted super class cannot propagate to sub classes.
            // In fact, the new updating type should be DSU_CLASS_BOTH any way.
            new_updating_type = DSU_CLASS_BOTH;
          } else {
            new_updating_type = Javelus::join(new_updating_type, interface_updating_type);
          }
        }
      }
    }
  }

  if (old_version->is_in_error_state()) {
    // TBD #5057930: special error code is needed in 1.6
    assert(false, "to be decided");
  }

  u2 old_flags = old_version->access_flags().get_flags();
  u2 new_flags = new_version->access_flags().get_flags();
  if (old_flags != new_flags) {
    //new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BC);
    //new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_NONE);
    //assert(false, "to be considered");
    new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
  }


  {
    /****************************************************/
    /* Here we do a very coarse grained comparison.     */
    /****************************************************/
    //
    // Check if the number, names, types and order of fields declared in these classes
    // are the same.
    int n_fields = old_version->java_fields_count();
    if (n_fields != new_version->java_fields_count()) {
      // TBD: Should we distinguish DSU_CLASS_SFIELD and DSU_CLASS_FIELD here??
      //new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_FIELD);
      new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
    } else {
      for (int i = 0; i < n_fields; i++) {
        FieldInfo* old_field = old_version->field(i);
        FieldInfo* new_field = new_version->field(i);
        // access
        old_flags = old_field->access_flags();
        new_flags = new_field->access_flags();
        if ((old_flags ^ new_flags) & JVM_RECOGNIZED_FIELD_MODIFIERS) {
          // TBD: Should we distinguish DSU_CLASS_SFIELD and DSU_CLASS_FIELD here??
          new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
        }
        // offset
        if (old_field->offset() != new_field->offset()) {
            new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
        }
        // name and signature
        Symbol* name_sym1 = old_field->name(old_version->constants());
        Symbol* sig_sym1  = old_field->name(old_version->constants());
        Symbol* name_sym2 = new_field->name(new_version->constants());
        Symbol* sig_sym2  = new_field->name(new_version->constants());
        if (name_sym1 != name_sym2 || sig_sym1 != sig_sym2) {
          new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BOTH);
        }
      }
    }
  }

  {
    // Do a parallel walk through the old and new methods. Detect
    // cases where they match (exist in both), have been added in
    // the new methods, or have been deleted (exist only in the
    // old methods).  The class file parser places methods in order
    // by method name, but does not order overloaded methods by
    // signature.  In order to determine what fate befell the methods,
    // this code places the overloaded new methods that have matching
    // old methods in the same order as the old methods and places
    // new overloaded methods at the end of overloaded methods of
    // that name. The code for this order normalization is adapted
    // from the algorithm used in instanceKlass::find_method().
    // Since we are swapping out of order entries as we find them,
    // we only have to search forward through the overloaded methods.
    // Methods which are added and have the same name as an existing
    // method (but different signature) will be put at the end of
    // the methods with that name, and the name mismatch code will
    // handle them.
    Array<Method*>* k_old_methods = old_version->methods();
    Array<Method*>* k_new_methods = new_version->methods();
    int n_old_methods = k_old_methods->length();
    int n_new_methods = k_new_methods->length();

    int ni = 0;
    int oi = 0;
    while (true) {
      Method* k_old_method;
      Method* k_new_method;
      DSUMethod* dsu_method = NULL;
      enum { matched, added, deleted, undetermined } method_was = undetermined;

      if (oi >= n_old_methods) {
        if (ni >= n_new_methods) {
          break; // we've looked at everything, done
        }
        // New method at the end
        k_new_method = k_new_methods->at(ni);
        method_was = added;
      } else if (ni >= n_new_methods) {
        // Old method, at the end, is deleted
        k_old_method = k_old_methods->at(oi);
        method_was = deleted;
      } else {
        // There are more methods in both the old and new lists
        k_old_method = k_old_methods->at(oi);
        k_new_method = k_new_methods->at(ni);
        if (k_old_method->name() != k_new_method->name()) {
          // Methods are sorted by method name, so a mismatch means added
          // or deleted
          if (k_old_method->name()->fast_compare(k_new_method->name()) > 0) {
            method_was = added;
          } else {
            method_was = deleted;
          }
        } else if (k_old_method->signature() == k_new_method->signature()) {
          // Both the name and signature match
          method_was = matched;
        } else {
          // The name matches, but the signature doesn't, which means we have to
          // search forward through the new overloaded methods.
          int nj;  // outside the loop for post-loop check
          for (nj = ni + 1; nj < n_new_methods; nj++) {
            Method* mh = k_new_methods->at(nj);
            if (k_old_method->name() != mh->name()) {
              // reached another method name so no more overloaded methods
              method_was = deleted;
              break;
            }
            if (k_old_method->signature() == mh->signature()) {
              // found a match so swap the methods
              k_new_methods->at_put(ni, mh);
              k_new_methods->at_put(nj, k_new_method);
              k_new_method = mh;
              method_was = matched;
              break;
            }
          }

          if (nj >= n_new_methods) {
            // reached the end without a match; so method was deleted
            method_was = deleted;
          }
        }
      }

      switch (method_was) {
      case matched:
        // methods match, be sure modifiers do too
        {
          // allocate a DSUMethod
          dsu_method = this->allocate_method(k_old_method, CHECK_(DSU_ERROR_GET_UPDATING_TYPE));
          dsu_method->set_updating_type(DSU_METHOD_NONE);
          dsu_method->set_matched_new_method_index(ni);
          // compare the method implementation here

          jint old_flags = k_old_method->access_flags().as_int() & 0xFFFF;
          jint new_flags = k_new_method->access_flags().as_int() & 0xFFFF;
          if (old_flags != new_flags) {
            DSU_WARN(("WARNING:: %s old %d and new %d", k_old_method->name_and_sig_as_C_string(),
                old_flags,
                new_flags));
            new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_METHOD);
          }

          if (!MethodComparator::methods_EMCP(k_old_method, k_new_method)) {
            dsu_method->set_updating_type(DSU_METHOD_BC);
            DSU_TRACE(0x00000008, ("byte code of method %s::%s.%s is changed.",
                  this->name()->as_C_string(),
                  k_old_method->name()->as_C_string(),
                  k_old_method->signature()->as_C_string()));
            new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_BC);
          } else {
            DSU_TRACE(0x00000008, ("method %s::%s.%s is NOT changed.",
                  this->name()->as_C_string(),
                  k_old_method->name()->as_C_string(),
                  k_old_method->signature()->as_C_string()));
            // on stack repair
            // should we build method data here??
            // For the same unchanged methods,
            // they will be deoptimized.
            Method::build_interpreter_method_data(k_new_method, THREAD);
            if (HAS_PENDING_EXCEPTION) {
              assert((PENDING_EXCEPTION->is_a(SystemDictionary::OutOfMemoryError_klass())), "we expect only an OOM error here");
              CLEAR_PENDING_EXCEPTION;
            }
          }
        }
        RC_TRACE(0x00008000, ("Method matched: new: %s [%d] == old: %s [%d]",
          k_new_method->name_and_sig_as_C_string(), ni,
          k_old_method->name_and_sig_as_C_string(), oi));
        // advance to next pair of methods
        ++oi;
        ++ni;
        break;
      case added:
        // method added, see if it is OK
        new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_METHOD);
        RC_TRACE(0x00008000, ("Method added: new: %s [%d]",
          k_new_method->name_and_sig_as_C_string(), ni));
        ++ni; // advance to next new method
        break;
      case deleted:
        new_updating_type = Javelus::join(new_updating_type, DSU_CLASS_METHOD);

        // allocate a DSUMethod here;
        dsu_method = this->allocate_method(k_old_method, CHECK_(DSU_ERROR_GET_UPDATING_TYPE));
        dsu_method->set_updating_type(DSU_METHOD_DEL);

        RC_TRACE(0x00008000, ("Method deleted: old: %s [%d]",
          k_old_method->name_and_sig_as_C_string(), oi));
        ++oi; // advance to next old method
        break;
      default:
        ShouldNotReachHere();
      }
    }
  }

  if (old_updating_type == new_updating_type) {
    // warn here
    //ShouldNotReachHere();
    return DSU_ERROR_NONE;
  }


  DSU_TRACE(0x00000008, ("updating type of class %s is %s",
        this->name()->as_C_string(),
        Javelus::class_updating_type_name(new_updating_type)));
  this->set_updating_type(new_updating_type);

  return DSU_ERROR_NONE;
}

void DSUClass::update(TRAPS) {
  if (!prepared()) {
    return;
  }
  DSUClassUpdatingType updating_type = this->updating_type();

  InstanceKlass* klass = NULL;
  switch (updating_type) {
  case DSU_CLASS_NONE:
    break;
  case DSU_CLASS_STUB:
    // a stub class
    klass = this->old_version_class();
    if (klass != NULL) {
      klass->set_dsu_state(DSUState::dsu_has_been_added);
    }
    break;
  case DSU_CLASS_MC:
    relink_class(THREAD);
    break;
  case DSU_CLASS_BC:
    swap_class(THREAD);
    break;
  case DSU_CLASS_SFIELD:
  case DSU_CLASS_SMETHOD:
  case DSU_CLASS_SBOTH:
  case DSU_CLASS_FIELD:
  case DSU_CLASS_METHOD:
  case DSU_CLASS_BOTH:
    redefine_class(THREAD);
    break;
  case DSU_CLASS_DEL:
    // set restricted method
    undefine_class(THREAD);
    break;
  case DSU_CLASS_ADD:
    // TODO, eager or lazy adding is a question, especially depending on the application.
    // we would like to add a flag to control this.
    if (!IgnoreAddedClass) {
      define_class(THREAD);
    }
    break;
  case DSU_CLASS_UNKNOWN:
    break;
  default:
    ShouldNotReachHere();
  }

  if (HAS_PENDING_EXCEPTION) {
    Handle h_e (PENDING_EXCEPTION);
    ResourceMark rm(THREAD);
    DSU_WARN(("Updating class %s results in an exception.",
          this->name()->as_C_string()
          ));
    java_lang_Throwable::print(h_e, tty);
    return;
  }
}

void DSUClass::swap_class(TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  DSUClassUpdatingType updating_type  = this->updating_type();
  InstanceKlass* old_version = this->old_version_class();

  DSU_TRACE(0x000000400,
    ("Swap single class %s, change type is %s",old_version->name()->as_C_string(),
     Javelus::class_updating_type_name(updating_type))
    );

  InstanceKlass* new_version = this->new_version_class();

  assert(new_version != NULL, "sanity check");


  // We can update here before swap,
  // Now, the new_version is totally new.
  update_jmethod_ids(new_version,CHECK);

  // Swap methods
  Array<Method*>* old_methods = old_version->methods();
  old_version->set_methods(new_version->methods());
  new_version->set_methods(old_methods);

  //Swap fields
  Array<u2>* old_fields = old_version->fields();
  int java_fields_count = old_version->java_fields_count();
  old_version->set_fields(NULL, java_fields_count);
  old_version->set_fields(new_version->fields(), java_fields_count);
  new_version->set_fields(NULL, java_fields_count);
  new_version->set_fields(old_fields, java_fields_count);

  ConstantPool* old_constants = old_version->constants();
  old_version->set_constants(new_version->constants());
  new_version->constants()->set_pool_holder(old_version);
  new_version->set_constants(old_constants);
  old_constants->set_pool_holder(new_version);

  Array<u2>* old_inner_classes = old_version->inner_classes();
  old_version->set_inner_classes(new_version->inner_classes());
  new_version->set_inner_classes(old_inner_classes);

  // The class file bytes from before any retransformable agents mucked
  // with them was cached on the scratch class, move to the_class.
  // Note: we still want to do this if nothing needed caching since it
  // should get cleared in the_class too.
  if (old_version->get_cached_class_file_bytes() == 0) {
    // the_class doesn't have a cache yet so copy it
    old_version->set_cached_class_file(new_version->get_cached_class_file());
  } else if (new_version->get_cached_class_file_bytes() !=
           old_version->get_cached_class_file_bytes()) {
    // The same class can be present twice in the scratch classes list or there
    // are multiple concurrent RetransformClasses calls on different threads.
    // In such cases we have to deallocate scratch_class cached_class_file.
    os::free(new_version->get_cached_class_file());
  }

  // NULL out in scratch class to not delete twice.  The class to be redefined
  // always owns these bytes.
  new_version->set_cached_class_file(NULL);

  {
    // Old version has been swapped with new methods
    // Here we re-create the vtable and itable.
    ResourceMark rm(THREAD);
    // no exception should happen here since we explicitly
    // do not check loader constraints.
    // compare_and_normalize_class_versions has already checked:
    //  - classloaders unchanged, signatures unchanged
    //  - all instanceKlasses for redefined classes reused & contents updated

    old_version->vtable()->initialize_vtable(false, THREAD);
    old_version->itable()->initialize_itable(false, THREAD);

    assert(!HAS_PENDING_EXCEPTION || (THREAD->pending_exception()->is_a(SystemDictionary::ThreadDeath_klass())), "redefine exception");

#ifdef ASSERT
    old_version->vtable()->verify(tty);
#endif
  }

    // Copy the "source file name" attribute from new class version
  old_version->set_source_file_name_index(
    new_version->source_file_name_index());

  // Copy the "source debug extension" attribute from new class version
  old_version->set_source_debug_extension(
    new_version->source_debug_extension(),
    new_version->source_debug_extension() == NULL ? 0 :
    (int)strlen(new_version->source_debug_extension()));

  if (new_version->access_flags().has_localvariable_table() !=
    old_version->access_flags().has_localvariable_table()) {

      AccessFlags flags = old_version->access_flags();
      if (new_version->access_flags().has_localvariable_table()) {
        flags.set_has_localvariable_table();
      } else {
        flags.clear_has_localvariable_table();
      }
      old_version->set_access_flags(flags);
  }

  // Replace class annotation fields values
  Annotations* old_class_annotations = old_version->annotations();
  old_version->set_annotations(new_version->annotations());
  new_version->set_annotations(old_class_annotations);

  // Replace minor version number of class file
  u2 old_minor_version = old_version->minor_version();
  old_version->set_minor_version(new_version->minor_version());
  new_version->set_minor_version(old_minor_version);

  // Replace major version number of class file
  u2 old_major_version = old_version->major_version();
  old_version->set_major_version(new_version->major_version());
  new_version->set_major_version(old_major_version);

  // Replace CP indexes for class and name+type of enclosing method
  u2 old_class_idx  = old_version->enclosing_method_class_index();
  u2 old_method_idx = old_version->enclosing_method_method_index();
  old_version->set_enclosing_method_indices(
    new_version->enclosing_method_class_index(),
    new_version->enclosing_method_method_index());
  new_version->set_enclosing_method_indices(old_class_idx, old_method_idx);

  // Replace lifetime information.
  int old_born_rn = old_version->born_rn();
  // as we have not put the new version into the class hierarchy,
  // so we cannot swap born directly.
  // In fact, the true born rn is system_revision_number + 1.
  // this->from_rn == this->dsu()->from_rn()
  //               == system_revision_number (when activated) + 1
  old_version->set_born_rn(this->from_rn());
  new_version->set_born_rn(old_born_rn);

  //TODO  Should we add scratch class in the revision chain??
  //old_version->set_previous_version(new_version());
  //XXX Rember this is used for repair application threads
  new_version->set_next_version(old_version);

  // XXX Make old class dead here.
  new_version->set_dead_rn(this->to_rn());
  new_version->set_is_stale_class();
  new_version->set_copy_to_size(new_version->size_helper());
  new_version->set_dsu_state(DSUState::dsu_has_been_swapped);
  // the class now is new and clear the DSU state of it.
  old_version->set_dsu_state(DSUState::dsu_none);

  //XXX update method flags here.
  //Very important
  mark_old_and_obsolete_methods(CHECK);

  assert(!old_version->is_stale_class(), "sanity check");
  assert(new_version->is_stale_class(), "sanity check");

  if (old_version->oop_map_cache() != NULL) {
    // Flush references to any obsolete methods from the oop map cache
    // so that obsolete methods are not pinned.
    old_version->oop_map_cache()->flush_obsolete_entries();
  }

  //java_lang_Class::update_mirror(old_version, new_version);
  //new_version->set_java_mirror(old_version->java_mirror());

  // as we swap the methods, we should update the vtable and itable accordingly
  update_subklass_vtable_and_itable(old_version,THREAD);

  {// update java mirror
    oop old_java_mirror = old_version->java_mirror();
    oop new_java_mirror = new_version->java_mirror();

    InstanceKlass * java_lang_Class = InstanceKlass::cast(old_java_mirror->klass());
    Javelus::copy_fields(new_java_mirror, old_java_mirror, java_lang_Class);
  }

#ifdef ASSERT
  old_version->vtable()->verify(tty);
#endif
}

void DSUClass::update_subklass_vtable_and_itable(InstanceKlass* old_version, TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);
  InstanceKlass* subklass = (InstanceKlass*)(old_version->subklass());
  while(subklass != NULL) {
    if (!(subklass->dsu_will_be_redefined() || subklass->dsu_will_be_swapped())) {
      {
        ResourceMark rm(THREAD);

        subklass->vtable()->initialize_vtable(false, THREAD);
        subklass->itable()->initialize_itable(false, THREAD);
        assert(!HAS_PENDING_EXCEPTION || (THREAD->pending_exception()->is_a(SystemDictionary::ThreadDeath_klass())), "redefine exception");
      }
      update_subklass_vtable_and_itable(subklass,THREAD);
    }
    subklass = (InstanceKlass*)subklass->next_sibling();
  }
}

void DSUClass::undefine_class(TRAPS) {
  //TODO do nothing for CV(Concurrent Version)
  //Here, we only mark methods as invalid members.
  //since only dynamic dispatching can trigger deleted members.
  InstanceKlass* ik = this->old_version_class();
  if (ik == NULL) {
    return ;
  }
  if (!ik->is_linked()) {
    return;
  }


  ik->set_dsu_state(DSUState::dsu_has_been_deleted);

  ik->set_is_stale_class();
  //TODO, It seems that we need cast an object of a deleted class to its alive parent class.
  //ik->set_force_update(true);
  ik->set_dead_rn(this->to_rn());


  // 1). We first make all methods defined in deleted class non-executable.
  this->mark_old_and_obsolete_methods(CHECK);

  // 2). Make all entries in the virtual table inherited from super class to be
  klassVtable * vtable = ik->vtable();
  const int super_vtable_length = InstanceKlass::cast(ik->super())->vtable_length();
  for (int i=0; i<super_vtable_length; i++) {
    Method* method = vtable->method_at(i);
    InstanceKlass * method_ik = method->method_holder();
    if (method_ik->dsu_has_been_redefined()) {
      InstanceKlass * new_ik = method_ik->next_version();
      // TODO, here should be optimized, allocate a new vtable is unnecessary.
      (*vtable->adr_method_at(i)) = new_ik->vtable()->method_at(i);
    } else if (method_ik->dsu_has_been_deleted()) {
      // Here, the deleted super classes must already be set with a valid method.
      (*vtable->adr_method_at(i)) = method_ik->vtable()->method_at(i);
    }
  }

  // TODO, we only treat remove deleted classes as redefine  the deleted class to its alive super classes.
}

void DSUClass::relink_class(TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  InstanceKlass* old_version = this->old_version_class();

  if (!old_version->is_linked()) {
    DSU_DEBUG(("Relink an unlinked class [%s].", old_version->name()->as_C_string()));
    return ;
  }

  DSU_TRACE(0x00000800,("Relink a linked class [%s].", old_version->name()->as_C_string()));

  ConstantPool* constants = old_version->constants();
  int index = 1;
  for (index = 1; index < constants->length(); index++) {
    // Index 0 is unused
    Klass* entry;
    jbyte tag = constants->tag_at(index).value();
    switch (tag) {
    case JVM_CONSTANT_Class :
      entry = constants->klass_at(index,THREAD);
      if (entry->is_klass()) {
        if (entry->oop_is_instance()) {
          constants->unresolved_klass_at_put(index, entry->name());
        }
      }
      break;
    case JVM_CONSTANT_Long :
    case JVM_CONSTANT_Double :
      index++;
      break;
    default:
      break;
    } // end of switch
  }

  // We should repatch all VM bytecodes to Java bytecode

  Array<Method*>* methods = old_version->methods();
  int length = methods->length();

  bool print_replace = false;//old_version->name()->starts_with("org/apache/catalina/core/ContainerBase");

  for(int i=0; i<length; i++) {
    Method* method = methods->at(i);

    Javelus::repatch_method(method, print_replace,THREAD);

    //method->set_method_data(NULL);
    MethodData* md = method->method_data();
    if (md != NULL) {
      for (ProfileData* data = md->first_data();
        md->is_valid(data);
        data = md->next_data(data)) {
          if (data->is_ReceiverTypeData()) {
            ReceiverTypeData * rtd = (ReceiverTypeData*)data;
            for (uint row = 0; row < rtd->row_limit(); row++) {
              Klass* recv = rtd->receiver(row);
              if (recv != NULL ) {
                if (recv->is_stale_class()) {
                  Klass* next = InstanceKlass::cast(recv)->next_version();
                  // next may be null and next version
                  // TODO if we allow concurrent update, here next should the latest one?
                  assert(next != NULL,"next version cannot be null");
                  rtd->set_receiver(row,next);
                }
              }
            }
          }
      }
    }
  }

  ConstantPoolCache* cache =  constants->cache();
  assert(cache != NULL, "cache must not be null.");
  cache->reset_all_entries();

  old_version->set_dsu_state(DSUState::dsu_has_been_recompiled);
}

void DSUClass::redefine_class(TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  DSUClassUpdatingType updating_type  = this->updating_type();

  InstanceKlass* old_version = this->old_version_class();
  InstanceKlass* new_version = this->new_version_class();

  // 0x00000200 see dsu.hpp
  DSU_TRACE(0x00000200,
    ("Redefine single class %s, change type is %s.",
    old_version->name()->as_C_string(),
    Javelus::class_updating_type_name(updating_type))
    );

  //this->update_jmethod_ids(new_version, THREAD);
  //this->mark_old_and_obsolete_methods(THREAD);
  this->install_new_version(old_version, new_version, THREAD);

  // -------------------------------------------------------------------------
  // Because we have create vtable and itable at load_new_class_for_redefing.
  // And if there exists an class that is swapped with another one, the old
  // content has been updated at that time.
  // -------------------------------------------------------------------------
  {
    // Check and update new super which may be swapped.
    // We only need to update the super class, as interfaces are never swapped.
    InstanceKlass* new_super = InstanceKlass::cast(new_version->super());

    if (new_super->is_stale_class()) {
      assert(new_super->dsu_state() ==  DSUState::dsu_has_been_swapped, "sanity check");
      new_super = new_super->next_version();
      // as when we perform update, all classes involved should not be stale!!!
      // The surface of the versioned class hierarchy
      assert(!new_super->is_stale_class(), "next super should not be invalid.");
    }

    assert(!new_super->is_stale_class(), "should not be invalid.");
  }
  {
    ResourceMark rm(THREAD);
    // no exception should happen here since we explicitly
    // do not check loader constraints.
    // compare_and_normalize_class_versions has already checked:
    //  - classloaders unchanged, signatures unchanged
    //  - all instanceKlasses for redefined classes reused & contents updated

    klassVtable * new_vtable = new_version->vtable();
    const int new_vtable_length = new_vtable->length();
    bool stale = false;
    for(int i=0; i<new_vtable_length; i++) {
      Method* method = new_vtable->method_at(i);
      if (method->method_holder()->is_stale_class()) {
        stale = true;
        break;
      }
    }

    if (stale) {
      new_version->vtable()->initialize_vtable(false, CHECK);
      new_version->itable()->initialize_itable(false, CHECK);
      assert(!HAS_PENDING_EXCEPTION || (THREAD->pending_exception()->is_a(SystemDictionary::ThreadDeath_klass())), "redefine exception");
    }

#ifdef ASSERT
    for(int i=0; i<new_vtable_length; i++) {
      Method* method = new_vtable->method_at(i);
      assert(!method->method_holder()->is_stale_class(),"sanity check");
    }
#endif
  }


  // copy values of static fields from old instanceKlass to new instanceKlass
  apply_default_class_transformer(old_version, new_version, CHECK);

  // Some fix action, to be refactor
  post_fix_old_version            (old_version, new_version, CHECK);
  post_fix_new_version            (old_version, new_version, CHECK);
  post_fix_new_inplace_new_class  (old_version, new_version, CHECK);
  post_fix_stale_new_class        (old_version, new_version, CHECK);
  post_fix_vtable_and_itable      (old_version, new_version, CHECK);

  {// update java mirror
    oop old_java_mirror = old_version->java_mirror();
    oop new_java_mirror = new_version->java_mirror();

    InstanceKlass * java_lang_Class = InstanceKlass::cast(old_java_mirror->klass());
    Javelus::copy_fields(new_java_mirror, old_java_mirror, java_lang_Class);
    // all mirrors points to the new_version
    // all cached reflections are nullified.
  }
}

void DSUClass::install_new_version(InstanceKlass* old_version, InstanceKlass* new_version, TRAPS) {
  SystemDictionary::redefine_instance_class(old_version, new_version, THREAD);
  // we need set here.
  old_version->set_next_version(new_version);
  new_version->set_previous_version(old_version);
}

void DSUClass::apply_default_class_transformer(InstanceKlass* old_version, InstanceKlass* new_version, TRAPS) {
  int count = this->match_static_count();
  if (count == 0) {
    return;
  }
  u1* matched_fields = this->match_static_fields();

  oop old_mirror = old_version->java_mirror();
  oop new_mirror = new_version->java_mirror();
  assert(old_mirror != new_mirror, "sanity check");
  // copy unchanged static fields from old instanceKlass to new instanceKlass
  for (int i = 0; i < count; i += DSUClass::next_matched_field) {
    int old_offset = build_u4_from(&matched_fields[i + DSUClass::matched_field_old_offset]);
    int new_offset = build_u4_from(&matched_fields[i + DSUClass::matched_field_new_offset]);
    BasicType type = (BasicType) matched_fields[i + DSUClass::matched_field_type];
    u1 flags = matched_fields[i + DSUClass::matched_field_flags];
    assert(flags == 0, "static fields has 0 flag");
    Javelus::copy_field(old_mirror, new_mirror, old_offset, new_offset, type);
  }
}

// mark methods in the old class as old and invalid
// TODO, should move these code to post_fix_old_version
void DSUClass::mark_old_and_obsolete_methods(TRAPS) {
  HandleMark hm(THREAD);
  DSUMethod* dsu_method = this->first_method();
  for (; dsu_method!=NULL; dsu_method=dsu_method->next()) {
    Method* method = dsu_method->method();
    method->set_is_old();

    if (method->is_static() || method->name() == vmSymbols::object_initializer_name()
      || method->name()== vmSymbols::class_initializer_name()) {
        continue;
    }

    method->set_needs_stale_object_check();
    method->link_method_with_dsu_check(methodHandle(THREAD, method), THREAD);
  }
}

// TODO jmethodID is associated with idnum.
// However, we cannot maintain idnum when we delete a method as the methods array may shrink.
// (We can, but there is a lot of work to do.)
// We should hack every use of jmethodID
// Two cases:
// 1). In swap_class: new_version is in the old_version's space
// 2). In redefine_class: new_version is just the new_version
void DSUClass::update_jmethod_ids(InstanceKlass* new_version, TRAPS) {
  DSUMethod* dsu_method = this->first_method();
  Array<Method*>* new_methods = new_version->methods();
  for (; dsu_method!=NULL; dsu_method=dsu_method->next()) {
    Method* old_method = dsu_method->method();
    int matched_index = dsu_method->matched_new_method_index();
    if (matched_index < 0) {
      continue;
    }
    jmethodID jmid = old_method->find_jmethod_id_or_null();
    if (jmid != NULL) {
      // TODO this works as we can make the old jmethodID points the new method.
      Method::change_method_associated_with_jmethod_id(jmid, new_methods->at(matched_index));
    }
  }
}

// do some fixes after we have redefine a class
void DSUClass::post_fix_old_version(InstanceKlass* old_version,
  InstanceKlass* new_version, TRAPS) {
  DSUClassUpdatingType updating_type = this->updating_type();

  if (old_version->oop_map_cache() != NULL) {
    // Flush references to any obsolete methods from the oop map cache
    // so that obsolete methods are not pinned.
    old_version->oop_map_cache()->flush_obsolete_entries();
  }

  {
    // fix references in array klass
    Klass* array_klasses = old_version->array_klasses();
    if (array_klasses != NULL) {
      ResourceMark rm(THREAD);

      ObjArrayKlass * oak = ObjArrayKlass::cast(array_klasses);
      oak->set_element_klass(new_version);
      oak->set_bottom_klass(new_version);

      Klass* high_dimension = oak->higher_dimension();
      while(high_dimension != NULL) {
        oak = ObjArrayKlass::cast(high_dimension);
        oak->set_bottom_klass(new_version);
        high_dimension = oak->higher_dimension();
      }

      if (new_version->array_klasses()== NULL) {
        new_version->set_array_klasses(array_klasses);
      }
    }
  }

  if (updating_type != DSU_CLASS_FIELD && updating_type != DSU_CLASS_BOTH) {
    // TODO see comments on _transform_level in instanceKlass.hpp
    old_version->set_transformation_level(1);
  }
  // the class died during an update to to_rn
  // XXX Mark the class as dead here.
  old_version->set_is_stale_class();

  old_version->set_dead_rn(this->to_rn());
  old_version->set_dsu_state(DSUState::dsu_has_been_redefined);

  // Continuous Update Support
  if (old_version->new_inplace_new_class() != NULL) {
    InstanceKlass* old_new_inplace_new_class = old_version->new_inplace_new_class();
    old_new_inplace_new_class->set_is_stale_class();
    old_new_inplace_new_class->set_dead_rn(this->to_rn());
  }

  //XXX old version should in stale state
  mark_old_and_obsolete_methods(THREAD);
}

void DSUClass::post_fix_new_version(InstanceKlass* old_version,
  InstanceKlass* new_version,
  TRAPS) {
  int min_vtable_length = this->min_vtable_size();
  assert(min_vtable_length > 0, "must be preseted");

  // XXX This could be moved at loading time.
  // fields definition has changed
  // need create c1 and c2 and check mix new field
  Array<Method*>* methods = new_version->methods();
  int length = methods->length();
  for(int i=0; i<length; i++) {
    Method* method = methods->at(i);
    if (method->is_static()) {
      continue;
    }
    method->set_needs_stale_object_check();
    if (method->is_private()) {
      continue;
    }
    if (method->vtable_index() > min_vtable_length/*old_vtable_length*/) {
      method->set_needs_mixed_object_check();
    }
  }
  // TODO
  set_check_flags_for_methods(old_version, new_version, CHECK);

  InstanceKlass* superik = new_version->superklass();
  assert(superik != NULL, "should not be null");
  int dead_rn = superik->dead_rn();
  if (dead_rn == this->to_rn()) {
    // a swapped super class
    new_version->set_super(superik->next_version());
    new_version->initialize_supers(superik->next_version(), THREAD);
  }

  assert(new_version->java_mirror() != NULL, "Now the java mirror contains static fields.");

  // we also set transfromer here
  {
    new_version->set_class_transformer(class_transformer_method());
    new_version->set_class_transformer_args(class_transformer_args());

    new_version->set_object_transformer(object_transformer_method());
    new_version->set_object_transformer_args(object_transformer_args());
  }

  new_version->set_is_super_type_of_stale_class();
  //
  new_version->set_is_new_redefined_class();
  // We should adjust the born rn of scratch class to increment by one.
  new_version->set_born_rn(this->to_rn());

  // XXX
  update_jmethod_ids(new_version, THREAD);


  const int old_size_in_words = old_version->size_helper();
  const int new_size_in_words = new_version->size_helper();
  const int size_delta_in_words = old_size_in_words - new_size_in_words;
  if (size_delta_in_words > 0 && size_delta_in_words < (int) CollectedHeap::min_fill_size()) {
    // TODO make new size larger to avoid zap
    new_version->set_layout_helper(old_version->layout_helper());
    new_version->set_copy_to_size(old_version->layout_helper());
    if (this->stale_new_class() != NULL) {
      this->stale_new_class()->set_layout_helper(old_version->layout_helper());
      this->stale_new_class()->set_copy_to_size(old_version->layout_helper());
    }
    assert(this->new_inplace_new_class() == NULL, "must not be mixed object");
    assert(new_version->size_helper() == old_version->size_helper(), "must be equal");
  }
}

void DSUClass::post_fix_new_inplace_new_class(InstanceKlass* old_version,
  InstanceKlass* new_version,
  TRAPS) {
  HandleMark   hm(THREAD);
  ResourceMark rm(THREAD);

  InstanceKlass* new_inplace_new_class = this->new_inplace_new_class();

  if (new_inplace_new_class == NULL) {
    return;
  }

  {
    int new_vtable_length = new_version->vtable_length();
    int new_itable_length = new_version->itable_length();

    if (new_vtable_length > 0) {
      memcpy(new_inplace_new_class->start_of_vtable(),new_version->start_of_vtable(), new_vtable_length * HeapWordSize);
    }

    if (new_itable_length > 0) {
      // always > 0...
      memcpy(new_inplace_new_class->start_of_itable(),new_version->start_of_itable(), new_itable_length * HeapWordSize);
    }
  }

  // XXX
  // TODO Here we just make it easy to ..
  old_version->set_new_inplace_new_class(this->new_inplace_new_class());
  new_inplace_new_class->set_previous_version(old_version);
  new_inplace_new_class->set_next_version(new_version);

  // XXX hack on type system of JVM
  new_inplace_new_class->set_super(NULL);
  new_inplace_new_class->set_secondary_supers(NULL);
  new_inplace_new_class->initialize_supers(new_version, THREAD);

  if (HAS_PENDING_EXCEPTION) {
    tty->print_cr("[DSU] Initialize super error for mix class.");
  }

  new_inplace_new_class->set_is_inplace_new_class();
  new_inplace_new_class->set_array_klasses(new_version->array_klasses());

  // Increment born rn here after update...
  new_inplace_new_class->set_born_rn(this->to_rn());
  new_inplace_new_class->set_prototype_header(markOopDesc::biased_locking_prototype());
}

void DSUClass::post_fix_stale_new_class(InstanceKlass* old_version,
  InstanceKlass* new_version,
  TRAPS) {
  HandleMark   hm(THREAD);
  ResourceMark rm(THREAD);

  InstanceKlass* stale_new_class = this->stale_new_class();

  if (stale_new_class == NULL) {
    return;
  }

  {
    // vtable may be updated due to a swapped super class.
    int new_vtable_length = new_version->vtable_length();
    int new_itable_length = new_version->itable_length();

    //copy vtable from scratch to mix
    if (new_vtable_length > 0) {
      // memcpy copy contents in bytes but the length is in sizeof Heapword
      memcpy(stale_new_class->start_of_vtable(), new_version->start_of_vtable(), new_vtable_length * HeapWordSize);
    }

    if (new_itable_length > 0) {
      // always > 0...
      memcpy(stale_new_class->start_of_itable(), new_version->start_of_itable(), new_itable_length * HeapWordSize);
    }
  }
  // XXX
  // TODO Here we just make it easy to ..
  // TODO stale new does not has a next version
  old_version->set_stale_new_class(stale_new_class);
  stale_new_class->set_previous_version(old_version);
 // This will cause stale object check
  stale_new_class->set_is_stale_class();
  stale_new_class->set_dead_rn(this->to_rn());

  // Make type checking happy.
  stale_new_class->set_super(NULL);
  stale_new_class->set_secondary_supers(NULL);
  stale_new_class->initialize_supers(new_version, CHECK);
}

void DSUClass::post_fix_vtable_and_itable(InstanceKlass* old_version,
    InstanceKlass* new_version,
    TRAPS) {
  {
    klassVtable* old_vt = old_version->vtable();
    klassVtable* new_vt = new_version->vtable();
    assert(old_vt != NULL, "sanity check");
    assert(new_vt != NULL, "sanity check");
    int length = old_vt->length() < new_vt->length() ? old_vt->length() : new_vt->length();
    for (int i = 0; i < length; i++) {
      Method* old_method = old_vt->method_at(i);
      Method* new_method = new_vt->method_at(i);
      assert(old_method != NULL, "sanity check");
      assert(new_method != NULL, "sanity check");
      if (new_method->needs_stale_object_check()) {
        (*old_vt->adr_method_at(i)) = new_method;
      }
    }
  }

  {
    klassItable* old_it = old_version->itable();
    klassItable* new_it = new_version->itable();
    if (old_it != NULL && new_it != NULL) {
      for (int i = 0; i < old_it->size_offset_table(); i++) {
        assert(old_it->offset_entry(i) != NULL, "sanity check");
        Klass* old_itfc = old_it->offset_entry(i)->interface_klass();
        if (old_itfc == NULL) {
          break;
        }
        for (int j = 0; j < new_it->size_offset_table(); j++) {
          assert(new_it->offset_entry(j) != NULL, "sanity check");
          Klass* new_itfc = new_it->offset_entry(j)->interface_klass();
          if (new_itfc == NULL) {
            break;
          }
          if (old_itfc != new_itfc) {
            continue;
          }

          itableMethodEntry* old_ie = old_it->offset_entry(i)->first_method_entry(old_version);
          itableMethodEntry* new_ie = new_it->offset_entry(j)->first_method_entry(new_version);
          int count = klassItable::method_count_for_interface(old_itfc);
          for (int k = 0; k < count; k++, old_ie++, new_ie++) {
            if (new_ie->method() == NULL) {
              break;
            }
            if (new_ie->method()->needs_stale_object_check()) {
              old_ie->initialize(new_ie->method());
            }
          }
          break;
        } // inner for loop
      } // for loop
    } // out if
  }

}

void DSUClass::define_class(TRAPS) {
  HandleMark hm(THREAD);
  InstanceKlass* old_version = NULL;
  InstanceKlass* new_version = new_version_class();
  if (new_version == NULL) {
    return;
  }
  SystemDictionary::redefine_instance_class(old_version, new_version, CHECK);
}

// ------------------- DSU -----------------------------

void DSU::add_class_loader(DSUClassLoader* class_loader) {
  if (_first_class_loader == NULL) {
    assert(_last_class_loader == NULL, "sanity check");
    _first_class_loader = _last_class_loader = class_loader;
    return;
  }

  assert(_last_class_loader != NULL, "sanity check");
  class_loader->set_next(NULL);
  _last_class_loader->set_next(class_loader);
  _last_class_loader = _last_class_loader->next();
}

DSUClassLoader* DSU::allocate_class_loader(Symbol* id, Symbol* lid, TRAPS) {
  if (id == NULL || id->utf8_length() == 0 ) {
    id = SymbolTable::lookup("", 0, CHECK_NULL);
  }

  if (lid == NULL || lid->utf8_length() == 0 ) {
    lid = SymbolTable::lookup("", 0, CHECK_NULL);
  }

  DSUClassLoader * test = find_class_loader_by_id(id);
  if (test != NULL) {
    return test;
  }

  DSUClassLoader* result = new DSUClassLoader();
  result->set_dsu(this);
  result->set_id(id);
  result->set_lid(lid);
  // add the new allocated DSUClassLoader to this DSU
  add_class_loader(result);

  return result;
}


DSU::~DSU() {
  DSUClassLoader* p = _first_class_loader;
  while (p != NULL) {
    DSUClassLoader * q = p;
    p = p->next();
    delete q;
  }
  _first_class_loader = _last_class_loader = NULL;


  if (_shared_stream_provider != NULL) {
    assert(_shared_stream_provider->is_shared(), "sanity check");
    delete _shared_stream_provider;
  }

  free_classes_to_relink();
}


// Iterate all classes in this class.
void DSU::classes_do(void f(DSUClass * dsu_class, TRAPS), TRAPS) {
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    for(DSUClass *dsu_class = dsu_loader->first_class();dsu_class!=NULL;dsu_class=dsu_class->next()) {
      f(dsu_class, CHECK);
    }
  }
}


GrowableArray<DSUClass>*  DSU::_classes_to_relink = NULL;

void DSU::collect_classes_to_relink(TRAPS) {
  {
    MutexLocker sd_mutex(SystemDictionary_lock);
    if (_classes_to_relink == NULL) {
      _classes_to_relink = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<DSUClass>(20, true);
    }
    SystemDictionary::classes_do(check_and_append_relink_class, CHECK);
  }

  const int length = _classes_to_relink->length();


  for(int i=0; i<length; i++) {
    DSUClass* c = _classes_to_relink->adr_at(i);
    InstanceKlass* kh = c->old_version_class();
    assert(kh->is_klass(), "kh must be a klass");
    c->set_old_version(kh);
    c->set_name(kh->name());
  }

  if (DSU_TRACE_ENABLED(0x00000004)) {
    ResourceMark rm(THREAD);
    DSU_TRACE_MESG(("Find %d relink classes", length));
    for(int i=0; i<length; i++) {
      DSUClass* c = _classes_to_relink->adr_at(i);
      DSU_TRACE_MESG(("%d\t %s", i, c->name()->as_C_string()));
    }
  }
}

void DSU::check_and_append_relink_class(Klass* k, TRAPS) {
  if (k->oop_is_instance()) {
    //HandleMark hm(THREAD);
    InstanceKlass* ikh = InstanceKlass::cast(k);

    // only fix user defined class
    if (ikh->class_loader() == NULL) {
      return;
    }

    if (ikh->dsu_will_be_updated()) {
      // we will handle it, skip
      return;
    }

    bool should_relink = false;

    {
      HandleMark hm(THREAD); // used to clear aikh below
      constantPoolHandle cp(ikh->constants());

      int index = 1;
      for (index = 1; index < cp->length(); index++) {
        // Index 0 is unused
        jbyte tag = cp->tag_at(index).value();
        switch (tag) {
          case JVM_CONSTANT_Class : {
              Klass* entry = cp->klass_at(index, THREAD);//*(constants->obj_at_addr(index));
              if (entry->is_klass() && entry->oop_is_instance()) {
                // We only consider resolved class,
                // For class unresolved here and resolved before updating, we will implement a incremental approach.
                // TODO Do we can only unresolved dead class here?
                InstanceKlass* aikh = InstanceKlass::cast(entry);
                if (aikh->dsu_will_be_swapped() || aikh->dsu_will_be_redefined()) {
                  should_relink = true;
                }
              }
              break;
            }
          case JVM_CONSTANT_Long :
          case JVM_CONSTANT_Double :
            index++;
            break;
          default:
            break;
        } // end of switch

        if (should_relink) {
          break;
        }
      }

      if (!should_relink) {
        ConstantPoolCache* cache = cp->cache();

        if (cache != NULL) {
          for (int i = 0; i < cache->length(); i++) {
            ConstantPoolCacheEntry* e = cache->entry_at(i);
            if (e->is_vfinal()) {
              Method* m = e->f2_as_vfinal_method();
              assert(m != NULL, "sanity check");
              if (m->method_holder()->dsu_will_be_swapped()
                  && m->method_holder()->dsu_will_be_redefined()) {
                should_relink = true;
                assert(false, "shit happens");
                break;
              }
            }
          }
        }
      }
    }

    if (should_relink) {
      DSU::append_classes_to_relink(ikh);
      return;
    }
  }
}

void DSU::append_classes_to_relink(InstanceKlass* ikh) {
  // create a stack DSUClass object to pass parameters into append
  DSUClass dsu_class;

  // TODO: why it is BC before
  dsu_class.set_updating_type(DSU_CLASS_MC);
  dsu_class.set_old_version(ikh);

  // does it required?
  ikh->set_dsu_state(DSUState::dsu_will_be_recompiled);

  _classes_to_relink->append(dsu_class);

  // For safe deallocate heap object
  dsu_class.set_old_version_raw(NULL);
}

void DSU::relink_collected_classes(TRAPS) {
  const int length = _classes_to_relink->length();
  //tty->print_cr("length of relink collected classes is %d.",length);
  for(int i = 0; i < length; i++) {
    DSUClass* c = _classes_to_relink->adr_at(i);
    c->relink_class(CHECK);
  }
}

void DSU::free_classes_to_relink() {
  if (_classes_to_relink != NULL) {
    // _classes_to_relink is a GrowableArray,
    // just delete it.
    delete _classes_to_relink;
    _classes_to_relink = NULL;
  }
}

class CollectChangedReflectionClosure: public OopClosure {

public:
  CollectChangedReflectionClosure() {}
  virtual void do_oop(oop* unused) {
    DSU::check_and_append_changed_reflection(unused);
  }
  virtual void do_oop(narrowOop* unused) { ShouldNotReachHere(); }
};

void DSU::collect_changed_reflections(TRAPS) {
  HandleMark hm(THREAD);

  CollectChangedReflectionClosure ccrc;
  int weak_reflection_number_at_prepare = JNIHandles::collect_changed_reflections(&ccrc);
  Javelus::active_dsu()->_weak_reflection_number_at_prepare = weak_reflection_number_at_prepare;
}

void DSU::check_and_append_changed_reflection(oop* reflection) {
  if ((*reflection)->klass() == SystemDictionary::reflect_Field_klass()) {
    check_and_append_changed_reflect_field(reflection);
  } else if ((*reflection)->klass() == SystemDictionary::reflect_Method_klass()) {
    check_and_append_changed_reflect_method(reflection);
  } else if ((*reflection)->klass() == SystemDictionary::reflect_Constructor_klass()) {
    check_and_append_changed_reflect_constructor(reflection);
  }
}

// very coarse checking
void DSU::check_and_append_changed_reflect_field(oop* reflection) {
  Thread* thread = Thread::current();
  oop clazz = java_lang_reflect_Field::clazz(*reflection);
  Klass* klass = java_lang_Class::as_Klass(clazz);

  if (!klass->oop_is_instance()) {
    return;
  }

  if (!is_changed_reflect_field(*reflection)) {
    return;
  }
  //TODO
  InstanceKlass* ikh = InstanceKlass::cast(klass);
  int slot = java_lang_reflect_Field::slot(*reflection);
  fieldDescriptor fd;
  fd.reinitialize(ikh, slot);

  DSU* active_dsu = Javelus::active_dsu();
  Symbol* class_name = ikh->name();
  DSUClass* dsu_class = active_dsu->find_class_by_name(class_name);
  assert(dsu_class != NULL, "sanity check");

  InstanceKlass* new_ikh = dsu_class->new_version_class();
  assert(new_ikh != NULL, "sanity check");

  fieldDescriptor new_fd;
  bool found = new_ikh->find_local_field(fd.name(), fd.signature(), &new_fd);

  if (!found) {
    ResourceMark rm(thread);
    DSU_WARN(("Cannot find field %s %s in new version %s.",
      fd.name()->as_C_string(),
      fd.signature()->as_C_string(),
      class_name->as_C_string()
      ));
    return;
  }

  oop new_reflection = Reflection::new_field(&new_fd, false, false, thread);
  jobject new_handle = JNIHandles::make_global(Handle(new_reflection));

  dsu_class->append_resolved_reflection(fd.name(), fd.signature(), new_handle);
}

void DSU::check_and_append_changed_reflect_method(oop* reflection) {
  Thread* thread = Thread::current();
  oop clazz = java_lang_reflect_Method::clazz(*reflection);
  Klass* klass = java_lang_Class::as_Klass(clazz);

  if (!klass->oop_is_instance()) {
    return;
  }

  if (!is_changed_reflect_method(*reflection)) {
    return;
  }

  //TODO
  InstanceKlass* ikh = InstanceKlass::cast(klass);

  int slot = java_lang_reflect_Method::slot(*reflection);
  methodHandle method (thread, ikh->method_with_idnum(slot));
  assert(method.not_null(), "sanity check");

  DSU* active_dsu = Javelus::active_dsu();
  Symbol* class_name = ikh->name();
  DSUClass* dsu_class = active_dsu->find_class_by_name(class_name);
  assert(dsu_class != NULL, "sanity check");

  InstanceKlass* new_ikh = dsu_class->new_version_class();
  assert(new_ikh != NULL, "sanity check");


  methodHandle new_method (thread, dsu_class->find_new_matched_method(method->name(), method->signature()));
  if (new_method.is_null()) {
    ResourceMark rm(thread);
    DSU_WARN(("Cannot find method %s %s in new version %s.",
      method->name()->as_C_string(),
      method->signature()->as_C_string(),
      class_name->as_C_string()
      ));
    return;
  }

  oop new_reflection = Reflection::new_method(new_method, false, false, false, thread);
  jobject new_handle = JNIHandles::make_global(Handle(new_reflection));

  dsu_class->append_resolved_reflection(method->name(), method->signature(), new_handle);
}



void DSU::check_and_append_changed_reflect_constructor(oop* reflection) {
  Thread* thread = Thread::current();
  oop clazz = java_lang_reflect_Constructor::clazz(*reflection);
  Klass* klass = java_lang_Class::as_Klass(clazz);

  if (!klass->oop_is_instance()) {
    return;
  }


  InstanceKlass* ikh = InstanceKlass::cast(klass);
  int slot = java_lang_reflect_Constructor::slot(*reflection);
  methodHandle method (thread, ikh->method_with_idnum(slot));
  assert(method.not_null(), "sanity check");

  if (!is_changed_reflect_constructor(*reflection)) {
    return;
  }

  DSU* active_dsu = Javelus::active_dsu();
  Symbol* class_name = ikh->name();
  DSUClass* dsu_class = active_dsu->find_class_by_name(class_name);
  assert(dsu_class != NULL, "sanity check");

  InstanceKlass* new_ikh = dsu_class->new_version_class();
  assert(new_ikh != NULL, "sanity check");

  methodHandle new_method (thread,
    //new_ikh->find_method(method->name(), method->signature())
    dsu_class->find_new_matched_method(method->name(), method->signature())
    );
  if (new_method.is_null()) {
    ResourceMark rm(thread);
    DSU_WARN(("Cannot find constructor %s %s in new version %s.",
      method->name()->as_C_string(),
      method->signature()->as_C_string(),
      class_name->as_C_string()
      ));
    return;
  }

  oop new_reflection = Reflection::new_constructor(new_method, false, thread);
  jobject new_handle = JNIHandles::make_global(Handle(thread, new_reflection));

  dsu_class->append_resolved_reflection(method->name(), method->signature(), new_handle);
}

bool DSU::is_changed_reflect_field(oop reflection) {
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Field::clazz(reflection));

  //TODO
  InstanceKlass* ik = InstanceKlass::cast(klass);
  if (ik->dsu_will_be_redefined()) {
    return true;
  } else if (ik->dsu_will_be_deleted()) {
    ShouldNotReachHere();
  } else if (ik->dsu_will_be_swapped()) {
    return true;
  }
  return false;
}

bool DSU::is_changed_reflect_method(oop reflection) {
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Method::clazz(reflection));

  //TODO
  InstanceKlass* ik = InstanceKlass::cast(klass);
  if (ik->dsu_will_be_redefined()) {
    return true;
  } else if (ik->dsu_will_be_deleted()) {
    ShouldNotReachHere();
  } else if (ik->dsu_will_be_swapped()) {
    return true;
  }
  return false;
}

bool DSU::is_changed_reflect_constructor(oop reflection) {
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Constructor::clazz(reflection));

  //TODO
  InstanceKlass* ik = InstanceKlass::cast(klass);
  if (ik->dsu_will_be_redefined()) {
    return true;
  } else if (ik->dsu_will_be_deleted()) {
    ShouldNotReachHere();
  } else if (ik->dsu_will_be_swapped()) {
    return true;
  }
  return false;
}

class UpdateReflectionClosure : public ObjectClosure {
private:
  int                 _count;
public:
  UpdateReflectionClosure() : _count(0) {};
  int count() const {return _count;}
  void do_object(oop obj) {
    // Every object is in its consistent state.
    if (obj->klass() == SystemDictionary::reflect_Field_klass()) {
      if (DSU::is_changed_reflect_field(obj)) {
        DSU::update_changed_reflect_field(obj);
      }
    } else if (obj->klass() == SystemDictionary::reflect_Method_klass()) {
      if (DSU::is_changed_reflect_method(obj)) {
        DSU::update_changed_reflect_method(obj);
      }
    } else if (obj->klass() == SystemDictionary::reflect_Constructor_klass()) {
      if (DSU::is_changed_reflect_constructor(obj)) {
        DSU::update_changed_reflect_constructor(obj);
      }
    }
  }
};

void DSU::update_changed_reflect_field(oop old_reflection) {
  DSU* dsu = Javelus::active_dsu();
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Field::clazz(old_reflection));
  DSUClass* dsu_class = dsu->find_class_by_name(klass->name());
  assert(dsu_class != NULL, "sanity check");

  // TODO
  int slot = java_lang_reflect_Field::slot(old_reflection);
  fieldDescriptor fd;
  fd.reinitialize(InstanceKlass::cast(klass), slot);
  oop new_reflection = dsu_class->find_resolved_reflection(fd.name(), fd.signature());
  if (old_reflection == new_reflection) {
    ShouldNotReachHere();
    return;
  }

  assert(new_reflection != NULL, "should not be null");
  update_changed_reflect_field(old_reflection, new_reflection);
}

void DSU::update_changed_reflect_method(oop old_reflection) {
  DSU* dsu = Javelus::active_dsu();
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Method::clazz(old_reflection));
  DSUClass* dsu_class = dsu->find_class_by_name(klass->name());
  assert(dsu_class != NULL, "sanity check");

  int slot = java_lang_reflect_Method::slot(old_reflection);
  Method* old_method = InstanceKlass::cast(klass)->method_with_idnum(slot);
  oop new_reflection = dsu_class->find_resolved_reflection(old_method->name(), old_method->signature());
  if (old_reflection == new_reflection) {
    ShouldNotReachHere();
    return;
  }

  assert(new_reflection != NULL, "should not be null");
  update_changed_reflect_method(old_reflection, new_reflection);
}

void DSU::update_changed_reflect_constructor(oop old_reflection) {
  DSU* dsu = Javelus::active_dsu();
  Klass* klass = java_lang_Class::as_Klass(java_lang_reflect_Constructor::clazz(old_reflection));
  DSUClass* dsu_class = dsu->find_class_by_name(klass->name());
  assert(dsu_class != NULL, "sanity check");

  int slot = java_lang_reflect_Method::slot(old_reflection);
  Method* old_method = InstanceKlass::cast(klass)->method_with_idnum(slot);
  oop new_reflection = dsu_class->find_resolved_reflection(old_method->name(), old_method->signature());

  if (old_reflection == new_reflection) {
    ShouldNotReachHere();
    return;
  }

  assert(new_reflection != NULL, "should not be null");
  update_changed_reflect_constructor(old_reflection, new_reflection);
}

void DSU::update_changed_reflect_field(oop old_reflection, oop new_reflection) {
  InstanceKlass* ik = InstanceKlass::cast(old_reflection->klass());

  oop root = java_lang_reflect_Field::root(old_reflection);
  // copy contents from new to old
  while (ik != NULL) {
    Javelus::copy_fields(new_reflection, old_reflection, ik);
    ik = ik->superklass();
  }
  java_lang_reflect_Field::set_root(old_reflection, root);
#if 0
  //oop clazz = java_lang_reflect_Field::clazz(new_reflection);
  //java_lang_reflect_Field::set_clazz(old_reflection, clazz);

  int slot = java_lang_reflect_Field::slot(new_reflection);
  java_lang_reflect_Field::set_slot(old_reflection, slot);

  if (java_lang_reflect_Field::has_annotations_field()) {
    oop annotations = java_lang_reflect_Field::annotations(new_reflection);
    java_lang_reflect_Field::set_annotations(old_reflection, annotations);
  }
  //if (java_lang_reflect_Field::has_parameter_annotations_field()) {
  //  oop parameter_annotations = java_lang_reflect_Field::parameter_annotations(new_reflection);
  //  java_lang_reflect_Field::set_parameter_annotations(old_reflection, parameter_annotations);
  //}
  if (java_lang_reflect_Field::has_type_annotations_field()) {
    oop type_annotations = java_lang_reflect_Field::type_annotations(new_reflection);
    java_lang_reflect_Field::set_type_annotations(old_reflection, type_annotations);
  }
#endif
}

void DSU::update_changed_reflect_method(oop old_reflection, oop new_reflection) {
  InstanceKlass* ik = InstanceKlass::cast(old_reflection->klass());

  oop root = java_lang_reflect_Method::root(old_reflection);
  // copy contents from new to old
  while (ik != NULL) {
    Javelus::copy_fields(new_reflection, old_reflection, ik);
    ik = ik->superklass();
  }
  java_lang_reflect_Method::set_root(old_reflection, root);
#if 0
  //
  int slot = java_lang_reflect_Method::slot(new_reflection);
  java_lang_reflect_Method::set_slot(old_reflection, slot);

  //oop clazz = java_lang_reflect_Method::clazz(new_reflection);
  //java_lang_reflect_Method::set_clazz(old_reflection, clazz);

  oop name = java_lang_reflect_Method::name(new_reflection);
  java_lang_reflect_Method::set_name(old_reflection, name);

  oop return_type = java_lang_reflect_Method::return_type(new_reflection);
  java_lang_reflect_Method::set_return_type(old_reflection, return_type);

  oop parameter_types = java_lang_reflect_Method::parameter_types(new_reflection);
  java_lang_reflect_Method::set_parameter_types(old_reflection, parameter_types);

  oop exception_types = java_lang_reflect_Method::exception_types(new_reflection);
  java_lang_reflect_Method::set_exception_types(old_reflection, exception_types);

  int modifiers = java_lang_reflect_Method::modifiers(new_reflection);
  java_lang_reflect_Method::set_modifiers(old_reflection, modifiers);

  jboolean override_value = java_lang_reflect_Method::override(new_reflection);
  java_lang_reflect_Method::set_override(old_reflection, override_value);

  if (java_lang_reflect_Method::has_annotations_field()) {
    oop annotations = java_lang_reflect_Method::annotations(new_reflection);
    java_lang_reflect_Method::set_annotations(old_reflection, annotations);
  }
  if (java_lang_reflect_Method::has_parameter_annotations_field()) {
    oop parameter_annotations = java_lang_reflect_Method::parameter_annotations(new_reflection);
    java_lang_reflect_Method::set_parameter_annotations(old_reflection, parameter_annotations);
  }
  if (java_lang_reflect_Method::has_annotation_default_field()) {
    oop annotation_default = java_lang_reflect_Method::annotation_default(new_reflection);
    java_lang_reflect_Method::set_annotation_default(old_reflection, annotation_default);
  }
  if (java_lang_reflect_Method::has_annotation_default_field()) {
    oop annotation_default = java_lang_reflect_Method::annotation_default(new_reflection);
    java_lang_reflect_Method::set_annotation_default(old_reflection, annotation_default);
  }
  if (java_lang_reflect_Method::has_type_annotations_field()) {
    oop type_annotations = java_lang_reflect_Method::type_annotations(new_reflection);
    java_lang_reflect_Method::set_type_annotations(old_reflection, type_annotations);
  }
#endif
}

void DSU::update_changed_reflect_constructor(oop old_reflection, oop new_reflection) {
  InstanceKlass* ik = InstanceKlass::cast(old_reflection->klass());

  oop root = java_lang_reflect_Constructor::root(old_reflection);
  // copy contents from new to old
  while (ik != NULL) {
    Javelus::copy_fields(new_reflection, old_reflection, ik);
    ik = ik->superklass();
  }
  java_lang_reflect_Constructor::set_root(old_reflection, root);
#if 0
  int slot = java_lang_reflect_Constructor::slot(new_reflection);
  java_lang_reflect_Constructor::set_slot(old_reflection, slot);

  //oop clazz = java_lang_reflect_Constructor::clazz(new_reflection);
  //java_lang_reflect_Constructor::set_clazz(old_reflection, clazz);

  oop parameter_types = java_lang_reflect_Constructor::parameter_types(new_reflection);
  java_lang_reflect_Constructor::set_parameter_types(old_reflection, parameter_types);

  oop exception_types = java_lang_reflect_Constructor::exception_types(new_reflection);
  java_lang_reflect_Constructor::set_exception_types(old_reflection, exception_types);

  int modifiers = java_lang_reflect_Constructor::modifiers(new_reflection);
  java_lang_reflect_Constructor::set_modifiers(old_reflection, modifiers);

  jboolean override_value = java_lang_reflect_Constructor::override(new_reflection);
  java_lang_reflect_Constructor::set_override(old_reflection, override_value);

  if (java_lang_reflect_Constructor::has_annotations_field()) {
    oop annotations = java_lang_reflect_Constructor::annotations(new_reflection);
    java_lang_reflect_Constructor::set_annotations(old_reflection, annotations);
  }
  if (java_lang_reflect_Constructor::has_parameter_annotations_field()) {
    oop parameter_annotations = java_lang_reflect_Constructor::parameter_annotations(new_reflection);
    java_lang_reflect_Constructor::set_parameter_annotations(old_reflection, parameter_annotations);
  }
  if (java_lang_reflect_Constructor::has_type_annotations_field()) {
    oop type_annotations = java_lang_reflect_Constructor::type_annotations(new_reflection);
    java_lang_reflect_Constructor::set_type_annotations(old_reflection, type_annotations);
  }
#endif
}

// XXX This should be called before updating class;
void DSU::update_changed_reflection(TRAPS) {
  // Heap scan to update all copied reflection instance.
  {
    HandleMark hm(THREAD);

    // Ensure that the heap is parsable
    Universe::heap()->ensure_parsability(false);  // no need to retire TALBs

    // Iterate over objects in the heap
    UpdateReflectionClosure dic;

    Universe::heap()->object_iterate(&dic);
  }
}

void DSU::flush_dependent_code(TRAPS) {
  CodeCache::mark_all_nmethods_for_deoptimization();

  ResourceMark rm(THREAD);
  DeoptimizationMarker dm;

  // Deoptimize all activations depending on marked nmethods
  Deoptimization::deoptimize_dependents();

  // Make the dependent methods not entrant (in VM_Deoptimize they are made zombies)
  CodeCache::make_marked_nmethods_not_entrant();
}


DSUClassLoader * DSU::find_class_loader_by_id(Symbol* id) {
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    if (dsu_loader->id() == id) {
      return dsu_loader;
    }
  }
  return NULL;
}

DSUClassLoader * DSU::find_class_loader_by_loader(Handle loader) {
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    if (dsu_loader->classloader() == loader()) {
      return dsu_loader;
    }
  }
  return NULL;
}

// used by jvmti
DSUClassLoader *DSU::find_or_create_class_loader_by_loader(Handle loader, TRAPS) {
  DSUClassLoader *dsu_loader = find_class_loader_by_loader(loader);
  if (dsu_loader == NULL) {
    Symbol* id;
    Symbol* lid;
    if (loader() == SystemDictionary::java_system_loader()) {
      id = SymbolTable::lookup("", 0, CHECK_NULL);
    } else {
      // Should we have to create a DSUClassLoader first?
      stringStream st;
      st.print("%d", (int)os::random());
      char* cstr = st.as_string();
      id = SymbolTable::lookup(cstr, (int)strlen(cstr), CHECK_NULL);
      // TODO
      KlassHandle loader_klass (THREAD, loader->klass());
      Handle loader_of_loader (THREAD, loader_klass->class_loader());
      DSUClassLoader *dsu_loader_of_loader = find_or_create_class_loader_by_loader(loader_of_loader, CHECK_NULL);
      lid = dsu_loader_of_loader->lid();
    }

    dsu_loader = allocate_class_loader(id, lid, CHECK_NULL);
  }
  return dsu_loader;
}

DSUClass* DSU::find_class_by_name(Symbol* name) {
  for (DSUClassLoader* dsu_loader = first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    DSUClass* dsu_class = dsu_loader->find_class_by_name(name);
    if (dsu_class != NULL) {
      return dsu_class;
    }
  }
  return NULL;
}

DSUClass* DSU::find_class_by_name_and_loader(Symbol* name, Handle loader) {
  DSUClassLoader* dsu_loader = find_class_loader_by_loader(loader);
  if (dsu_loader != NULL) {
    return dsu_loader->find_class_by_name(name);
  }
  return NULL;
}

int compare_InstanceKlass(InstanceKlass* *left, InstanceKlass* *right) {
  return 0;
}

// Versioned Class Hierarchy
// VCH is used to ensure type safe in the lazy updating of Javelus.
// So, we have no need to handle swapped class.
// We simply treat them as unchanged classes.
void mark_single_class(DSUClass * dsu_class, TRAPS) {
  DSUClassUpdatingType type = dsu_class->updating_type();
  switch(type) {
    case DSU_CLASS_MC:
    case DSU_CLASS_BC:
      return;
    case DSU_CLASS_SMETHOD:
    case DSU_CLASS_SFIELD:
    case DSU_CLASS_SBOTH:
    case DSU_CLASS_METHOD:
    case DSU_CLASS_FIELD:
    case DSU_CLASS_BOTH:
     break;
    case DSU_CLASS_DEL:
    case DSU_CLASS_NONE:
    case DSU_CLASS_UNKNOWN:
      return;
    default:
      ShouldNotReachHere();
  }

  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  InstanceKlass* old_version = dsu_class->old_version_class();
  InstanceKlass* new_version = dsu_class->new_version_class();

  assert(old_version != NULL, "old version should not be null.");
  assert(new_version != NULL, "new version should not be null.");

  GrowableArray<InstanceKlass*>* no_beta_super_of_new_version = new GrowableArray<InstanceKlass*>(10);
  GrowableArray<InstanceKlass*>* all_super_of_new_version = new GrowableArray<InstanceKlass*>(10);

  DSU::collect_all_super_classes(no_beta_super_of_new_version, new_version, true, false, CHECK);
  DSU::collect_all_super_classes(all_super_of_new_version, new_version, true, true, CHECK);

  //TODO no sort, the set would be very small.
  no_beta_super_of_new_version->sort(&compare_InstanceKlass);
  all_super_of_new_version->sort(&compare_InstanceKlass);

  // Here is a simple implementation,
  // we may set flags for a single class many times.
  int all_length = all_super_of_new_version->length();
  int no_beta_length = no_beta_super_of_new_version->length();

  assert(all_length > no_beta_length, "sanity check");

  int all_index = 0;
  int no_beta_index = 0;

  assert(!new_version->is_type_narrowed_class(), "we have not set this flag");

  while(true) {
    if (no_beta_index >= no_beta_length)
      break;

    assert(no_beta_index <= all_index, "sanity check");

    InstanceKlass* no_beta_ikh = all_super_of_new_version->at(no_beta_index);

    while(all_index <= all_length) {
      InstanceKlass* all_ikh = all_super_of_new_version->at(all_index);
      int ret = compare_InstanceKlass(&no_beta_ikh, &all_ikh);
      if (ret == 0) {
        // all_ikh exists in all and no beta
        all_index++;
        break;
      } else if (ret < 0) {
        all_index++;
        // all_ikh only exists in all
        // may be a type narrowing relevant class if it is not stale.
        if (!all_ikh->is_stale_class()) {
          // TODO, we assume that stale class cannot be executed,
          // i.e., no old code can be executed.
          //if (no_beta_super_of_new_version->contains(all_ikh)) {
            // ikh is only reachable by following beta edge.
            all_ikh->set_is_type_narrowing_relevant_type();
            // TODO we should to set flags for type narrowing checking.
            new_version->set_is_type_narrowed_class();
          //}
        }
      } else {
        break;
      }
    }
    no_beta_index++;
  }

}

void DSU::mark_versioned_class_hierarchy(TRAPS) {
  this->classes_do(&mark_single_class, CHECK);
}

// Collect all super types of the klass.
// in this method.
void DSU::collect_all_super_classes(GrowableArray<InstanceKlass*>* results,
    InstanceKlass* klass, bool include_alpha, bool include_beta, TRAPS) {
  // NO Handle Marks

  int queue_head = results->length();
  results->append(klass);

  while(queue_head < results->length()) {
    InstanceKlass* current_klass = results->at(queue_head);
    queue_head ++;
    InstanceKlass* super_klass = current_klass->superklass();
    if (super_klass != NULL) {
      // versioned class hierarchy is a graph
      if (!results->contains(super_klass)) {
        results->append(super_klass);
      }
    }

    if (include_alpha) {
      super_klass = current_klass->next_version();
      if (super_klass != NULL) {
        if (!results->contains(super_klass)) {
          results->append(super_klass);
        }
      }
    }
    if (include_beta) {
      super_klass = current_klass->previous_version();
      if (super_klass != NULL) {
        if (!results->contains(super_klass)) {
          results->append(super_klass);
        }
      }
    }
  }
}

// -------------------- DSUStreamProvider -----------------------

DSUStreamProvider::DSUStreamProvider() {}

DSUStreamProvider::~DSUStreamProvider() {}

// -------------------- DSUPathEntryStreamProvider -----------------------

DSUPathEntryStreamProvider::DSUPathEntryStreamProvider()
: _first_entry(NULL), _last_entry(NULL) {}

DSUPathEntryStreamProvider::~DSUPathEntryStreamProvider()
{
  // to be added
  //tty->print_cr("Deallocate DSUPathEntryStreamProvider");
}

void DSUPathEntryStreamProvider::append_path(char* path, TRAPS) {
  struct stat st;
  if (os::stat((const char *)path, &st) == 0) {
    // File or directory found
    ClassPathEntry* new_entry = NULL;
    new_entry = ClassLoader::create_class_path_entry((const char*)path, &st, false, false, CHECK);
    add_entry(new_entry);
  }
}

void DSUPathEntryStreamProvider::add_entry(ClassPathEntry* entry) {
  if (_first_entry == NULL) {
    assert(_last_entry == NULL, "sanity check");
    entry->set_next(NULL);
    _first_entry = _last_entry = entry;
    return;
  }

  entry->set_next(NULL);
  _last_entry->set_next(entry);
  _last_entry = _last_entry->next();
}

ClassFileStream* DSUPathEntryStreamProvider::open_stream(const char* name, TRAPS) {
  ClassFileStream* stream = NULL;
  {
    ClassPathEntry* e = _first_entry;
    while (e != NULL) {
      stream = e->open_stream(name, CHECK_NULL);
      if (stream != NULL) {
        break;
      }
      e = e->next();
    }
  }
  return stream;
}


// -------------------------- DSUBuilder ----------------------------

DSUBuilder::DSUBuilder():
_dsu(NULL),
_succ(false)
{

}

DSUBuilder::~DSUBuilder() {
  // to be added
  if (!_succ && _dsu != NULL) {
    delete _dsu;
  }
}

DSU* DSUBuilder::dsu() const {
  assert(_dsu != NULL, "instantiate before using.");
  return _dsu;
}

bool DSUBuilder::build(TRAPS) {
  // XXX allocate DSU here.
  _dsu = new DSU();

  _succ = build_all(THREAD);

  if (HAS_PENDING_EXCEPTION) {
    _succ = false;
  }

  return _succ;
}

// --------------------------------- DSUDynamicPatchBuilder ----------

const char * DSUDynamicPatchBuilder::command_names [] = {
    "classloader",
    "classpath",
    "addclass",
    "modclass",
    "delclass",
    "transformer",
    "loaderhelper",
    "reflection",
  };

DSUDynamicPatchBuilder::DSUDynamicPatchBuilder(const char * dynamic_patch):
  _dynamic_patch(dynamic_patch),
  _current_class(NULL),
  _current_class_loader(NULL),
  _default_class_loader(NULL)
{
  // we use a shared class path entry here.
  // i.e., all class loaders share the same path
  _shared_stream_provider = new DSUPathEntryStreamProvider();
}

DSUDynamicPatchBuilder::~DSUDynamicPatchBuilder()
{
  // to be added

}

bool DSUDynamicPatchBuilder::build_all(TRAPS) {
  const char * path = _dynamic_patch;

  _succ = true;

  if (path == NULL || path[0] == '\0') {
    DSU_WARN(("build a DSU with empty dynamic patch."));
    _succ = false;
    return false;
  }

  // set the shared stream provider
  dsu()->set_shared_stream_provider(_shared_stream_provider);
  // build the default class loader
  build_default_class_loader(CHECK_false);
  // parsing starts with default class loader
  use_default_class_loader();
  parse_file(path, CHECK_false);
  // post popular subclass caused by indirect changed super classes
  populate_sub_classes(CHECK_false);

  return _succ;
}

// resolve all old version
// register place holder in the DSU dictionary
void DSUDynamicPatchBuilder::populate_sub_classes(TRAPS) {
  DSUClassLoader* dsu_class_loader = dsu()->first_class_loader();
  for(;dsu_class_loader != NULL; dsu_class_loader = dsu_class_loader->next() ) {
    DSUClass* dsu_class = dsu_class_loader->first_class();
    for(; dsu_class != NULL; dsu_class = dsu_class->next()) {
      if (dsu_class->require_old_version()) {
        HandleMark hm(THREAD);
        InstanceKlass* old_version;
        dsu_class->resolve_old_version(old_version, CHECK);
        if (old_version = NULL) {
          populate_sub_classes(dsu_class, old_version, CHECK);
        }
      }
    }
  }
}

// invariant: ik has a DSUClass
void DSUDynamicPatchBuilder::populate_sub_classes(DSUClass* dsu_class, InstanceKlass* ik, TRAPS) {
  HandleMark hm(THREAD);

  assert(dsu_class!= NULL && ik != NULL && ik->is_klass(), "sanity");

  if (ik->subklass() == NULL) {
    return;
  }

  InstanceKlass* subklass = (InstanceKlass*)ik->subklass();
  while(subklass != NULL) {
    Handle loader(THREAD, subklass->class_loader());
    Symbol* class_name = subklass->name();
    DSUClassLoader* sub_dsu_class_loader = dsu()->find_or_create_class_loader_by_loader(loader, THREAD);
    assert(sub_dsu_class_loader != NULL, "sanity check");

    DSUClass* sub_dsu_class = sub_dsu_class_loader->find_class_by_name(class_name);
    if (sub_dsu_class != NULL) {
      // we already has created a DSUClass for this class during parsing.
      // do nothing here.
      //continue;
    } else {
      sub_dsu_class = sub_dsu_class_loader->allocate_class(class_name, CHECK);
      // TODO Here, a indirect updated class just copy the updating type of its super class.
      //sub_dsu_class->set_updating_type(DSU_CLASS_INDIRECT);
      sub_dsu_class->set_updating_type(dsu_class->updating_type());
      populate_sub_classes(sub_dsu_class, subklass, CHECK);
    }
    if (subklass->next_sibling() == NULL) {
      break;
    }
    subklass = (InstanceKlass*)subklass->next_sibling();
  }
}

void DSUDynamicPatchBuilder::parse_file(const char * file, TRAPS) {
  struct stat st;
  if (os::stat(file, &st) == 0) {
    // found file, open it
    int file_handle = os::open(file, 0, 0);
    if (file_handle != -1) {
      // read contents into resource array
      ResourceMark rm(THREAD);
      u1* buffer = NEW_RESOURCE_ARRAY(u1, st.st_size);
      size_t num_read = os::read(file_handle, (char*) buffer, st.st_size);
      // close file
      os::close(file_handle);
      if (num_read == (size_t)st.st_size) {
        char * token = (char*) buffer;
        char * line = token;
        int pos = 0;
        while (num_read != (size_t) pos) {
          if (token[pos] == '\n') {
            token[pos] = '\0';
            DSU_DEBUG(("Parse line: raw %s", line));
            parse_line(line, CHECK);
            if (!_succ) {
              DSU_DEBUG(("Parse line %s failed.", line));
              return;
            }
            line = token + pos + 1;
          }
          pos++;
        }
      } else {
        DSU_WARN(("File size of dynamic patch error!"));
      }
    } else {
      DSU_WARN(("Dynamic patch does not exist!"));
    }
  } else {
    DSU_WARN(("Status Fail! Dynamic patch does not exist!"));
  }
}


void DSUDynamicPatchBuilder::build_default_class_loader(TRAPS) {
  DSUClassLoader* dsu_class_loader = dsu()->allocate_class_loader(NULL, NULL, CHECK);

  dsu_class_loader->set_stream_provider(_shared_stream_provider);
  dsu_class_loader->resolve(CHECK);

  _default_class_loader = dsu_class_loader;
}

DSUDynamicPatchBuilder::DynamicPatchCommand DSUDynamicPatchBuilder::parse_command_name(const char * line, int* bytes_read) {
  *bytes_read = 0;
  char command[33];
  int result = sscanf(line, "%32[a-z]%n", command, bytes_read);
  for (uint i = 0; i < ARRAY_SIZE(command_names); i++) {
    if (strcmp(command, command_names[i]) == 0) {
      return (DynamicPatchCommand)i;
    }
  }
  return UnknownCommand;
}


DSUClassLoader* DSUDynamicPatchBuilder::current_class_loader() {
  assert(_current_class_loader != NULL, "must be set before using");
  return _current_class_loader;
}

DSUClass* DSUDynamicPatchBuilder::current_class() {
  assert(_current_class != NULL, "must be set before using");
  return _current_class;
}

void DSUDynamicPatchBuilder::append_class_path_entry(char * line, TRAPS) {
  _shared_stream_provider->append_path(line, CHECK);
}

// classloader [id] [lid]
void DSUDynamicPatchBuilder::use_class_loader(char *line, TRAPS) {
  char c_id[33];
  char c_lid[33];

  int result = sscanf(line, "%32[a-z] %32[a-z]", c_id, c_lid);

  Symbol* id;
  Symbol* lid;

  if (result > 0) {
    id = SymbolTable::lookup(c_id, (int)strlen(c_id), CHECK);
  }

  if (result > 1) {
    lid = SymbolTable::lookup(c_lid, (int)strlen(c_lid), CHECK);;
  }

  // getOrCreate class loader
  DSUClassLoader* dsu_class_loader = dsu()->allocate_class_loader(id, lid, CHECK);

  assert(dsu_class_loader != NULL, "sanity check");

  // set stream provider
  dsu_class_loader->set_stream_provider(_shared_stream_provider);

  // set current class loader
  _current_class_loader = dsu_class_loader;
}

void DSUDynamicPatchBuilder::append_added_class(char * line, TRAPS) {
  Symbol* class_name = SymbolTable::lookup(line, (int)strlen(line), CHECK);
  DSUClass * dsu_class = current_class_loader()->allocate_class(class_name, CHECK);
  _current_class = dsu_class;

  dsu_class->set_updating_type(DSU_CLASS_ADD);
  dsu_class->set_stream_provider(current_class_loader()->stream_provider());

  // TODO why we need resolve class loader here.
  // I cannot remember
  //dsu_class->dsu_class_loader()->resolve(CHECK);
  //assert(dsu_class->dsu_class_loader()->classloader() != NULL, "sanity check");
}

void DSUDynamicPatchBuilder::append_modified_class(char * line, TRAPS) {
  Symbol* class_name = SymbolTable::lookup(line, (int)strlen(line), CHECK);
  DSUClass * dsu_class = current_class_loader()->allocate_class(class_name, CHECK);
  _current_class = dsu_class;
  dsu_class->set_updating_type(DSU_CLASS_NONE);
  dsu_class->set_stream_provider(current_class_loader()->stream_provider());
}

void DSUDynamicPatchBuilder::append_deleted_class(char * line, TRAPS) {
  Symbol* class_name = SymbolTable::lookup(line, (int)strlen(line), CHECK);
  DSUClass * dsu_class = current_class_loader()->allocate_class(class_name, CHECK);
  _current_class = dsu_class;
  dsu_class->set_updating_type(DSU_CLASS_DEL);
}

void DSUDynamicPatchBuilder::append_transformer(char * line, TRAPS) {
  Symbol* class_name = SymbolTable::lookup(line, (int)strlen(line), CHECK);
  current_class_loader()->resolve_transformer(class_name, CHECK);
  //ShouldNotReachHere();
}

void DSUDynamicPatchBuilder::append_loader_helper(char * line, TRAPS) {
  Symbol* class_name = SymbolTable::lookup(line, (int)strlen(line), CHECK);
  ShouldNotReachHere();
}

// The characters allowed in a class or method name.  All characters > 0x7f
// are allowed in order to handle obfuscated class files (e.g. Volano)
#define DSU_ID "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789$_<>" \
        "\x80\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x8b\x8c\x8d\x8e\x8f" \
        "\x90\x91\x92\x93\x94\x95\x96\x97\x98\x99\x9a\x9b\x9c\x9d\x9e\x9f" \
        "\xa0\xa1\xa2\xa3\xa4\xa5\xa6\xa7\xa8\xa9\xaa\xab\xac\xad\xae\xaf" \
        "\xb0\xb1\xb2\xb3\xb4\xb5\xb6\xb7\xb8\xb9\xba\xbb\xbc\xbd\xbe\xbf" \
        "\xc0\xc1\xc2\xc3\xc4\xc5\xc6\xc7\xc8\xc9\xca\xcb\xcc\xcd\xce\xcf" \
        "\xd0\xd1\xd2\xd3\xd4\xd5\xd6\xd7\xd8\xd9\xda\xdb\xdc\xdd\xde\xdf" \
        "\xe0\xe1\xe2\xe3\xe4\xe5\xe6\xe7\xe8\xe9\xea\xeb\xec\xed\xee\xef" \
        "\xf0\xf1\xf2\xf3\xf4\xf5\xf6\xf7\xf8\xf9\xfa\xfb\xfc\xfd\xfe\xff"

// binary name
#define DSU_BINARY_NAME "[" DSU_ID "/;[]"

#define DSU_SIG "[" DSU_ID "/;[()]"

void DSUDynamicPatchBuilder::append_reflection(char* line, TRAPS) {
  char c_old_name[256];
  char c_new_name[256];
  char c_old_sig[1024];
  char c_new_sig[1024];
  char c_new_class_name[256];

  int result = sscanf(
    line,
    "%255" DSU_BINARY_NAME " " "%1023" DSU_SIG " " "%255" DSU_BINARY_NAME " " "%1023" DSU_SIG " " "%255" DSU_BINARY_NAME ,
    c_old_name,
    c_old_sig,
    c_new_name,
    c_new_sig,
    c_new_class_name);

  if (result != 4
    && result != 5) {
    _succ = false;
    DSU_WARN(("Command reflection format error, %s, result=%d", line, result));
    return;
  }

  Symbol* old_name = SymbolTable::lookup(c_old_name, (int)strlen(c_old_name), CHECK);
  Symbol* old_sig = SymbolTable::lookup(c_old_sig, (int)strlen(c_old_sig), CHECK);
  Symbol* new_name = SymbolTable::lookup(c_new_name, (int)strlen(c_new_name), CHECK);
  Symbol* new_sig = SymbolTable::lookup(c_new_sig, (int)strlen(c_new_sig), CHECK);

  Symbol* new_class_name = NULL;

  if (result == 4) {
    new_class_name = current_class()->name();
  } else if (result == 5) {
    new_class_name = SymbolTable::lookup(c_new_class_name, (int)strlen(c_new_class_name), CHECK);
  } else {
    ShouldNotReachHere();
  }

  current_class()->append_match_reflection(
    old_name,
    old_sig,
    new_name,
    new_sig,
    new_class_name);


}

void DSUDynamicPatchBuilder::use_default_class_loader() {
  _current_class_loader = _default_class_loader;
}

void DSUDynamicPatchBuilder::parse_line(char *line, TRAPS) {
  if (line[0] == '\0') return;
  if (line[0] == '#')  return;
  int bytes_read;

  DSU_DEBUG(("Parse line %s", line));

  DynamicPatchCommand command = parse_command_name(line, &bytes_read);
  line+=bytes_read;

  while(*line == ' ' || *line == '\t') {
    line++;
  }

  switch(command) {
  case UnknownCommand:
    DSU_WARN(("Unknown command during parsing dynamic patch. %s", line));
    return;
  case ClassLoaderCommand:
    use_class_loader(line,CHECK);
    return;
  case ClassPathCommand:
    append_class_path_entry(line, CHECK);
    return;
  case AddClassCommand:
    append_added_class(line, CHECK);
    return;
  case ModClassCommand:
    append_modified_class(line, CHECK);
    return;
  case DelClassCommand:
    append_deleted_class(line, CHECK);
    return;
  case TransformerCommand:
    append_transformer(line, CHECK);
    return;
  case LoaderHelperCommand:
    append_loader_helper(line, CHECK);
    return;
  case ReflectionCommand:
    append_reflection(line, CHECK);
    return;
  }
}


// ----------------- DSUDirectStreamProvider ------------

DSUDirectStreamProvider::DSUDirectStreamProvider(
    jint class_byte_count,
    const unsigned char* class_bytes
    ):
_class_byte_count(class_byte_count),
_class_bytes(class_bytes)
{

}

DSUDirectStreamProvider::~DSUDirectStreamProvider() {

}

ClassFileStream *DSUDirectStreamProvider::open_stream(const char * class_name, TRAPS) {
  return new ClassFileStream((u1*) _class_bytes,
      _class_byte_count, (char *)"__VM_DSUDirectStreamProvider__");
}

// ----------------- DSUJvmtiBuilder ---------------

DSUJvmtiBuilder::DSUJvmtiBuilder(jint class_count, const jvmtiClassDefinition *class_defs)
: _class_count(class_count), _class_defs(class_defs) {}

DSUJvmtiBuilder::~DSUJvmtiBuilder()
{
  // to be added
}

// TODO
// sort of class defs have not been implemented.


bool DSUJvmtiBuilder::build_all(TRAPS) {

  _succ = true;

  // we use the following convention to indicate coarse updating type
  // see definition of jvmtiClassDefinition at jvmti.h
  // klass == NULL: added class
  // class_byte_count == 0: deleted class
  // otherwise: modififed class
  for(int i = 0; i < _class_count; i++) {
    const jvmtiClassDefinition  *def = &_class_defs[i];
    // initial type
    DSUClassUpdatingType type = DSU_CLASS_NONE;
    if (def->klass == NULL) {
      type = DSU_CLASS_ADD;
      build_added_class(type, def, CHECK_false);
    } else if (def->class_byte_count == 0) {
      type = DSU_CLASS_DEL;
      build_deleted_class(type, def, CHECK_false);
    } else {
      build_modified_class(type, def, CHECK_false);
    }

    // return immediately if we have an error
    if (!_succ) {
      return false;
    }
  }

  return _succ;
}

// in fact, jvmti has no need to implement add new classes.
// as during debugging, any new class will be compiled into the
// original directory.a
void DSUJvmtiBuilder::build_added_class(DSUClassUpdatingType type,
    const jvmtiClassDefinition * def, TRAPS) {
  ShouldNotReachHere();
}


void DSUJvmtiBuilder::build_deleted_class(DSUClassUpdatingType type, const jvmtiClassDefinition * def, TRAPS) {

  oop mirror = JNIHandles::resolve_non_null(def->klass);
  InstanceKlass* old_class = InstanceKlass::cast(java_lang_Class::as_Klass(mirror));
  Symbol* class_name = old_class->name();
  Handle class_loader(THREAD, old_class->class_loader());

  DSUClassLoader *dsu_class_loader = dsu()->find_or_create_class_loader_by_loader(class_loader, CHECK);
  DSUClass *dsu_class = dsu_class_loader->allocate_class(class_name, CHECK);

  dsu_class->set_updating_type(type);
  DSUDirectStreamProvider *stream_provider = new DSUDirectStreamProvider(def->class_byte_count, def->class_bytes);
  dsu_class->set_stream_provider(stream_provider);
}


void DSUJvmtiBuilder::build_modified_class(DSUClassUpdatingType type, const jvmtiClassDefinition* def, TRAPS) {
  oop mirror = JNIHandles::resolve_non_null(def->klass);
  InstanceKlass* old_class = InstanceKlass::cast(java_lang_Class::as_Klass(mirror));
  Symbol* class_name = old_class->name();
  Handle class_loader(THREAD, old_class->class_loader());

  DSUClassLoader *dsu_class_loader = dsu()->find_or_create_class_loader_by_loader(class_loader, THREAD);
  DSUClass *dsu_class = dsu_class_loader->allocate_class(class_name, THREAD);

  dsu_class->set_updating_type(type);
  DSUDirectStreamProvider *stream_provider = new DSUDirectStreamProvider(def->class_byte_count, def->class_bytes);
  dsu_class->set_stream_provider(stream_provider);
}

void DSUJvmtiBuilder::build_transformers(TRAPS) {
  for (DSUClassLoader* dsu_loader = dsu()->first_class_loader(); dsu_loader!=NULL; dsu_loader=dsu_loader->next()) {
    dsu_loader->resolve(THREAD);

    if (HAS_PENDING_EXCEPTION) {
      DSU_WARN(("DSUJvmtiBuilder: build transformer failed!"));
      CLEAR_PENDING_EXCEPTION;
      continue;
    }

    ResourceMark rm(THREAD);
    // setup the arguments to getProperty
    Handle key_str   = java_lang_String::create_from_str("org.javelus.transformerClass", CHECK);

    // return value
    JavaValue result(T_OBJECT);

    // public static String getProperty(String key, String def);
    JavaCalls::call_static(&result,
      KlassHandle(THREAD, SystemDictionary::System_klass()),
      vmSymbols::getProperty_name(),
      vmSymbols::string_string_signature(),
      key_str,
      CHECK);

    oop value_oop = (oop)result.get_jobject();
    if (value_oop == NULL) {
      DSU_WARN(("Please set org.javelus.transformerClass=/path/to/transformer/class/.class!"));
      //return;
      continue;
    }

    // convert Java String to utf8 string
    const char* file = java_lang_String::as_utf8_string(value_oop);




    struct stat st;
    if (os::stat(file, &st) == 0) {
      // found file, open it
      int file_handle = os::open(file, 0, 0);
      if (file_handle != -1) {
        // read contents into resource array
        ResourceMark rm(THREAD);
        u1* buffer = NEW_RESOURCE_ARRAY(u1, st.st_size);
        size_t num_read = os::read(file_handle, (char*) buffer, st.st_size);
        // close file
        os::close(file_handle);

        ClassFileStream stream((u1*) buffer, st.st_size, (char *)"__JAVELUS_LOAD_TRANSFORMER_CLASS__");

        // Parse the stream.
        Handle class_loader = Handle(THREAD, dsu_loader->classloader());
        Handle protection_domain(THREAD, NULL);
        // Set redefined class handle in JvmtiThreadState class.
        // This redefined class is sent to agent event handler for class file
        // load hook event.

        Symbol* null_name = NULL;
        Klass* k = SystemDictionary::parse_stream(null_name,
          class_loader,
          protection_domain,
          &stream,
          THREAD);

        if (HAS_PENDING_EXCEPTION) {
          //symbolOop ex_name = PENDING_EXCEPTION->klass()->klass_part()->name();
          DSU_WARN(("Parsing stream for transfromer class failed!"));
          CLEAR_PENDING_EXCEPTION;
          //return;
          continue;
        }

        InstanceKlass* transformer_class = InstanceKlass::cast(k);

        // We just allocate constantPoolCache here and do not initialize vtable and itable.
        Rewriter::rewrite(transformer_class, THREAD);
        if (HAS_PENDING_EXCEPTION) {
          DSU_WARN(("Rewriting transfromer class failed!"));
          CLEAR_PENDING_EXCEPTION;
          continue;
        }

        {
          ResourceMark rm(THREAD);
          // no exception should happen here since we explicitly
          // do not check loader constraints.
          // compare_and_normalize_class_versions has already checked:
          //  - classloaders unchanged, signatures unchanged
          //  - all instanceKlasses for redefined classes reused & contents updated
          transformer_class->vtable()->initialize_vtable(false, THREAD);
          transformer_class->itable()->initialize_itable(false, THREAD);
          assert(!HAS_PENDING_EXCEPTION || (THREAD->pending_exception()->is_a(SystemDictionary::ThreadDeath_klass())), "redefine exception");
        }

        transformer_class->set_init_state(InstanceKlass::linked);

        transformer_class->initialize(THREAD);

        if (HAS_PENDING_EXCEPTION) {
          DSU_WARN(("Initializing transfromer class failed!"));
          CLEAR_PENDING_EXCEPTION;
          continue;
        }

        dsu_loader->set_transformer_class(transformer_class);
      }
    }
  }
}

// ----------------- DSUClassLoader -----------------------

DSUClassLoader::DSUClassLoader()
: _dsu(NULL),
  _next(NULL),
  _id(NULL),
  _lid(NULL),
  _loaded_loader(NULL),
  _stream_provider(NULL),
  _loader_data(NULL),
  _helper_class(NULL),
  _transformer_class(NULL),
  _first_class(NULL),
  _last_class(NULL) {}


bool DSUClassLoader::validate(TRAPS) {
  return false;
}

DSUClassLoader::~DSUClassLoader() {
  DSUClass* p = _first_class;
  while(p != NULL) {
    DSUClass* q = p;
    p = p->next();
    delete q;
  }
  _first_class = NULL;
  _last_class = NULL;
}

DSUError DSUClassLoader::prepare(TRAPS) {
  // rsolve the class loader first
  resolve(CHECK_(DSU_ERROR_RESOLVE_CLASS_LOADER_FAILED));

  // than prepare all classes
  for (DSUClass* dsu_class = first_class(); dsu_class != NULL; dsu_class = dsu_class->next()) {
    DSUError ret = dsu_class->prepare(THREAD);

    if (ret != DSU_ERROR_NONE) {
      return ret;
    }

    if (HAS_PENDING_EXCEPTION) {
      return DSU_ERROR_PREPARE_DSUCLASS;;
    }
  }

  remove_unchanged_classes();

  return DSU_ERROR_NONE;
}

DSUClass *DSUClassLoader::find_class_by_name(Symbol *name) {
  for (DSUClass* dsu_class = first_class(); dsu_class!=NULL; dsu_class=dsu_class->next()) {
    if (dsu_class->name() == name) {
      return dsu_class;
    }
  }
  return NULL;
}

bool DSUClassLoader::resolved() {
  return _loader_data != NULL;
}

void DSUClassLoader::resolve(TRAPS) {
  if (_loader_data != NULL) {
    return;
  }
  assert(id() != NULL, "sanity check");
  if (id()->utf8_length() == 0) {
    // bind to the system class loader;
    ClassLoaderData* result = ClassLoaderData::class_loader_data(SystemDictionary::java_system_loader());
    set_class_loader_data(result);
    return;
  }

  if (this->helper_class() != NULL) {
    // run helper class to resolve these classloader
    assert(false, "to be added");
    return;
  }

  // no helper class
  // we try to guess the class loader
  // XXX:
  // 1). There is a class with name "xxx" that can only be loaded by this class loader;

  HandleMark hm(THREAD);
  ClassLoaderData* result;
  {
    MutexLocker mu(SystemDictionary_lock);
    for (DSUClass* dsu_class = first_class(); dsu_class!=NULL; dsu_class=dsu_class->next()) {
      if (dsu_class->require_old_version()) {
        Symbol* class_name = dsu_class->name();
        ResourceMark rm(THREAD);
        DSU_DEBUG(("Resolve class loader by class [%s]", class_name->as_C_string()));
        Javelus::pick_loader_by_class_name(class_name, result, CHECK);
        if (result != NULL) {
          DSU_DEBUG(("Success to resolve class loader by class %s", class_name->as_C_string()));
          break;
        }
      }
    }
  }

  if (result != NULL) {
    set_class_loader_data(result);
    return;
  }

  assert(false, "We cannot find the class loader");
}

bool DSUClassLoader::resolve_transformer(Symbol* transformer_name, TRAPS) {
  InstanceKlass* transformer = NULL;

  DSU_DEBUG(("Resolve transformer [%s]", transformer_name->as_C_string()));

  // use the stream_provider of the class loader to load transformer.
  DSUError ret = load_new_version(transformer_name, transformer, this->stream_provider(), THREAD);
  if (ret != DSU_ERROR_NONE) {
    DSU_WARN(("Load new version failed"));
    return false;
  }

  if (HAS_PENDING_EXCEPTION) {
    DSU_WARN(("Load new version failed with exception"));
    return false;
  }

  assert(transformer->is_linked(), "sanity check");

  // we should initialize transformer classes before invoke method of it.
  transformer->initialize(CHECK_false);
  transformer->set_is_transformer_class();
  assert(transformer->is_transformer_class(), "sanity check!");
  set_transformer_class(transformer);

  return DSU_ERROR_NONE;
}

DSUClass* DSUClassLoader::allocate_class(Symbol* name, TRAPS) {
  if (name == NULL || name->utf8_length()==0) {
    return NULL;
  }

  DSUClass* test = find_class_by_name(name);

  if (test != NULL) {
    return test;
  }

  DSUClass* dsu_class = new DSUClass();

  dsu_class->set_name(name);
  dsu_class->set_dsu_class_loader(this);

  // XXX set stream provider here.
  dsu_class->set_stream_provider(stream_provider());
  // add class to this class loader
  this->add_class(dsu_class);

  return dsu_class;
}

void DSUStreamProvider::print() {
  tty->print_cr("DSUStream Provider");
}

void DSUPathEntryStreamProvider::print() {
  tty->print_cr("DSUPathEntryStreamProvider Provider");
}


void DSUDirectStreamProvider::print() {
  tty->print_cr("DSUDirectStreamProvider Provider");
}


DSUError  DSUClassLoader::load_new_version(Symbol* name, InstanceKlass* &new_class,
    DSUStreamProvider *stream_provider, TRAPS) {
  ResourceMark rm(THREAD);
  new_class = NULL;

  if (!resolved()) {
    resolve(CHECK_(DSU_ERROR_RESOLVE_NEW_CLASS));
  }

  assert(resolved(), "sanity check");

  Handle the_class_loader (THREAD, classloader());

  // Check the state of the class loader.
  if (the_class_loader.is_null()) {
    // TODO, error code should be adjust to the right one.
    return DSU_ERROR_NULL_POINTER;
  }


  // Null protection domain
  // TODO, this should be consider later.
  Handle protection_domain;

  stringStream st;
  // st.print() uses too much stack space while handling a StackOverflowError
  // st.print("%s.class", h_name->as_utf8());
  st.print_raw(name->as_utf8());
  st.print_raw(".class");
  const char* file_name = st.as_string();

  ClassFileStream* cfs = stream_provider->open_stream(file_name, CHECK_(DSU_ERROR_RESOLVE_NEW_CLASS));

  if (cfs == NULL) {
    return DSU_ERROR_RESOLVE_NEW_CLASS;
  }

  Klass* k = SystemDictionary::parse_stream(name,
    the_class_loader,
    protection_domain,
    cfs,
    THREAD);

  if (HAS_PENDING_EXCEPTION) {
    Symbol* ex_name = PENDING_EXCEPTION->klass()->name();
    CLEAR_PENDING_EXCEPTION;

    if (ex_name == vmSymbols::java_lang_UnsupportedClassVersionError()) {
      return DSU_ERROR_UNSUPPORTED_VERSION;
    } else if (ex_name == vmSymbols::java_lang_ClassFormatError()) {
      return DSU_ERROR_INVALID_CLASS_FORMAT;
    } else if (ex_name == vmSymbols::java_lang_ClassCircularityError()) {
      return DSU_ERROR_CIRCULAR_CLASS_DEFINITION;
    } else if (ex_name == vmSymbols::java_lang_NoClassDefFoundError()) {
      // The message will be "XXX (wrong name: YYY)"
      return DSU_ERROR_NAMES_DONT_MATCH;
    } else if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
      return DSU_ERROR_OUT_OF_MEMORY;
    } else {  // Just in case more exceptions can be thrown..
      DSU_TRACE_MESG(("Fails verification, exception name is %s, class name is %s." ,
        ex_name->as_C_string(), name->as_C_string()));
      return DSU_ERROR_FAILS_VERIFICATION;
    }
  }

  InstanceKlass* new_super = InstanceKlass::cast(k->super());
  ClassLoaderData* loader_data = new_super->class_loader_data();
  Symbol* new_super_name = new_super->name();

  assert(new_super->is_linked(), "sanity check");

  // Check DSUClass again
  DSUClassLoader* super_dsu_class_loader = dsu()->find_class_loader_by_loader(loader_data->class_loader());
  if (super_dsu_class_loader != NULL) {
    DSUClass* super_dsu_class = super_dsu_class_loader->find_class_by_name(new_super_name);
    if (super_dsu_class == NULL) {  // a class not in dynamic patch
      if (!new_super->is_linked()) {
        return DSU_ERROR_TO_BE_ADDED;
      }
    } else if (!super_dsu_class->require_old_version()) { // another added class in dynamic patch
      if (!new_super->is_linked()) {
        return DSU_ERROR_TO_BE_ADDED;
      }
    } else { // a redefined class in dynamic patch
      if (!new_super->is_linked()) {
        return DSU_ERROR_TO_BE_ADDED;
      }
    }
  } else {
    // a class not in dynamic patch
    if (!new_super->is_linked()) {
      return DSU_ERROR_TO_BE_ADDED;
    }
  }

  assert(new_super->is_linked(), "sanity check");

  new_class = InstanceKlass::cast(k);

  // We just allocate constantPoolCache here and do not initialize vtable and itable.
  Rewriter::rewrite(new_class, THREAD);
  if (HAS_PENDING_EXCEPTION) {
    Symbol* ex_name = PENDING_EXCEPTION->klass()->name();
    CLEAR_PENDING_EXCEPTION;
    if (ex_name == vmSymbols::java_lang_OutOfMemoryError()) {
      return DSU_ERROR_OUT_OF_MEMORY;
    } else {
      return DSU_ERROR_REWRITER;
    }
  }

  // need link methods
  new_class->link_methods(CHECK_(DSU_ERROR_REWRITER));

  assert(new_super->is_linked(), "sanity check");
  {
    ResourceMark rm(THREAD);
    // no exception should happen here since we explicitly
    // do not check loader constraints.
    // compare_and_normalize_class_versions has already checked:
    //  - classloaders unchanged, signatures unchanged
    //  - all instanceKlasses for redefined classes reused & contents updated
    new_class->vtable()->initialize_vtable(false, THREAD);
    new_class->itable()->initialize_itable(false, THREAD);
    assert(!HAS_PENDING_EXCEPTION || (THREAD->pending_exception()->is_a(SystemDictionary::ThreadDeath_klass())), "redefine exception");
#ifdef ASSERT
    new_class->vtable()->verify(tty);
#endif
    if (HAS_PENDING_EXCEPTION) {
      assert(false, "should not have exception");
    }
  }

  new_class->set_init_state(InstanceKlass::linked);
  return DSU_ERROR_NONE;
}


void DSUClassLoader::add_class(DSUClass * klass) {
  assert( klass->next() == NULL, "should not in any other list.");
  if (_first_class == NULL) {
    assert(_last_class == NULL, "sanity check");
    _first_class = _last_class = klass;
  } else {
    assert(_last_class != NULL, "sanity check");
    _last_class->set_next(klass);
    _last_class = klass;
  }
}

void DSUClassLoader::remove_unchanged_classes() {
  if (_first_class == NULL) {
    return;
  } else if (_first_class == _last_class) {
    // remove the first one
    if (_first_class->prepared() && _first_class->updating_type() == DSU_CLASS_NONE) {
      delete _first_class;
      _first_class = _last_class = NULL;
    }
  } else {
    DSUClass *p = _first_class->next();
    DSUClass *q = _first_class;
    // p is next
    // q is previous
    while (p != NULL) {
      if (p->prepared() && p->updating_type() == DSU_CLASS_NONE) {
        q->set_next(p->next());
        if (_last_class == p) {
          _last_class = q;
        }
        delete p;
        p = q->next();
      } else {
        q = p;
        p = p->next();
      }
    }
    q = _first_class;
    if (q->prepared() && q->updating_type() == DSU_CLASS_NONE) {
      _first_class = q->next();
      delete q;
      if (_first_class == NULL) {
        _last_class = NULL;
      }
    }
  }
}

// ---------------------- DSUClass ------------------------------

DSUClass::DSUClass()
: _next(NULL),
  _dsu_class_loader(NULL),
  _updating_type(DSU_CLASS_UNKNOWN),
  _prepared(false),
  _stream_provider(NULL),
  _name(NULL),
  _old_version(NULL),
  _new_version(NULL),
  _new_inplace_new_class(NULL),
  _stale_new_class(NULL),
  _object_transformer_method(NULL),
  _object_transformer_args(NULL),
  _class_transformer_method(NULL),
  _class_transformer_args(NULL),
  _first_method(NULL),
  _last_method(NULL),
  _old_ycsc(NULL),
  _new_ycsc(NULL),
  _min_object_size_in_bytes(0),
  _min_vtable_size(0),
  _match_static_count(0),
  _match_static_fields(NULL),
  _match_reflections(NULL),
  _resolved_reflections_name_and_sig(NULL),
  _resolved_reflections(NULL) {}

void DSUClass::print() {

}


void DSUClass::append_match_reflection(Symbol* old_name, Symbol* old_sig,
    Symbol* new_name, Symbol* new_sig, Symbol* new_class_name) {
  if (_match_reflections == NULL) {
    _match_reflections = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<Symbol*>(25, true);
  }

  _match_reflections->append(old_name);
  _match_reflections->append(old_sig);
  _match_reflections->append(new_name);
  _match_reflections->append(new_sig);
  _match_reflections->append(new_class_name);
}

void DSUClass::append_resolved_reflection(Symbol* name, Symbol* sig, jobject reflection) {
  if (_resolved_reflections == NULL) {
    _resolved_reflections = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<jobject>(15, true);
    _resolved_reflections_name_and_sig = new (ResourceObj::C_HEAP, mtInternal) GrowableArray<Symbol*>(15, true);
  }

  if (find_resolved_reflection(name, sig) != NULL) {
    return;
  }

  _resolved_reflections_name_and_sig->append(name);
  _resolved_reflections_name_and_sig->append(sig);
  _resolved_reflections->append(reflection);
}

oop DSUClass::find_resolved_reflection(Symbol* old_name, Symbol* old_sig) {
  assert(_resolved_reflections != NULL, "should be calculated before VM safe point.");

  int length = _resolved_reflections->length();
  for(int i=0, j=0; j < length; j++, i+=2) {
    Symbol* name = _resolved_reflections_name_and_sig->at(i);
    Symbol* sig  = _resolved_reflections_name_and_sig->at(i + 1);
    if (name == old_name && sig == old_sig) {
      return JNIHandles::resolve(_resolved_reflections->at(j));
    }
  }

  return NULL;
}

Method* DSUClass::find_new_matched_method(Symbol* name, Symbol* signature) {
  if (_match_reflections == NULL) {
    // no match information,
    // return immediately
    return new_version_class()->find_method(name, signature);
  }

  assert(this->old_version_resolved(), "sanity check");
  assert(this->new_version_resolved(), "sanity check");

  int length = _match_reflections->length();
  assert(length % 5 == 0, "sanity check");
  // resolve
  if ( length > 0) {
    Symbol* old_class_name = this->name();
    for(int i=0; i<length; i+=5) {
      Symbol* old_name = _match_reflections->at(i);
      Symbol* old_sig  = _match_reflections->at(i + 1);
      Symbol* new_name = _match_reflections->at(i + 2);
      Symbol* new_sig  = _match_reflections->at(i + 3);
      Symbol* new_class_name = _match_reflections->at(i + 4);

      if (old_class_name != new_class_name) {
        ShouldNotReachHere();
      }

      if (old_name != name || old_sig != signature) {
        continue;
      }

      Method* result = new_version_class()->find_method(new_name, new_sig);
      assert(result != NULL, "sanity check");
      return result;
    }
  }

  // not in match information
  // return
  return new_version_class()->find_method(name, signature);
}

bool DSUClass::validate(TRAPS) {
  return false;
}

int DSUClass::from_rn() const {
  return dsu_class_loader()->dsu()->from_rn();
}

int DSUClass::to_rn() const {
  return dsu_class_loader()->dsu()->to_rn();
}

DSUClass::~DSUClass() {
  DSUMethod* p = first_method();

  while(p != NULL) {
    DSUMethod * q = p;
    p = p->next();
    delete q;
  }

  _first_method = _last_method = NULL;

  if (_match_static_fields != NULL) {
    FREE_C_HEAP_ARRAY(jint, _match_static_fields, mtInternal);
  }

  if (_name != NULL) _name->decrement_refcount();

  // TODO, see unchanged class
  //
  if (_new_version != NULL) {
    HandleMark hm;
    InstanceKlass* new_ik (new_version_class());
    // let verifier happy.
    new_ik->set_is_stale_class();
  }

  // As the stream provider may be shared by lots of classes
  // we make a check here and free it accordingly
  if (_stream_provider != NULL && !_stream_provider->is_shared()) {
    delete _stream_provider;
  }

  if (_resolved_reflections != NULL) {
    int length = _resolved_reflections->length();
    for(int i=0; i<length; i++) {
      jobject s = _resolved_reflections->at(i);
      JNIHandles::destroy_global(s);
    }
    delete _match_reflections;
  }
}

DSUMethod* DSUClass::allocate_method(Method* method, TRAPS) {
  DSUMethod* dsu_method = new DSUMethod();

  dsu_method->set_dsu_class(this);
  dsu_method->set_method(method);
  dsu_method->set_matched_new_method_index(-1);

  this->add_method(dsu_method);

  return dsu_method;
}

void DSUClass::add_method(DSUMethod * method) {
  assert( method->next() == NULL, "should not in any other list.");
  if (_first_method == NULL) {
    assert(_last_method == NULL, "sanity check");
    _first_method = _last_method = method;
  } else {
    assert(_last_method != NULL, "sanity check");
    _last_method->set_next(method);
    _last_method = method;
  }
}

void DSUClass::methods_do(void f(DSUMethod* dsu_method,TRAPS), TRAPS) {
  for(DSUMethod* p = first_method(); p!=NULL; p=p->next()) {
    f(p, CHECK);
  }
}

DSUMethod* DSUClass::find_method(Symbol* name, Symbol* desc) {
  for(DSUMethod* p = first_method(); p!=NULL; p=p->next()) {
    methodHandle mh(p->method());
    if (mh->name() == name && mh->signature() == desc) {
      return p;
    }
  }
  return NULL;
}


// ---------------------------- DSUMethod --------------------------

DSUMethod::DSUMethod()
: _updating_type(DSU_METHOD_NONE),
  _method(NULL),
  _matched_new_method_index(-1),
  _dsu_class(NULL),
  _next(NULL) {}

DSUMethod::~DSUMethod() {}

// TODO
bool DSUMethod::validate(TRAPS) {
  return false;
}

Method* DSUMethod::method() const {
  return _method;
}


//TODO here we just replace the handle of methodOop.
void StackRepairClosure::do_oop(oop *p) {

}


// --------------------- DeveloperInterface Entry ----------------

//return revision number of current thread.
JVM_ENTRY(jint, CurrentRevisionNumber(JNIEnv *env, jclass cls))
  return thread->current_revision();
JVM_END

// GetMixThat:
// return the MixNewObject if it exists
JVM_ENTRY(jobject, GetMixThat(JNIEnv *env,jclass cls,jobject o))
  oop obj = JNIHandles::resolve_non_null(o);
  Handle o_h(THREAD, obj);
  Javelus::transform_object_common(o_h, CHECK_NULL);

  if (obj->mark()->is_mixed_object()) {
    return JNIHandles::make_local(env, (oop)obj->mark()->decode_phantom_object_pointer());
  }
  return o;
JVM_END

JVM_ENTRY(jobject, ReplaceObject(JNIEnv *env, jclass clas, jobject old_o, jobject new_o))
  oop old_obj = JNIHandles::resolve_non_null(old_o);
  oop new_obj = JNIHandles::resolve_non_null(new_o);

  Handle o_h (THREAD, old_obj);
  Handle n_h (THREAD, new_obj);
  Javelus::transform_object_common(o_h, CHECK_NULL);
  Javelus::transform_object_common(n_h, CHECK_NULL);

  if (new_obj->mark()->is_mixed_object()) {
    return NULL;
  }

  InstanceKlass* ik = InstanceKlass::cast(n_h->klass());

  assert(!(ik->is_stale_class() || ik->is_inplace_new_class()),  "only mix new or new here");

  Handle o_phantom (THREAD, o_h());
  if (o_h->mark()->is_mixed_object()) {
    o_phantom = Handle(THREAD, (oop) o_h->mark()->decode_phantom_object_pointer());
  }
  while (ik != NULL) {
    Javelus::copy_fields(n_h(), o_h(), o_phantom(), ik);
    ik = ik->superklass();
  }
  return JNIHandles::make_local(env, old_obj);

JVM_END

void invoke_dsu_common(const char *dynamic_patch, jboolean sync, TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);
  DSUDynamicPatchBuilder patch_builder(dynamic_patch);

  DSU_DEBUG(("Dynamic patch file: %s", dynamic_patch));

  if (!patch_builder.build(THREAD)) {
    DSU_WARN(("Parsing dynamic patch met exceptions, clear DSU"));
    return;
  }

  if (HAS_PENDING_EXCEPTION) {
    Handle pending_exception (THREAD, PENDING_EXCEPTION);
    pending_exception->print();
    // cleanup outside the handle mark.
    //report parse DSU error?
    DSU_WARN(("Parsing dynamic patch met exceptions, clear DSU"));
    CLEAR_PENDING_EXCEPTION;
    return ;
  }

  DSU* dsu = patch_builder.dsu();

  dsu->validate(CHECK);

  VM_DSUOperation * op = new VM_DSUOperation(dsu);
  DSUTask* task = new DSUTask(op);

  Javelus::get_dsu_thread()->add_task(task);

  if (sync) {
    MutexLocker locker(DSURequest_lock);
    //tty->print_cr("wait until DSU finish try");
    DSURequest_lock->wait();
  }

}

void invoke_dsu_default(jboolean sync, TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);
  // setup the arguments to getProperty
  Handle key_str   = java_lang_String::create_from_str("org.javelus.dynamicPatch", CHECK);

  // return value
  JavaValue result(T_OBJECT);

  // public static String getProperty(String key, String def);
  JavaCalls::call_static(&result,
                         KlassHandle(THREAD, SystemDictionary::System_klass()),
                         vmSymbols::getProperty_name(),
                         vmSymbols::string_string_signature(),
                         key_str,
                         CHECK);

  oop value_oop = (oop)result.get_jobject();
  if (value_oop == NULL) {
    DSU_WARN(("Please set org.javelus.dynamicPatch=/path/to/dynamic/patch!"));
    return;
  }

  // convert Java String to utf8 string
  const char* value = java_lang_String::as_utf8_string(value_oop);

  invoke_dsu_common(value, sync, CHECK);
}

JVM_ENTRY(void, InvokeDSU(JNIEnv *env,jclass cls, jstring dynamic_patch, jboolean sync))
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);
  Handle h_patch (THREAD, JNIHandles::resolve_non_null(dynamic_patch));
  const char* dynamic_patch_str   = java_lang_String::as_utf8_string(h_patch());
  invoke_dsu_common(dynamic_patch_str, sync, CHECK);
JVM_END

JVM_ENTRY(void, InvokeDSU2(JNIEnv *env, jclass cls))
  invoke_dsu_default(true, CHECK);
JVM_END

JVM_ENTRY(void, InvokeDSU3(JNIEnv *env, jclass cls, jboolean sync))
  invoke_dsu_default(sync, CHECK);
JVM_END

const char * Javelus::class_updating_type_name(DSUClassUpdatingType ct) {
  switch(ct) {
  case DSU_CLASS_NONE:
    return "DSU_CLASS_NONE";
  case DSU_CLASS_MC:
    return "DSU_CLASS_MC";
  case DSU_CLASS_BC:
    return "DSU_CLASS_BC";
  case DSU_CLASS_SMETHOD:
    return "DSU_CLASS_SMETHOD";
  case DSU_CLASS_SFIELD:
    return "DSU_CLASS_SFIELD";
  case DSU_CLASS_SBOTH:
    return "DSU_CLASS_SBOTH";
  case DSU_CLASS_METHOD:
    return "DSU_CLASS_METHOD";
  case DSU_CLASS_FIELD:
    return "DSU_CLASS_FIELD";
  case DSU_CLASS_BOTH:
    return "DSU_CLASS_BOTH";
  case DSU_CLASS_DEL:
    return "DSU_CLASS_DEL";
  case DSU_CLASS_STUB:
    return "DSU_CLASS_STUB";
  case DSU_CLASS_UNKNOWN:
    return "DSU_CLASS_UNKNOWN";
  default:
    DSU_WARN(("int to DSUClassUpdatingType error!, unsupported type %d",ct));
    ShouldNotReachHere();
  }
  return "DSU_CLASS_NONE";
}


JVM_ENTRY(void, RedefineSingleClass(JNIEnv *env, jclass cls, jstring name, jbyteArray file))
  //%note jni_3
  assert(false, "Deprecated!!");
  Handle loader;
  Handle protection_domain;
  // Find calling class
  InstanceKlass* k = InstanceKlass::cast(thread->security_get_caller_class(1));
  if (k != NULL) {
    loader = Handle(THREAD, k->class_loader());
  } else {
    // We call ClassLoader.getSystemClassLoader to obtain the system class loader.
    loader = Handle(THREAD, SystemDictionary::java_system_loader());
  }

  oop name_oop = JNIHandles::resolve_non_null(name);
  Handle name_handle(THREAD, name_oop);

  Symbol* sym = java_lang_String::as_symbol(name_handle, THREAD);
  jclass result = NULL;

  result = find_class_from_class_loader(env, sym, true, loader,
    protection_domain, true, thread);

  oop tmp = JNIHandles::resolve_non_null(file);
  typeArrayOop a = typeArrayOop(JNIHandles::resolve_non_null(file));

  int len = a->length();
  jbyte *bytes = NEW_C_HEAP_ARRAY(jbyte, len, mtInternal);

  memcpy(bytes, a->byte_at_addr(0), sizeof(jbyte)*len);

  jvmtiClassDefinition class_definitions;
  class_definitions.klass = result;
  class_definitions.class_byte_count = len;
  class_definitions.class_bytes = (unsigned char *)bytes;

  VM_RedefineClasses op(1, &class_definitions, jvmti_class_load_kind_redefine);
  VMThread::execute(&op);
  FREE_C_HEAP_ARRAY(jbyte, bytes, mtInternal);
JVM_END



Dictionary*     Javelus::_dsu_dictionary = NULL;
DSUThread*      Javelus::_dsu_thread = NULL;
Method*         Javelus::_implicit_update_method = NULL;
InstanceKlass*  Javelus::_developer_interface_klass = NULL;

DSU*            Javelus::_first_dsu = NULL;
DSU*            Javelus::_last_dsu = NULL;
DSU* volatile   Javelus::_active_dsu = NULL;
int             Javelus::_latest_rn =  0;
const int       Javelus::MIN_REVISION_NUMBER = -1;
const int       Javelus::MAX_REVISION_NUMBER = 100;

InstanceKlass* Javelus::developer_interface_klass() {
  return _developer_interface_klass;
}

void Javelus::set_active_dsu(DSU* dsu) {
  assert(Thread::current()->is_DSU_thread(), "sanity");
  assert(_active_dsu == NULL, "sanity");
  _active_dsu = dsu;
  active_dsu()->set_from_rn(Javelus::system_revision_number());
  active_dsu()->set_to_rn  (Javelus::system_revision_number() + 1);
}

DSU* Javelus::active_dsu() {
  return _active_dsu;
}

void Javelus::finish_active_dsu() {
  assert(SafepointSynchronize::is_at_safepoint(), "sanity" );
  assert(_active_dsu != NULL, "sanity");

  install_dsu(active_dsu());

  _active_dsu = NULL;

  increment_system_rn();
}

void Javelus::discard_active_dsu() {
  DSU* dsu = _active_dsu;
  if (dsu != NULL) {
    delete dsu;
    _active_dsu = NULL;
  }
}

DSU* Javelus::get_DSU(int from_rn) {
  if (_first_dsu == NULL) {
    return NULL;
  }

  for(DSU* dsu = _first_dsu; dsu != NULL; dsu = dsu->next()) {
    if (dsu->from_rn() == from_rn) {
      return dsu;
    }
  }
  return NULL;
}

void Javelus::create_DevelopInterface_klass(TRAPS) {

  // create constantPool

  InstanceKlass* super = InstanceKlass::cast(SystemDictionary::Object_klass());
  Array<Klass*>* local_interfaces = Universe::the_empty_klass_array();
  Array<u2>* fields = Universe::the_empty_short_array();

  // create methods
  // 1. <init>
  // 2. invokeDSU
  enum {
    init_index = 0,
    invokeDSU_index =1,
    invokeDSU2_index =2,
    invokeDSU3_index =3,
    redefine_index = 4,
    getmixthat_index = 5,
    replaceObject_index = 6,
    crn_index = 7,
    total_methods
  };

  Array<Method*>* methods = MetadataFactory::new_array<Method*>(ClassLoaderData::the_null_class_loader_data(), total_methods, CHECK);

  InlineTableSizes sizes(
      0, // total_lvt_length,
      0, // linenumber_table_length,
      0, // exception_table_length,
      0, // checked_exceptions_length,
      0, // method_parameters_length,
      0, // generic_signature_index,
      0, // runtime_visible_annotations_length + runtime_invisible_annotations_length,
      0, // runtime_visible_parameter_annotations_length + runtime_invisible_parameter_annotations_length,
      0, // runtime_visible_type_annotations_length + runtime_invisible_type_annotations_length,
      0, // annotation_default_length,
      0);

  Method* m_init = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
            5, // 0 aload_0; 1 invokespecial Object.<init>; 4 return;
            accessFlags_from(0), &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_invokeDSU = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_invokeDSU2 = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_invokeDSU3 = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_redefine = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_getmixthat = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_replaceObject = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);

  Method* m_crn = Method::allocate(ClassLoaderData::the_null_class_loader_data(),
      0,
      accessFlags_from( JVM_ACC_PUBLIC | JVM_ACC_STATIC | JVM_ACC_NATIVE),
      &sizes, ConstMethod::NORMAL, CHECK);


  enum {
    MethodRef_Object_init_index = 1,
    Class_DeveloperInterface_index,
    Class_Object_index,
    Symbol_init_name_index,
    Symbol_init_sig_index,
    NameAndType_init_index,
    Symbol_invokeDSU_name_index,
    Symbol_invokeDSU_sig_index,
    Symbol_invokeDSU2_sig_index,
    Symbol_invokeDSU3_sig_index,
    Symbol_redefine_name_index,
    Symbol_redefine_sig_index,
    Symbol_getMixThat_name_index,
    Symbol_getMixThat_sig_index,
    Symbol_replaceObject_name_index,
    Symbol_replaceObject_sig_index,
    Symbol_crn_name_index,
    Symbol_crn_sig_index,
    Limit
    //    Symbol_invokeDSU_signature_index = Symbol_init_sig_index,
  };


  ConstantPool* cp = ConstantPool::allocate(ClassLoaderData::the_null_class_loader_data(), Limit, CHECK);

  const char * c_org_javelus_DeveloperInterface = "org/javelus/DeveloperInterface";
  Symbol* org_javelus_DeveloperInterface = SymbolTable::lookup(c_org_javelus_DeveloperInterface, (int)strlen(c_org_javelus_DeveloperInterface), CHECK);

  const char * c_invokeDSU_name = "invokeDSU";
  Symbol* invokeDSU_name = SymbolTable::lookup(c_invokeDSU_name, (int)strlen(c_invokeDSU_name), CHECK);

  const char * c_string_boolean_void_signature = "(Ljava/lang/String;Z)V";
  Symbol* string_boolean_void_signature = SymbolTable::lookup(c_string_boolean_void_signature, (int)strlen(c_string_boolean_void_signature), CHECK);

  const char * c_boolean_void_signature = "(Z)V";
  Symbol* boolean_void_signature = SymbolTable::lookup(c_boolean_void_signature, (int)strlen(c_boolean_void_signature), CHECK);

  const char * c_redefineSingleClass_name = "redefineSingleClass";
  Symbol* redefineSingleClass_name = SymbolTable::lookup(c_redefineSingleClass_name, (int)strlen(c_redefineSingleClass_name), CHECK);

  const char * c_string_byte_array_void_signature = "(Ljava/lang/String;[B)V";
  Symbol* string_byte_array_void_signature = SymbolTable::lookup(c_string_byte_array_void_signature, (int)strlen(c_string_byte_array_void_signature), CHECK);

  const char * c_getMixThat_name = "getMixThat";
  Symbol* getMixThat_name = SymbolTable::lookup(c_getMixThat_name, (int)strlen(c_getMixThat_name), CHECK);

  const char * c_object_object_signature = "(Ljava/lang/Object;)Ljava/lang/Object;";
  Symbol* object_object_signature = SymbolTable::lookup(c_object_object_signature, (int)strlen(c_object_object_signature), CHECK);

  const char * c_replaceObject_name = "replaceObject";
  Symbol* replaceObject_name = SymbolTable::lookup(c_replaceObject_name, (int)strlen(c_replaceObject_name), CHECK);

  const char * c_object_object_object_signature = "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;";
  Symbol* object_object_object_signature = SymbolTable::lookup(c_object_object_object_signature, (int)strlen(c_object_object_object_signature), CHECK);

  const char * c_crn_name = "currentRevisionNumber";
  Symbol* crn_name = SymbolTable::lookup(c_crn_name, (int)strlen(c_crn_name), CHECK);

  cp->method_at_put(MethodRef_Object_init_index,Class_Object_index,NameAndType_init_index);
  //cp->klass_at_put(Class_DeveloperInterface_index,NULL);
  cp->klass_at_put(Class_Object_index, SystemDictionary::Object_klass());
  cp->symbol_at_put(Symbol_init_name_index, vmSymbols::object_initializer_name());
  cp->symbol_at_put(Symbol_init_sig_index, vmSymbols::void_method_signature());
  cp->name_and_type_at_put(NameAndType_init_index,Symbol_init_name_index,Symbol_init_sig_index);
  cp->symbol_at_put(Symbol_invokeDSU_name_index, invokeDSU_name);
  cp->symbol_at_put(Symbol_invokeDSU_sig_index, string_boolean_void_signature);
  cp->symbol_at_put(Symbol_invokeDSU2_sig_index, vmSymbols::void_method_signature());
  cp->symbol_at_put(Symbol_invokeDSU3_sig_index, boolean_void_signature);
  cp->symbol_at_put(Symbol_redefine_name_index, redefineSingleClass_name);
  cp->symbol_at_put(Symbol_redefine_sig_index, string_byte_array_void_signature);
  cp->symbol_at_put(Symbol_getMixThat_name_index, getMixThat_name);
  cp->symbol_at_put(Symbol_getMixThat_sig_index, object_object_signature);
  cp->symbol_at_put(Symbol_replaceObject_name_index, replaceObject_name);
  cp->symbol_at_put(Symbol_replaceObject_sig_index, object_object_object_signature);
  cp->symbol_at_put(Symbol_crn_name_index, crn_name);
  cp->symbol_at_put(Symbol_crn_sig_index, vmSymbols::void_int_signature());

  m_init->set_constants(cp);
  m_init->set_name_index(Symbol_init_name_index);
  m_init->set_signature_index(Symbol_init_sig_index);


  unsigned char code[5] = {
    Bytecodes::_aload_0,
    Bytecodes::_invokespecial,
    0x00,
    (unsigned char) MethodRef_Object_init_index,
    Bytecodes::_return
  };
  m_init->set_code(code);
  m_init->compute_size_of_parameters(THREAD);

  m_invokeDSU->set_constants(cp);
  m_invokeDSU->set_name_index(Symbol_invokeDSU_name_index);
  m_invokeDSU->set_signature_index(Symbol_invokeDSU_sig_index);
  m_invokeDSU->compute_size_of_parameters(THREAD);

  m_invokeDSU2->set_constants(cp);
  m_invokeDSU2->set_name_index(Symbol_invokeDSU_name_index);
  m_invokeDSU2->set_signature_index(Symbol_invokeDSU2_sig_index);
  m_invokeDSU2->compute_size_of_parameters(THREAD);

  m_invokeDSU3->set_constants(cp);
  m_invokeDSU3->set_name_index(Symbol_invokeDSU_name_index);
  m_invokeDSU3->set_signature_index(Symbol_invokeDSU3_sig_index);
  m_invokeDSU3->compute_size_of_parameters(THREAD);

  m_redefine->set_constants(cp);
  m_redefine->set_name_index(Symbol_redefine_name_index);
  m_redefine->set_signature_index(Symbol_redefine_sig_index);
  m_redefine->compute_size_of_parameters(THREAD);

  m_getmixthat->set_constants(cp);
  m_getmixthat->set_name_index(Symbol_getMixThat_name_index);
  m_getmixthat->set_signature_index(Symbol_getMixThat_sig_index);
  m_getmixthat->compute_size_of_parameters(THREAD);

  m_replaceObject->set_constants(cp);
  m_replaceObject->set_name_index(Symbol_replaceObject_name_index);
  m_replaceObject->set_signature_index(Symbol_replaceObject_sig_index);
  m_replaceObject->compute_size_of_parameters(THREAD);

  m_crn->set_constants(cp);
  m_crn->set_name_index(Symbol_crn_name_index);
  m_crn->set_signature_index(Symbol_crn_sig_index);
  m_crn->compute_size_of_parameters(THREAD);


  methods->at_put(init_index, m_init);
  methods->at_put(invokeDSU_index, m_invokeDSU);
  methods->at_put(invokeDSU2_index, m_invokeDSU2);
  methods->at_put(invokeDSU3_index, m_invokeDSU3);
  methods->at_put(redefine_index, m_redefine);
  methods->at_put(getmixthat_index, m_getmixthat);
  methods->at_put(replaceObject_index, m_replaceObject);
  methods->at_put(crn_index, m_crn);

  //set up entry
  //m_invokeDSU->link_method(m_invokeDSU,CHECK);

  InstanceKlass* ikh = InstanceKlass::allocate_instance_klass(ClassLoaderData::the_null_class_loader_data(),
      super->vtable_length(),
      super->itable_length(),
      0,
      0,
      super->reference_type(),
      accessFlags_from(JVM_ACC_PUBLIC),
      org_javelus_DeveloperInterface,
      super,
      false,
      CHECK);



  ikh->set_class_loader_data(ClassLoaderData::the_null_class_loader_data());
  ikh->set_should_verify_class(false);
  ikh->set_layout_helper(super->layout_helper());
  //ikh->set_class_loader(NULL);


  ikh->set_nonstatic_field_size(super->nonstatic_field_size());
  ikh->set_has_nonstatic_fields(super->has_nonstatic_fields());
  cp->set_pool_holder(ikh);
  ikh->set_constants(cp);
  ikh->set_local_interfaces(local_interfaces);
  ikh->set_fields(fields, 0);
    // sort methods
  Method::sort_methods(methods);
  ikh->set_methods(methods);
  ikh->set_has_final_method();


  if (JvmtiExport::can_maintain_original_method_order()) {
    int length = ikh->methods()->length();
    Array<int>* method_ordering = MetadataFactory::new_array<int>(ClassLoaderData::the_null_class_loader_data(), length, CHECK);
    for (int index = 0; index < length; index++) {
      Method* m =methods->at(index);
      method_ordering->at_put(index, index);
      m->set_vtable_index(Method::invalid_vtable_index);
    }
    ikh->set_method_ordering(method_ordering);
  } else {
    ikh->set_method_ordering(Universe::the_empty_int_array());
  }


  // The instanceKlass::_methods_jmethod_ids cache and the
  // instanceKlass::_methods_cached_itable_indices cache are
  // both managed on the assumption that the initial cache
  // size is equal to the number of methods in the class. If
  // that changes, then instanceKlass::idnum_can_increment()
  // has to be changed accordingly.
  ikh->set_initial_method_idnum(methods->length());
  ikh->set_name(org_javelus_DeveloperInterface);
  cp->klass_at_put(Class_DeveloperInterface_index, ikh);

  ikh->set_transitive_interfaces(super->transitive_interfaces());

  ikh->set_minor_version(0);
  ikh->set_major_version(50);

  ikh->set_born_rn(0);
  ikh->set_dead_rn(Javelus::MAX_REVISION_NUMBER);
  ikh->set_copy_to_size(ikh->size_helper());
  ikh->set_inner_classes(Universe::the_empty_short_array());

  ikh->initialize_supers(super, CHECK);

  java_lang_Class::create_mirror(ikh, Handle(THREAD, ikh->class_loader()), Handle(THREAD, NULL), CHECK);

  SystemDictionary::define_instance_class(instanceKlassHandle(ikh),THREAD);
  //--- finish load

  // link
  ikh->link_class(THREAD);

  // initialize
  // ikh->initialize(THREAD);


  //register native method
  m_invokeDSU->set_native_function(
    CAST_FROM_FN_PTR(address, &InvokeDSU),
    Method::native_bind_event_is_interesting);

  m_invokeDSU2->set_native_function(
    CAST_FROM_FN_PTR(address, &InvokeDSU2),
    Method::native_bind_event_is_interesting);

  m_invokeDSU3->set_native_function(
    CAST_FROM_FN_PTR(address, &InvokeDSU3),
    Method::native_bind_event_is_interesting);

  m_redefine->set_native_function(
    CAST_FROM_FN_PTR(address, &RedefineSingleClass),
    Method::native_bind_event_is_interesting);

  m_getmixthat->set_native_function(
    CAST_FROM_FN_PTR(address, &GetMixThat),
    Method::native_bind_event_is_interesting);

  m_replaceObject->set_native_function(
    CAST_FROM_FN_PTR(address, &ReplaceObject),
    Method::native_bind_event_is_interesting);

  m_crn->set_native_function(
    CAST_FROM_FN_PTR(address, &CurrentRevisionNumber),
    Method::native_bind_event_is_interesting);

  _developer_interface_klass = ikh;
}




void Javelus::initialize(TRAPS) {
  create_DevelopInterface_klass(CHECK);
  DSUEagerUpdate::initialize(CHECK);
}


void Javelus::dsu_thread_init() {
  _dsu_thread            = make_dsu_thread("DSU Thread");
  _dsu_dictionary        = new Dictionary(100);
}

//Currently, we use two pass iteration to update a single thread..
//Why? I don't know.
// In first pass, we use a vframeStream.
// In second pass, we use a stackFrame
// check_single_thread is only used for policy THREAD_SAFE_POINT.
// So any target rn must be less or equal than
bool Javelus::check_single_thread(JavaThread * thread) {
  assert(thread == JavaThread::current(),"Only the thread itself can update itself");
  int from_rn = thread->current_revision();
  int to_rn   = from_rn + 1;

  if (to_rn > Javelus::system_revision_number()) {
    // it is a invalid update...
    return false;
  }

  bool do_set_barrier= false;

  bool thread_safe = true;

  // The return_barrier_id has been cleared before calling this method.
  //assert(thread->return_barrier_id() != NULL, "If it is old and it must have return barrier");

  HandleMark hm(thread);
  for(vframeStream vfst(thread); !vfst.at_end(); vfst.next()) {
    methodHandle method (thread, vfst.method());

    InstanceKlass* ik = method->method_holder();


    if (ik->dead_rn() == to_rn) {
      // If the method is changed, it is restricted for any updating
      // XXX changed methods must have already been marked dead here.
      // Changed method must die at to_rn
      if (method->is_restricted_method() ) {
        thread_safe = false;
        // Here we install return barrier
        // TODO Here we just set the id and will install it at repair_thread
        // set barrier at caller if possible
        do_set_barrier = true;
      } else {
        if (do_set_barrier) {
          thread->set_return_barrier_id(vfst.frame_id());
        }

        // Here we must check whether the frame is a optimized dead method.
        // If it is we must deoptimize current frame
        if (method->is_native()) {
          continue;
        } else if (vfst.is_interpreted_frame()) {
          continue;
        } else {
          // Here the method is a compiled dead method
          // XXX the method will do a re-walk from top to find the frame and ....
          Deoptimization::deoptimize_frame(thread,vfst.frame_id());
        }
      }
    } else if (do_set_barrier) {
      thread->set_return_barrier_id(vfst.frame_id());
    }

  }// end of frame iteration

  return thread_safe;
}

void Javelus::repair_single_thread(JavaThread * thread) {
  assert(thread == JavaThread::current(),"only the thread itself can repair itself.");

  int from_rn = thread->current_revision();
  // note we
  int to_rn   = from_rn;// + 1;

  frame * current = NULL;
  frame * callee = NULL;
  StackFrameStream callee_fst(thread);
  int count = 0;
  for(StackFrameStream fst(thread); !fst.is_done(); fst.next()) {
    current = fst.current();
    if (count>0) {
      callee = callee_fst.current();
      callee_fst.next();
    }
    count ++;
    if (current->is_interpreted_frame()) {
      ResourceMark rm;
      // update method and constantpoolcache
      Method* method = current->interpreter_frame_method();
      // XXX We do not know
      InstanceKlass* ik = method->method_holder();
      //TODO
      //We must set up the old flag during VM_DSUOperation
      if (ik->dead_rn() == to_rn) {
        // TODO replace loosely restricted method
        // Here we can not allow any changed method
        assert( !method->is_restricted_method(), "No restricted method here !!");
        // tty->print_cr("repair single thread %s", method->name_and_sig_as_C_string());
        //TODO Currently We on-stack-replace the loosely-stricted method with next matched version.
        InstanceKlass* new_ik = ik->next_version();
        Method* new_method = new_ik->find_method(method->name(), method->signature());
        assert(new_method != NULL, "loosely restricted method must have a new version");
        int bci = current->interpreter_frame_bci();

        // TODO
        // I have no idea why we cannot build interpreter method as profile_method here.
        // Obviously, we are not in thread_vm.
        // But how can profile_method do...
        //methodOopDesc::build_interpreter_method_data(new_method, thread);
        //
        //if (thread->has_pending_exception()) {
        //  DSU_WARN(("build interpreter method data meets exception in repair_single_thread"));
        //}
        current->interpreter_frame_set_method(new_method);
        current->interpreter_frame_set_bcp(new_method->bcp_from(bci));
        *(current->interpreter_frame_cache_addr()) = new_method->constants()->cache();

        // TODO, see deoptimzation.
        // We must clear the resolved flags in cp cache.
        // But we must preserve the size of parameters.

        if (callee != NULL && callee->is_interpreted_frame()) {
          Bytecodes::Code code  = Bytecodes::code_at(method, method->bcp_from(bci));

          if (code >= Bytecodes::_invokevirtual  && code <= Bytecodes::_invokedynamic) {
            Bytecodes::Code new_code  = Bytecodes::code_at(new_method, new_method->bcp_from(bci));

            assert(code == new_code, "new code must be old code");

            int old_entry_index = Bytes::get_native_u2((address)method->bcp_from(bci) +1);
            int new_entry_index = Bytes::get_native_u2((address)new_method->bcp_from(bci) +1);
            ConstantPoolCacheEntry * new_entry = new_method->constants()->cache()->entry_at(new_entry_index);

            if (UseInterpreter && !new_entry->is_resolved(code)) {
              ConstantPoolCacheEntry * old_entry = method->constants()->cache()->entry_at(old_entry_index);
              assert(old_entry->is_resolved(code), "old entry must be resolved..");
              ConstantPoolCache::copy_method_entry(method->constants()->cache(), old_entry_index, new_method->constants()->cache(), new_entry_index);
            } else {
              DSU_WARN(("Thread repair finds a resolved new entry"));
            }
          } else {
            assert(bci == 0 || code == Bytecodes::_monitorenter,"other cases");
          }
        }
      }
    } else if (current->is_compiled_frame()) {

    } else if (current->is_native_frame()) {

    }
  }// end for frame walk loop

}

void Javelus::invoke_return_barrier(JavaThread* thread) {
  assert(thread == Thread::current(), "sanity check");
  ResourceMark rm(thread);
  intptr_t* return_barrier_id = thread->return_barrier_id();

  DSU_TRACE(0x00000100,("Thread %s enters a return barrier. [" PTR_FORMAT ",%d].",
              thread->get_thread_name(),
              p2i(return_barrier_id),
              thread->return_barrier_type()
              ));

  if (return_barrier_id == NULL) {
    return ;
  }

#if 0
  thread->print_stack_on(tty);

  RegisterMap reg_map(thread);
  vframe* start_vf = thread->last_java_vframe(&reg_map);
  assert(start_vf->is_java_frame(), "sanity check");

  start_vf->fr().print_on(tty);
#endif

  // First remove the return barrier.
  thread->clear_return_barrier_id();

  int from_rn = thread->current_revision();
  int   to_rn = from_rn + 1;

  // 1). first cases, we update this thread isolated;
  // TODO we need reconsider POLUS policy here
  //bool update_this_thread = thread->is_return_barrier_update_thread();
  bool update_this_thread = to_rn <= Javelus::system_revision_number();

  // 2). other two cases
  // 2.1). we do a retry
  //bool do_eager_update = return_barrier_id == NULL;
  bool do_eager_update = thread->is_return_barrier_eager_update();
  bool do_eager_wakeup = thread->is_return_barrier_eager_wakeup();
  // 2.2). we do eager udpate
  //bool do_retry = ! (update_this_thread || do_eager_update);
  bool do_retry = thread->is_return_barrier_wake_up();

  assert(!(do_retry && do_eager_update), "retry is conflicit with eager update.");

  // Remember to clear return barrier type here.
  if (!do_eager_wakeup) {
    thread->clear_return_barrier_type();
  }

  if (update_this_thread) {
    // it is a valid to_rn

    // We are safe to update to the next version here.
    thread->increment_revision();

    //Here may be continuous update
    while(Javelus::check_single_thread(thread)) {
      //Check and install return barrier
      Javelus::repair_single_thread(thread);
      //
      thread->increment_revision();
    }

    //Fetche installed return barrier id.
    intptr_t * barrier = thread->return_barrier_id();
    if (barrier != NULL) {
      Javelus::install_return_barrier_single_thread(thread,barrier);
    }
  } else if (do_eager_update) {
    DSUEagerUpdate::start_eager_update(thread);
    if (thread->has_pending_exception()) {
      DSU_TRACE_MESG(("eager update objects meets exception."));
    }
  } else if (do_eager_wakeup) {
    if (Javelus::active_dsu() != NULL) {
      bool last = true;

      {
        MutexLocker mu(Threads_lock);

        for (JavaThread *t = Threads::first(); t != NULL; t = t->next()) {
          if (thread == t) {
            continue;
          }

          if (t->is_return_barrier_eager_wakeup()) {
            DSU_TRACE(0x00000100,("Thread %s may be the last",
              t->get_thread_name()
              ));
            last = false;
            break;
          }
        }

        // clear return barrier
        thread->clear_return_barrier_type();
      }

      if (last) {
        DSU_TRACE(0x00000100,("Thread %s is last " PTR_FORMAT,
              thread->get_thread_name(),
              p2i(return_barrier_id)
              ));

        Javelus::wakeup_DSU_thread();
        Javelus::waitRequest(EagerWakeupDSUSleepTime);
      } else { // not last wait the last to wakeup DSU
        DSU_TRACE(0x00000100,("Thread %s is not last " PTR_FORMAT,
              thread->get_thread_name(),
              p2i(return_barrier_id)
              ));
        Javelus::waitRequest(EagerWakeupDSUSleepTime);
      }
    }
  } else if (do_retry) {
    // from rn is equal with system rn
    // Here may be a retry of existing DSU request.
    // Just wake up the sleeping DSUThread.
    DSU_TRACE_MESG(("wakeup DSU Thread at return barrier."));
    if (Javelus::active_dsu() != NULL) {
      Javelus::wakeup_DSU_thread();
      Javelus::waitRequest();
    }
  } else {
    ShouldNotReachHere();
  }



  DSU_TRACE(0x00000100,("Thread %s leaves a return barrier.",
              thread->get_thread_name()
              ));

}



void Javelus::install_return_barrier_all_threads() {
  for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
    if (thr->is_Compiler_thread()) {
      continue;
    }
    //thread is walkable
    intptr_t * barrier = thr->return_barrier_id();
    if (barrier == NULL) {
      continue;
    }
    if (thr->has_last_Java_frame()) {
      install_return_barrier_single_thread(thr,barrier);
    }
  }
}

// the barrier is the id of the rm.
// We should replace its caller's pc.
void Javelus::install_return_barrier_single_thread(JavaThread * thread, intptr_t * barrier) {
  assert(thread->return_barrier_id() == barrier,"just check return barrier id.");

  for(StackFrameStream fst(thread); !fst.is_done(); fst.next()) {
    frame * current = fst.current();
    if (current->id() == barrier) {
      if (EagerWakeupDSU) {
        thread->set_return_barrier_type(JavaThread::_eager_wakeup_dsu);
      } else {
        thread->set_return_barrier_type(JavaThread::_wakeup_dsu);
      }

      DSU_TRACE(0x00000100, ("Install return barrier for thread %s at "PTR_FORMAT,
              thread->get_thread_name(),
              p2i(barrier)));
      if (current->is_interpreted_frame()) {
        Bytecodes::Code code = Bytecodes::code_at(current->interpreter_frame_method(), current->interpreter_frame_bcp());
        if (!(code == Bytecodes::_invokespecial || code == Bytecodes::_invokevirtual || code == Bytecodes::_invokedynamic || code == Bytecodes::_invokeinterface)) {
          current->print_on(tty);
        }
        assert(code == Bytecodes::_invokespecial || code == Bytecodes::_invokevirtual || code == Bytecodes::_invokedynamic || code == Bytecodes::_invokeinterface, "sanity check");
        const int length = Bytecodes::length_at(current->interpreter_frame_method(), current->interpreter_frame_bcp());
        Bytecode_invoke bi(methodHandle(current->interpreter_frame_method()), current->interpreter_frame_bci());
        TosState tos = as_TosState(bi.result_type());
        address barrier_addr, return_addr;
        barrier_addr = Interpreter::return_with_barrier_entry(tos, length, code);
        return_addr = Interpreter::return_entry(tos, length, code);
        assert(current->pc() == return_addr || current->pc() == barrier_addr, "must be return or pre-set return with barrier");
        current->patch_pc(thread, barrier_addr);
      } else if (current->is_compiled_frame()) {
        // Do nothing.
        // XXX set the address of
        if (current->is_deoptimized_frame()) {
          // We do not depotimize a deoptimized frame.
        } else {
          // We just deoptimize it
          Deoptimization::deoptimize(thread, *current, fst.register_map());
        }
      }
      return;
    }
  }
}


// This method will check all application threads.
// If there exists restricted method on stack
// XXX remember!!
// Return true if it is system-wide safe.
// System-wide safe <==> all threads are safe to update to system revision number.
bool Javelus::check_application_threads() {
  assert(SafepointSynchronize::is_at_safepoint(),
    "DSU safepoint must be in VM safepoint");

  bool do_print = DSU_TRACE_ENABLED(0x00000080);

  // In default, to revision number is current system revision number plus one.
  int sys_from_rn = Javelus::system_revision_number();
  int sys_to_rn = sys_from_rn + 1;
  bool sys_safe = true;

  ResourceMark rm;

  for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {

    if (!thr->has_last_Java_frame()) {
      continue;
    }/*else {
     tty->print_cr("JavaThread::");
     }*/

    bool thread_safe = true;
    int t_from_rn = thr->current_revision();

    // In default, the target rn will be increment by one.
    int t_to_rn   = t_from_rn + 1;

    if (do_print) {
      tty->print_cr("[DSU] Check thread %s.",thr->get_thread_name());
    }

    bool do_set_barrier = false;
    for(vframeStream vfst(thr); !vfst.at_end(); vfst.next()) {
      Method* method = vfst.method();
      InstanceKlass* ik = method->method_holder();
      if (!ik->oop_is_instance() ) {
        //We only check instanceKlass
        continue;
      }
      if (t_from_rn == sys_from_rn) {
        // The thread is in the youngest version and will updated to a new version
        assert(ik->dead_rn() >= sys_to_rn, "the method must be alive here");

        // If the method is changed, it is restricted for any updating
        if (method->is_restricted_method()) {
          thread_safe = false;
          // Here we install return barrier
          // TODO Here we just set the id and will install it at repair_thread
          if (do_print) {
            tty->print_cr(" * [%s] - [%d,%d) id=" PTR_FORMAT, method->name_and_sig_as_C_string(), ik->born_rn(), ik->dead_rn(), p2i(vfst.frame_id()));
          }
          do_set_barrier = true;
        } else if (do_set_barrier) {
          // Return barrier can only be put after the caller of the oldest restricted method.
          if (do_print) {
            tty->print_cr(" + [%s] - [%d,%d) id="PTR_FORMAT, method->name_and_sig_as_C_string(),ik->born_rn(),ik->dead_rn(), p2i(vfst.frame_id()));
          }
          thr->set_return_barrier_id(vfst.frame_id());
          do_set_barrier = false;
        } else {
          if (do_print) {
            tty->print_cr(" - [%s] - [%d,%d) I[%d] id="PTR_FORMAT, method->name_and_sig_as_C_string(),ik->born_rn(),ik->dead_rn(),vfst.is_interpreted_frame(), p2i(vfst.frame_id()));
          }
        }
      } else if (t_from_rn < sys_from_rn) {
        //assert(thr->return_barrier_id() != NULL, "If it is old and it must have return barrier");
        if (do_print) {
          tty->print_cr(" # [%s] - [%d,%d) I[%d] id="PTR_FORMAT,method->name_and_sig_as_C_string(),ik->born_rn(),ik->dead_rn(),vfst.is_interpreted_frame(), p2i(vfst.frame_id()));
        }
        thread_safe = false;
        break;
      } else {
        // t_from_rn must <= sys_from_rn
        ShouldNotReachHere();
      }
    }// end of thread walking
    if ( !thread_safe
      /*t_to_rn != sys_to_rn*/) {
      // We can not update all thread to the system to rn
      sys_safe = false;
    }
  }// end of all thread walking
  return sys_safe;
}

InstanceKlass* Javelus::resolve_dsu_klass_or_null(Symbol* class_name, ClassLoaderData* loader_data, Handle protection_domain, TRAPS) {
    //assert(THREAD->is_DSU_thread(),"must be dsu thread");
    unsigned int d_hash = dictionary()->compute_hash(class_name, loader_data);
    int d_index = dictionary()->hash_to_index(d_hash);
    Klass* probe = dictionary()->find(d_index, d_hash, class_name, loader_data, protection_domain, CHECK_NULL);

    if (probe != NULL ) {
      return InstanceKlass::cast(probe);
    }

    // Check DSUClass again
    DSUClassLoader* dsu_class_loader = active_dsu()->find_class_loader_by_loader(loader_data->class_loader());
    if (dsu_class_loader == NULL) {
      return NULL;
    }
    DSUClass* dsu_class = dsu_class_loader->find_class_by_name(class_name);
    if (dsu_class == NULL) {
      return NULL;
    }

    InstanceKlass* old_version = NULL;
    int ret = dsu_class->resolve_old_version(old_version, THREAD);
    if (ret != DSU_ERROR_NONE) {
      return NULL;
    }

    // resolve new version
    InstanceKlass* new_version= NULL;
    dsu_class->resolve_new_version(new_version, CHECK_NULL);
    return new_version;
}

DSUClassUpdatingType Javelus::get_updating_type(InstanceKlass* ikh, TRAPS) {
  HandleMark hm(THREAD);

  Symbol* class_name = ikh->name();
  Handle class_loader(THREAD, ikh->class_loader());

  DSUClass* dsu_class = active_dsu()->find_class_by_name_and_loader(class_name, class_loader);

  if (dsu_class != NULL && dsu_class->prepared()) {
    return dsu_class->updating_type();
  }

  return DSU_CLASS_UNKNOWN;
}


void Javelus::prepare_super_class(InstanceKlass* ikh, TRAPS) {
  HandleMark hm(THREAD);

  InstanceKlass* super_class = InstanceKlass::cast(ikh->super());
  assert(super_class != NULL, "no java.lang.Object class");

  Symbol* class_name = super_class->name();
  Handle class_loader (THREAD, super_class->class_loader());
  DSUClass* dsu_class = active_dsu()->find_class_by_name_and_loader(class_name, class_loader);

  if (dsu_class != NULL) {
    dsu_class->prepare(CHECK);
  }
}

void Javelus::prepare_interfaces(InstanceKlass* ikh, TRAPS) {
  HandleMark hm(THREAD);

  Array<Klass*>* interfaces = ikh->local_interfaces();

  const int n_itfc = interfaces->length();
  for(int i=0; i<n_itfc; i++) {
    Klass* itfc = interfaces->at(i);
    assert(itfc != NULL, "no java.lang.Object class");

    Symbol* class_name = itfc->name();
    Handle class_loader (THREAD, itfc->class_loader());
    DSUClass* dsu_class = active_dsu()->find_class_by_name_and_loader(class_name, class_loader);

    if (dsu_class != NULL) {
      dsu_class->prepare(CHECK);
      assert(dsu_class->prepared(),"sanity now, ");
    }
  }
}

class PickClassLoaderCLDClosure : public CLDClosure {
 private:
  Symbol* _class_name;
  ClassLoaderData* _loader_data;
 public:
   PickClassLoaderCLDClosure(Symbol* class_name) : _class_name(class_name) {}
   void do_cld(ClassLoaderData* cld);
   ClassLoaderData* loader_data() const { return _loader_data; }
};



void PickClassLoaderCLDClosure::do_cld(ClassLoaderData* cld) {
  Klass* klass = Javelus::find_klass_in_system_dictionary(_class_name, cld);
  if (klass != NULL) {
    if (_loader_data != NULL) {
      if (klass->class_loader_data() == cld) {
        _loader_data = cld;
      } else {
        ResourceMark rm;
        DSU_WARN(("Ignore a non-defined class loader data for class %s", _class_name->as_C_string()));
      }
    } else {
      ResourceMark rm;
      DSU_WARN(("Find a duplicated class loader data for class %s", _class_name->as_C_string()));
    }
  }
}

void Javelus::pick_loader_by_class_name(Symbol* class_name, ClassLoaderData* &loader, TRAPS) {
  PickClassLoaderCLDClosure c(class_name);
  ClassLoaderDataGraph::cld_do(&c);
  loader = c.loader_data();
}

Klass* Javelus::find_klass_in_system_dictionary(Symbol* name, ClassLoaderData* data) {
  return SystemDictionary::find_class(name, data);
}

InstanceKlass* Javelus::resolve_dsu_klass(Symbol* class_name, ClassLoaderData* loader_data, Handle protection_domain, TRAPS) {
    InstanceKlass* klass = resolve_dsu_klass_or_null(class_name, loader_data, protection_domain, THREAD);
    if (HAS_PENDING_EXCEPTION || klass == NULL) {
      KlassHandle k_h(THREAD, klass);
      // can return a null klass
      Handle class_loader(THREAD, loader_data->class_loader());
      klass = InstanceKlass::cast(SystemDictionary::handle_resolution_exception(class_name, class_loader, protection_domain, true, k_h, THREAD));
    }
    return klass;
}

void Javelus::add_dsu_klass(InstanceKlass* k, TRAPS) {
  assert(active_dsu() != NULL, "sanity check");
  MutexLocker ml(SystemDictionary_lock, THREAD);
  Symbol* class_name = k->name();
  //  unsigned int d_hash = dictionary()->compute_hash(name_h, class_loader_h);
  //  int d_index = dictionary()->hash_to_index(d_hash);
  dictionary()->add_klass(class_name, k->class_loader_data(), k);
  if (!k->is_linked()) {
    k->set_init_state(InstanceKlass::linked);
  }
}

// After install new classes, we can repair the stack to conform to the new world.
// XXX This method must be called at the end of redefining all classes.
// * We will increment the rn of safe thread
// * All replaced classes have been marked as dead.
void Javelus::repair_application_threads() {
  StackRepairClosure src;
  DoNothinCodeBlobClosure dcbc;

  int sys_from_rn = Javelus::system_revision_number();
  int sys_to_rn = sys_from_rn + 1;

  //bool do_print = true;
  Thread* thread = Thread::current();

  for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
    //fetch the barrier
    intptr_t * barrier = thr->return_barrier_id();
    bool do_update_thread = barrier == NULL;
    int t_from_rn = thr->current_revision();
    // Each time, we can only do one step update.
    int t_to_rn   = t_from_rn + 1;

    if (t_to_rn != sys_to_rn) {
      continue;
    }

    if (thr->has_last_Java_frame()) {
      DSU_TRACE(0x00000080,("Repair thread %s.",thr->get_thread_name()));
      frame * callee = NULL;
      StackFrameStream callee_fst(thr);
      int count = 0;
      for(StackFrameStream fst(thr); !fst.is_done(); fst.next()) {
        frame *current = fst.current();
        if (count > 0) {
          callee = callee_fst.current();
          callee_fst.next();
        }
        count++;
        if (current->is_interpreted_frame()) {
          // update method and constantpoolcache
          Method* method = current->interpreter_frame_method();
          InstanceKlass *ik = method->method_holder();

          DSU_TRACE(0x00000080,(" -j [%s] - [%d,%d)%s%s", method->name_and_sig_as_C_string(), ik->born_rn(), ik->dead_rn(),
                (method->is_native() ? " native" : ""), (method->is_old() ? " old" : "")));

          if (method->is_old()) {
            assert(ik->dead_rn() == sys_to_rn, "sanity check");
          }

          if (ik->dead_rn() == sys_to_rn) {
            // TODO replace loosely restricted method
            assert(!method->is_restricted_method(), "No restricted method here !!");
            assert(method->is_old(), "must be old");

            // XXX We do not know
            Method* new_method = NULL;

            //TODO Currently We on-stack-replace the loosely-stricted method with next matched version.
            InstanceKlass* new_ik = ik->next_version();
            new_method = new_ik->find_method(method->name(), method->signature());

            assert(new_method != NULL,"loosely restricted method must have a new version");
            assert(new_method->is_method(),"must be a method");
            assert(!new_method->is_old(),"new method must not be old");

            assert(!method->is_native(), "should not be native");
            assert(!new_method->is_native(), "should not be native");

            int bci = current->interpreter_frame_bci();
            current->interpreter_frame_set_method(new_method);
            current->interpreter_frame_set_bcp(new_method->bcp_from(bci));
            *(current->interpreter_frame_cache_addr()) = new_method->constants()->cache();

            if (callee != NULL && !method->is_native()) {
              Bytecodes::Code code  = Bytecodes::code_at(method, method->bcp_from(bci));
              // in fact bci may be monitor entry
              assert(bci >= 0, "can invokestatic be the first stmt.");

              if (!(code >= Bytecodes::_invokevirtual && code <= Bytecodes::_invokedynamic)) {
                tty->print_cr("code is %d, bci is  %d", code, bci);
                current->print_value();
                method->print_codes();
              }

              assert(code >= Bytecodes::_invokevirtual && code <= Bytecodes::_invokedynamic, "must be an invoke"  );
              int old_entry_index = Bytes::get_native_u2((address)method->bcp_from(bci) +1);
              int new_entry_index = Bytes::get_native_u2((address)new_method->bcp_from(bci) +1);

              assert(old_entry_index >= 0, "old entry index must be greater than 0");
              assert(new_entry_index >= 0, "new entry index must be greater than 0");

#ifdef ASSERT
              Bytecodes::Code new_code  = Bytecodes::code_at(new_method, new_method->bcp_from(bci));

              if (code != new_code) {
                tty->print_cr("Old code is: %s @%d", Bytecodes::name(code), code);
                tty->print_cr("New code is: %s @%d", Bytecodes::name(new_code), new_code);
                tty->print_cr("bci is: %d", bci);
                method->print();
                new_method->print();
              }

              assert(code == new_code, "new code must be old code");
              if (new_entry_index > new_method->constants()->cache()->length()) {
                tty->print_cr("%d %d", old_entry_index, new_entry_index);
                new_method->print();
                tty->print_cr("end print method");
              }
#endif

              // update callee entry, we only do a partial initialize of the new entry
              assert(new_entry_index < new_method->constants()->cache()->length(), "new entry index must be smaller than cache length");
              ConstantPoolCacheEntry * new_entry = new_method->constants()->cache()->entry_at(new_entry_index);
              if (UseInterpreter && !new_entry->is_resolved(code)) {
                ConstantPoolCache::copy_method_entry(method->constants()->cache(), old_entry_index,
                  new_method->constants()->cache(), new_entry_index);
              }
            }
          }
        } else if (current->is_compiled_frame()) {
          CodeBlob * cb = current->cb();
          if (cb->is_nmethod()) {
            nmethod* nm = (nmethod*)cb;
            Method*  m = nm->method();
            DSU_TRACE(0x00000080,(" -c [%s] )", m->name_and_sig_as_C_string()));
          }
        }
      } // end for frame walk loop

      // At last we can set the rn of the thread to sys_to_rn
      // TODO
      if (do_update_thread) {
        thr->increment_revision();
        assert(thr->current_revision() == sys_to_rn, "Thread must be updated after update stack");
      }
    }
  }
}

void Javelus::transform_object(Handle h, TRAPS) {
  Javelus::transform_object_common(h, CHECK);
}



bool Javelus::transform_object_common_no_lock(Handle stale_object, TRAPS){
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);
  assert(THREAD == Thread::current(), "sanity check");
  JavaThread *thread = (JavaThread*)THREAD;

  bool transformed = false;
  {
    // continue update until the c_dead_rn == t_crn
    while (true) {
      Klass* klass = stale_object->klass();
      assert(klass != NULL, "klass must not be null");
      assert(klass->oop_is_instance(), "we only transform instance objects.");

      InstanceKlass* stale_klass = InstanceKlass::cast(klass);

      // DSU may happen during run custom transformer. so here, we reread the crn of thread.
      int c_dead_rn   = stale_klass->dead_rn();
      int t_crn = thread->current_revision();

      if (c_dead_rn > t_crn) {
        break;
      }

      assert(stale_klass->is_stale_class(), "invalid klass must not be invalid");

      if (stale_klass->is_stale_class()) {
        // This is a pattern of Temp Version
        // We will fix it later.
        if (stale_klass->next_version() == NULL) {
          // We enter here from an custom transformer method.
          // see post_fix_stale_new_class
          assert(stale_klass->previous_version() != NULL, "This may be an temp version and must have previous version or this is an object of a deleted class");
          assert(stale_klass->previous_version()->stale_new_class() == stale_klass, "invariant");
          return true;
        }

        // 5 types;
        // 1). simple => mixed
        // 2). simple => simple
        // 3). mixed => simple
        // 4). mixed => mixed (Reallocate a new phantom object)
        // 5). mixed => mixed (Relink a new phantom object)
        if (stale_object->mark()->is_mixed_object()) { // mixed to *
          // stale_klass is an inplace new class
          Handle old_phantom_object (THREAD, (oop)stale_object->mark()->decode_phantom_object_pointer());
          InstanceKlass* old_inplace_klass = stale_klass;
          InstanceKlass* old_phantom_klass = stale_klass->next_version(); // invariant: the next version of an inplace klass is the corresponding new klass.

          assert(old_inplace_klass->is_inplace_new_class(), "invariant");
          assert(old_phantom_klass == old_phantom_object->klass(), "invariant");

          InstanceKlass* new_inplace_klass = old_phantom_klass->new_inplace_new_class();
          InstanceKlass* new_phantom_klass = old_phantom_klass->next_version();

          Javelus::check_class_initialized(thread, new_phantom_klass);

          if (old_phantom_klass->should_only_replace_klass()) { // mixed to replace klass
            // A special case when we only add instance methods or static members to the class.
            DSU_TRACE(0x00001000,("Transforming Object: [mixed2replaceklass] [%s]  [%d : %d]", stale_klass->name()->as_C_string(), c_dead_rn, t_crn));
            stale_object->set_klass(new_inplace_klass);
          } else if (new_inplace_klass != NULL) { // mixed to mixed with new phantom object
            // We should create a pair of MixObjects.
            // 1). create MixNewObject in the old MixNewObject
            // 2). create a new MixNewObject
            if (new_phantom_klass->size_helper() > old_phantom_klass->size_helper() ) { // mixed to mixed reallocated
              DSU_TRACE(0x00001000,("Transforming Object: [mixed2mixed_reallocate] [%s]  [%d : %d]", stale_klass->name()->as_C_string(), c_dead_rn, t_crn));

              // we should allocate a new phantom object and relink it with the inplace object.
              run_transformer(
                stale_object,       /* inplace object     */
                old_phantom_object, /* old phantom object  */
                old_phantom_object, /* old inplace klass   */
                old_phantom_klass,  /* old (phantom) klass */
                new_inplace_klass,  /* new inplace klass   */
                new_phantom_klass,  /* new (phantom) klass */
                THREAD);
            } else {
              DSU_TRACE(0x00001000,("Transforming Object: [mixed2mixed_relink] [%s]  [%d : %d]", stale_klass->name()->as_C_string(), c_dead_rn, t_crn));

              // we could re-allocate new MixNewObject in the old MixNewObject.
              // We need create an MixObject Here.
              oop new_object = new_phantom_klass->allocate_instance(CHECK_false);
              Handle new_phantom_object (THREAD, new_object);

              assert(((intptr_t)((address)new_phantom_object()) & 0x07) == 0, "must be 8 byte alignment");
              run_transformer(
                stale_object,
                old_phantom_object,
                new_phantom_object,
                old_phantom_klass,
                new_phantom_klass,
                new_phantom_klass,
                CHECK_false);
            }

            assert(stale_object->mark()->is_mixed_object(),"obj must be mixed_object after update");
          } else { // mixed to simple
            DSU_TRACE(0x00001000,("Transforming Object: [mixed_objects2simple] [%s]  [%d : %d]",stale_klass->name()->as_C_string(),c_dead_rn,t_crn));
            // MixObjects will be unlinked.
            run_transformer(
              stale_object,
              old_phantom_object,
              stale_object,
              old_inplace_klass,
              new_phantom_klass,
              new_phantom_klass,
              CHECK_false);
          }
        } else { // simple to *
          // invalid ikh is old class
          InstanceKlass* old_phantom_klass = stale_klass;
          assert(stale_klass->next_version() != NULL, "invalid class must have a next version.");

          InstanceKlass* new_inplace_klass = stale_klass->new_inplace_new_class();
          InstanceKlass* new_phantom_klass = stale_klass->next_version();

          Javelus::check_class_initialized(thread, new_phantom_klass);

          if (old_phantom_klass->should_only_replace_klass()) {
            // A special case when we only add instance methods or static members to the class.
            DSU_TRACE(0x00001000,("Transforming Object: only replace new klass, [%s]  [%d : %d]", stale_klass->name()->as_C_string(), c_dead_rn, t_crn));
            stale_object->set_klass(new_phantom_klass);
          } else if (new_inplace_klass != NULL) { // simple to mixed
            DSU_TRACE(0x00001000,("Transforming Object: [simple2mixed] [%s]  [%d : %d]", stale_klass->name()->as_C_string(), c_dead_rn, t_crn));

            // We need create a phantom object here.
            oop new_object = new_phantom_klass->allocate_instance(CHECK_false);
            assert(((intptr_t)((address)new_object) & 0x07) == 0, "must be 8 byte alignment");
            Handle new_phantom_object(THREAD, new_object);

            run_transformer(
              stale_object,
              stale_object,
              new_phantom_object,
              old_phantom_klass,
              new_inplace_klass,
              new_phantom_klass,
              CHECK_false);
            assert(stale_object->mark()->is_mixed_object(), "sanity check");
          } else { // simple to simple
            DSU_TRACE(0x00001000,("Transforming Object: [simple2simple] [%s] ["PTR_FORMAT"] [%d : %d]",
                  stale_klass->name()->as_C_string(), p2i(stale_object()), c_dead_rn, t_crn));
            run_transformer(
              stale_object,
              stale_object,
              stale_object,
              old_phantom_klass,
              new_phantom_klass,
              new_phantom_klass,
              CHECK_false);
          }
        }

        transformed = true;
      } else {
        assert(c_dead_rn == t_crn, "the class must be valid at this thread.");
      }
    } // end of while
  }// end of block
  assert(stale_object->klass() != NULL, "sanity check");
  return transformed;
}

bool Javelus::transform_object_common(Handle obj, TRAPS) {
  HandleMark hm(THREAD);
  ResourceMark rm(THREAD);

  Klass* klass = obj->klass();

  assert(klass != NULL, "klass must not be null");
  assert(obj->is_instance(), "we only transform instance objects.");
  assert(klass->oop_is_instance(), "we only transform instance objects.");

  InstanceKlass* stale_klass = InstanceKlass::cast(klass);
  if (!stale_klass->is_stale_class()) {
    return false;
  }

  int c_dead_rn = stale_klass->dead_rn();
  JavaThread *thread = (JavaThread*)THREAD;//JavaThread::current();
  int t_crn = thread->current_revision();

  if (c_dead_rn > t_crn) {
    return false;
  }

  bool transformed = false;
  // Any other threads will be blocked here.
  //Handle lock = (THREAD, Javelus::developer_interface_klass());
  if (DSUEagerUpdate_lock->owner() == thread) {
    transformed = transform_object_common_no_lock(obj, THREAD);
  } else {
    Handle lock (THREAD, stale_klass->java_mirror());
    ObjectLocker locker (lock, THREAD);
    transformed = transform_object_common_no_lock(obj, THREAD);
  }

  assert(!transformed || !obj->klass()->is_stale_class(), "sanity check");

  if (HAS_PENDING_EXCEPTION) {
    DSU_WARN(("transform object results in an exception"));
    return false;
  }

  return transformed;
}

void Javelus::link_mixed_object(Handle inplace_object, Handle phantom_object, TRAPS){
  markOop new_mark = markOopDesc::encode_phantom_object_pointer_as_mark(phantom_object());
  markOop old_mark = inplace_object->mark();
  // MixNewObject is now can only be seen by the lazy object update routine.
  // We can set the mark freely.
  // Am I right?
  // In case we update a dead object, we should clear its mark
  if (old_mark->must_be_preserved(phantom_object())) {
    phantom_object->set_mark(phantom_object()->klass()->prototype_header());
  } else {
    phantom_object->set_mark(old_mark);
  }
  // A CAS op to install old mark with Mix
  if (Atomic::cmpxchg_ptr(new_mark, inplace_object->mark_addr(), old_mark) != old_mark) {
    link_mixed_object_slow(inplace_object, phantom_object, THREAD);
  }
}

void Javelus::unlink_mixed_object(Handle inplace_object, Handle old_phantom_object, TRAPS){
  VM_UnlinkMixedObject um(&inplace_object);
  VMThread::execute(&um);
}

// We must inflate the old MixNewObject or use a VMOperation to relink MixObjects.
void Javelus::relink_mixed_object(Handle inplace_object, Handle old_phantom_object, Handle new_phantom_object, TRAPS){
  VM_RelinkMixedObject rm(&inplace_object, &new_phantom_object);
  VMThread::execute(&rm);
}

void Javelus::link_mixed_object_slow(Handle inplace_object, Handle new_phantom_object, TRAPS){
  markOop new_mark = markOopDesc::encode_phantom_object_pointer_as_mark(new_phantom_object());
  markOop old_mark = inplace_object->mark();

  if (UseBiasedLocking) {
    assert((!SafepointSynchronize::is_at_safepoint()), "no safepoint");
    if(old_mark->has_bias_pattern()){
      BiasedLocking::revoke_and_rebias(inplace_object, false, THREAD);
    }
  }

  // XXX
  // Mixed-object is a stable state of head
  // Only install Mixed-Objects from another stable state INFLATED to MIXED-OBJECTS
  // Now Object is in INFLATED
  ObjectSynchronizer::inflate(THREAD, inplace_object());

  int retry_count = 10;
  old_mark = inplace_object->mark();
  new_phantom_object->set_mark(old_mark);
  while (Atomic::cmpxchg_ptr(new_mark, inplace_object->mark_addr(), old_mark) != old_mark) {
    old_mark = inplace_object->mark();
    new_phantom_object->set_mark(old_mark);
    retry_count--;
    if(retry_count <= 0){
      DSU_WARN(("Linking mixed object failed."));
      break;
    }
  }
}

void collect_transformer_arguments(JavaCallArguments &args, Handle inplace_object, Handle phantom_object, Array<u1>* transformer_args, TRAPS) {
  args.push_oop(inplace_object);
  if (transformer_args != NULL) {
    int length = transformer_args->length();
    for (int i = 0; i < length; i+=DSUClass::next_transformer_arg) {
      u4 offset = build_u4_from(transformer_args->adr_at(i + DSUClass::transformer_field_offset));
      BasicType type = (BasicType) transformer_args->at(i + DSUClass::transformer_field_type);
      u1 flag = transformer_args->at(i + DSUClass::transformer_field_flag);
      //Field may be in the phantom object, but this object is not a mixed object.
      if ((flag & 0x01) != 0) {
        Javelus::read_arg(phantom_object, offset, type, args, THREAD);
      } else {
        Javelus::read_arg(inplace_object, offset, type, args, THREAD);
      }
    }
  }
}

void run_default_transformer(Handle inplace_object, Handle old_phantom_object, Handle new_phantom_object,
     InstanceKlass* old_phantom_klass, InstanceKlass* new_phantom_klass, TRAPS) {
  Array<u1>* matched_fields = new_phantom_klass->matched_fields();

  const int old_inplace_size_in_bytes = InstanceKlass::cast(inplace_object->klass())->size_helper() << LogBytesPerWord;
  const int old_phantom_size_in_bytes = old_phantom_klass->size_helper() << LogBytesPerWord;
  const int new_phantom_size_in_bytes = new_phantom_klass->size_helper() << LogBytesPerWord;

  bool from_simple  = inplace_object == old_phantom_object;
  bool to_simple    = inplace_object == new_phantom_object;
  bool unlink_mixed = !from_simple && to_simple;
  bool link         = from_simple && !to_simple;
  bool relink       = !from_simple && !to_simple && old_phantom_object() != new_phantom_object();
  bool reallocate_inplace = to_simple && new_phantom_size_in_bytes < old_inplace_size_in_bytes;
  bool reallocate_phantom = !from_simple && !to_simple && old_phantom_object == new_phantom_object && new_phantom_size_in_bytes < old_phantom_size_in_bytes;

  if (matched_fields == NULL) {
    {
      // This should be done atomic if we have a concurrent gc.
      // The thread should be in VM or at safe point.
      assert(old_phantom_klass->stale_new_class() != NULL, "sanity check");
      inplace_object->set_klass(old_phantom_klass->stale_new_class());
      if (link) {
        Javelus::link_mixed_object(inplace_object, new_phantom_object, CHECK);
        assert(inplace_object->mark()->is_mixed_object(), "sanity check");
      } else if (relink) {
        Javelus::relink_mixed_object(inplace_object, old_phantom_object, new_phantom_object, CHECK);
      } else if (unlink_mixed) {
        Javelus::unlink_mixed_object(inplace_object, old_phantom_object, CHECK);
      }
      // now the object may is a simple object or a mixed object with empty fields.
    }
    return;
  }

  ResourceMark rm(THREAD);
  char *prototype_c = NEW_RESOURCE_ARRAY(char, new_phantom_size_in_bytes);

  // Allocate an clean oop
  memset(prototype_c, 0, new_phantom_size_in_bytes);
  oop prototype_oop = (oop) prototype_c;
  prototype_oop->set_mark(new_phantom_object->mark());
  assert(old_phantom_klass->stale_new_class() != NULL, "sanity check");

  // prototype object is in the new type
  prototype_oop->set_klass(old_phantom_klass->next_version());

  int length = matched_fields->length();
  //2.1). first pass copy match fields to prototype (include fields declared in super class)
  for (int i = 0; i < length; i += DSUClass::next_matched_field){
    u4 o_offset   = build_u4_from(matched_fields->adr_at(i + DSUClass::matched_field_old_offset));
    u4 n_offset   = build_u4_from(matched_fields->adr_at(i + DSUClass::matched_field_new_offset));
    BasicType type = (BasicType) matched_fields->at(i + DSUClass::matched_field_type);
    u1 flags = matched_fields->at(i + DSUClass::matched_field_flags);
    if ((flags & 0x04) != 0) {
      // after all before fields are copied, we can clear memory (inplace_object_size - ycsc_object_size)
      memset(((char*)((address)inplace_object())) + o_offset, 0, (int)n_offset);
    } else if ((flags & 0x01) != 0) {
      Javelus::copy_field(old_phantom_object(), prototype_oop, o_offset, n_offset, type);
    } else {
      Javelus::copy_field(inplace_object(), prototype_oop, o_offset, n_offset, type);
    }
  }

  // Before copy back values, we should make the object as an "half-valid" new obj
  // - set the stale new klass.
  {
    // This should be done atomic if we have a concurrent gc.
    // The thread should be in VM or at safe point.
    assert(old_phantom_klass->stale_new_class() != NULL, "sanity check");
    inplace_object->set_klass(old_phantom_klass->stale_new_class());
    if (link) {
      Javelus::link_mixed_object(inplace_object, new_phantom_object, CHECK);
      assert(inplace_object->mark()->is_mixed_object(), "sanity check");
    } else if (relink) {
      Javelus::relink_mixed_object(inplace_object, old_phantom_object, new_phantom_object, CHECK);
    } else if (unlink_mixed) {
      Javelus::unlink_mixed_object(inplace_object, old_phantom_object, CHECK);
    } else {
    }
    // now the object may is a simple object or a mixed object with empty fields.
  }

  if (reallocate_inplace) {
    Javelus::realloc_decreased_object(inplace_object, old_inplace_size_in_bytes, new_phantom_size_in_bytes);
  }

  if (reallocate_phantom) {
    assert(old_phantom_object == new_phantom_object, "sanity check");
    Javelus::realloc_decreased_object(old_phantom_object, old_phantom_size_in_bytes, new_phantom_size_in_bytes);
  }

  //2.2). second pass copy all fields from prototype to new object
  for (int i = 0; i < length; i += DSUClass::next_matched_field) {
    u4 o_offset   = build_u4_from(matched_fields->adr_at(i + DSUClass::matched_field_old_offset));
    u4 n_offset   = build_u4_from(matched_fields->adr_at(i + DSUClass::matched_field_new_offset));
    BasicType type = (BasicType) matched_fields->at(i + DSUClass::matched_field_type);
    u1 flags = matched_fields->at(i + DSUClass::matched_field_flags);
    if ((flags & 0x04) != 0) {
      continue;
    } else if ((flags & 0x02) != 0) { // new field is in the mixed object
      Javelus::copy_field(prototype_oop, new_phantom_object(), n_offset, n_offset, type);
    } else {
      Javelus::copy_field(prototype_oop, inplace_object(), n_offset, n_offset, type);
    }
  }
}

void run_custom_transformer(Handle inplace_object, InstanceKlass* old_phantom_klass, Method* object_transformer, JavaCallArguments &args, TRAPS) {
  if (object_transformer != NULL) {
    inplace_object->set_klass(old_phantom_klass->stale_new_class());
    assert(old_phantom_klass->stale_new_class() != NULL, "temp version must be not NULL");
    assert(old_phantom_klass->stale_new_class() == inplace_object->klass(), "klass must be set up with temp version");

    Handle pending_exception;
    JavaValue result(T_VOID);
    JavaCalls::call(&result, object_transformer, &args, THREAD);
    if (HAS_PENDING_EXCEPTION) {
      pending_exception = Handle(THREAD, PENDING_EXCEPTION);
      DSU_WARN(("Run object transformer has exceptions"));
      pending_exception->print();
      CLEAR_PENDING_EXCEPTION;
    }
  }
}

void Javelus::run_transformer(Handle inplace_object, Handle old_phantom_object, Handle new_phantom_object,
  InstanceKlass* old_phantom_klass, InstanceKlass* new_inplace_klass,
  InstanceKlass* new_phantom_klass, TRAPS) {

  assert(old_phantom_klass->is_stale_class(), "only invalid class can be transformed!");
  assert(inplace_object.not_null(), "sanity check!");
  assert(old_phantom_object.not_null(), "sanity check");
  assert(new_phantom_object.not_null(), "sanity check");
  assert(old_phantom_klass != NULL, "sanity check!");
  assert(new_inplace_klass != NULL, "sanity check!");
  assert(!new_inplace_klass->is_stale_class(), "sanity check!");
  assert(new_phantom_klass != NULL, "next instanceKlass must not be null");

  Method* object_transformer = new_phantom_klass->object_transformer();
  Array<u1>* transformer_args = new_phantom_klass->object_transformer_args();

  // 1). save old fields
  int size = transformer_args == NULL ? 8 : transformer_args->length()/DSUClass::next_transformer_arg;
  JavaCallArguments args(size);
  if (object_transformer != NULL) {
    collect_transformer_arguments(args, inplace_object, inplace_object, transformer_args, CHECK);
  }

  run_default_transformer(inplace_object, inplace_object, new_phantom_object, old_phantom_klass, new_phantom_klass, CHECK);
  run_custom_transformer(inplace_object, old_phantom_klass, object_transformer, args, CHECK);

  inplace_object->set_klass(new_inplace_klass);
}

oopDesc* Javelus::merge_mixed_object(oopDesc* inplace_object) {
  return merge_mixed_object(inplace_object, (oopDesc*)inplace_object->mark()->decode_phantom_object_pointer());
}

oopDesc* Javelus::merge_mixed_object(oopDesc* inplace_object, oopDesc* phantom_object) {
  // iterate all fields if super contains mixfields
  // do memcpy
  // this will only happens at safepoint
  assert(inplace_object->is_instance(),"must be instance");

  InstanceKlass * ik = InstanceKlass::cast(phantom_object->klass());
  Array<u1>*  inplace_fields = ik->inplace_fields();
  if (inplace_fields != NULL) {
    char * src = (char*)inplace_object;
    char * dst = (char*)phantom_object;
    int inplace_fields_length = inplace_fields->length();
    for(int i = 0; i < inplace_fields_length; i += DSUClass::next_inplace_field){
      u4 offset = build_u4_from(inplace_fields->adr_at(i + DSUClass::inplace_field_offset));
      u4 length = build_u4_from(inplace_fields->adr_at(i + DSUClass::inplace_field_length));
      assert(offset >= (u4)(instanceOopDesc::header_size() << LogHeapWordSize), "we do not copy header");
      memcpy(dst + offset, src + offset, length);
    }
  }

  return phantom_object;
}



void Javelus::repatch_method(Method* method,bool print_replace, TRAPS) {
  // We cannot tolerate a GC in this block, because we've
  // cached the bytecodes in 'code_base'. If the methodOop
  // moves, the bytecodes will also move.
  No_Safepoint_Verifier nsv;
  Bytecodes::Code c;
  Bytecodes::Code java_c;
  // Bytecodes and their length
  const address code_base = method->code_base();
  const int code_length = method->code_size();

  ResourceMark rm(THREAD);

  if (print_replace) {
    DSU_TRACE_MESG(("Re-patch VM bytecode to Java byteocode of method %s.", method->name_and_sig_as_C_string()));
  }

  int bc_length;
  for (int bci = 0; bci < code_length; bci += bc_length) {
    address bcp = code_base + bci;

    c = (Bytecodes::Code)(*bcp);
    java_c = Bytecodes::java_code(c);

    switch(c) {
    case     Bytecodes::_fast_agetfield:
    case     Bytecodes::_fast_bgetfield:
    case     Bytecodes::_fast_cgetfield:
    case     Bytecodes::_fast_dgetfield:
    case     Bytecodes::_fast_fgetfield:
    case     Bytecodes::_fast_igetfield:
    case     Bytecodes::_fast_lgetfield:
    case     Bytecodes::_fast_sgetfield:

    case     Bytecodes::_fast_aputfield:
    case     Bytecodes::_fast_bputfield:
    case     Bytecodes::_fast_cputfield:
    case     Bytecodes::_fast_dputfield:
    case     Bytecodes::_fast_fputfield:
    case     Bytecodes::_fast_iputfield:
    case     Bytecodes::_fast_lputfield:
    case     Bytecodes::_fast_sputfield:

    case     Bytecodes::_fast_aload_0  :
    case     Bytecodes::_fast_iaccess_0:
    case     Bytecodes::_fast_aaccess_0:
    case     Bytecodes::_fast_faccess_0:

    case     Bytecodes::_fast_iload    :
    case     Bytecodes::_fast_iload2   :
    case     Bytecodes::_fast_icaload  :
      {
        if (print_replace) {
          tty->print_cr(" - replace %s with %s at %d, bc length = %d",Bytecodes::name(c),Bytecodes::name(java_c),bci, bc_length);
        }
        (*bcp) = java_c;
        break;
      }
    }

    // Since we have the code, see if we can get the length
    // directly. Some more complicated bytecodes will report
    // a length of zero, meaning we need to make another method
    // call to calculate the length.
    bc_length = Bytecodes::length_for(java_c);
    if (bc_length == 0) {
      bc_length = Bytecodes::length_at(method, bcp);

      // length_at will put us at the bytecode after the one modified
      // by 'wide'. We don't currently examine any of the bytecodes
      // modified by wide, but in case we do in the future...
      if (c == Bytecodes::_wide) {
        c = (Bytecodes::Code)bcp[1];
      }
    }

    assert(bc_length != 0, "impossible bytecode length");
  }
}


void Javelus::dsu_thread_loop() {
  DSUThread * thread = DSUThread::current();

  while(true) {
    ResourceMark rm;
    DSUTask * task = thread->next_task();

    if (task == NULL) {
      DSU_WARN(("Error in fetching task!!"));
      continue;
    }

    while(true) {

      // any time ,there is only one VM_DSUOperation in processing.
      // So any access to the static fields of class DSU from VM_DSUOperation
      // has no need of any synchronization.

      VM_DSUOperation * op = task->operation();
      VMThread::execute(op);

      if (op->is_finished()) {
        // Request is finished..
        DSU_INFO(("DSU Request is finished"));
        Javelus::notifyRequest();
        break;
      } else if (op->is_empty()) {
        DSU_WARN(("DSU Request has no updated class"));
        Javelus::notifyRequest();
        break;
       } else if (op->is_discarded()) {
        // Request is discarded..
        DSU_WARN(("DSU Request is discarded"));
        Javelus::discard_active_dsu();
        Javelus::notifyRequest();
        break;
      } else if (op->is_system_modified()) {
        // make a retry
        DSU_WARN(("DSU Request will be retried immediately."));
      } else if (op->is_interrupted()) {
        DSU_WARN(("DSU Request is interrupted"));
        // wait until current task finished or discarded
        // The op being executed will notify me.
        // TODO timeout doest not implement yet!!!
        thread->sleep(1000);
      } else {
        DSU_WARN(("Unexpected result for DSU Request."));
        Javelus::discard_active_dsu();
        Javelus::notifyRequest();
        break;
      }

   }

    delete task;
  }
}


// --------------------------- Synchronization Support -----------------------------


void Javelus::wakeup_DSU_thread() {
  Javelus::get_dsu_thread()->wakeup();
}


void Javelus::waitRequest() {
  MutexLocker locker(DSURequest_lock);
  DSURequest_lock->wait();
}


void Javelus::waitRequest(long time) {
  MutexLocker locker(DSURequest_lock);
  DSURequest_lock->wait(true, time);
}

void Javelus::notifyRequest() {
  MutexLocker locker(DSURequest_lock);
  DSURequest_lock->notify_all();
}




// -------------------------- End of Synchronization ------------------------------

DSUThread* Javelus::make_dsu_thread(const char * name) {
  EXCEPTION_MARK;
  DSUThread* dsu_thread = NULL;

  Klass* k = SystemDictionary::resolve_or_fail(vmSymbols::java_lang_Thread(), true, CHECK_0);
  InstanceKlass* klass = InstanceKlass::cast(k);
  instanceHandle thread_oop = klass->allocate_instance_handle(CHECK_0);
  Handle string = java_lang_String::create_from_str(name, CHECK_0);

  // Initialize thread_oop to put it into the system threadGroup
  Handle thread_group (THREAD,  Universe::system_thread_group());
  JavaValue result(T_VOID);
  JavaCalls::call_special(&result, thread_oop,
    klass,
    vmSymbols::object_initializer_name(),
    vmSymbols::threadgroup_string_void_signature(),
    thread_group,
    string,
    CHECK_0);

  {
    MutexLocker mu(Threads_lock, THREAD);
    dsu_thread = new DSUThread();
    // At this point the new CompilerThread data-races with this startup
    // thread (which I believe is the primoridal thread and NOT the VM
    // thread).  This means Java bytecodes being executed at startup can
    // queue compile jobs which will run at whatever default priority the
    // newly created CompilerThread runs at.


    // At this point it may be possible that no osthread was created for the
    // JavaThread due to lack of memory. We would have to throw an exception
    // in that case. However, since this must work and we do not allow
    // exceptions anyway, check and abort if this fails.

    if (dsu_thread == NULL || dsu_thread->osthread() == NULL) {
      vm_exit_during_initialization("java.lang.OutOfMemoryError",
        "unable to create new native thread");
    }

    java_lang_Thread::set_thread(thread_oop(), dsu_thread);

    // Note that this only sets the JavaThread _priority field, which by
    // definition is limited to Java priorities and not OS priorities.
    // The os-priority is set in the CompilerThread startup code itself
    java_lang_Thread::set_priority(thread_oop(), NearMaxPriority);
    // CLEANUP PRIORITIES: This -if- statement hids a bug whereby the compiler
    // threads never have their OS priority set.  The assumption here is to
    // enable the Performance group to do flag tuning, figure out a suitable
    // CompilerThreadPriority, and then remove this 'if' statement (and
    // comment) and unconditionally set the priority.

    // Compiler Threads should be at the highest Priority
    //    if ( CompilerThreadPriority != -1 )
    //      os::set_native_priority( dsu_thread, CompilerThreadPriority );
    //    else
    os::set_native_priority( dsu_thread, os::java_to_os_priority[NearMaxPriority]);

    // Note that I cannot call os::set_priority because it expects Java
    // priorities and I am *explicitly* using OS priorities so that it's
    // possible to set the compiler thread priority higher than any Java
    // thread.

    java_lang_Thread::set_daemon(thread_oop());

    dsu_thread->set_threadObj(thread_oop());
    Threads::add(dsu_thread);
    Thread::start(dsu_thread);
  }
  // Let go of Threads_lock before yielding
  os::yield(); // make sure that the compiler thread is started early (especially helpful on SOLARIS)

  return dsu_thread;
}


void Javelus::copy_fields(oop src, oop dst, InstanceKlass* ik) {
  FieldInfo* field_info = NULL;
  int new_field_count = ik->java_fields_count();
  ConstantPool* constants = ik->constants();
  for (int i = 0; i < new_field_count; i++) {
    field_info = ik->field(i);
    int i_flags  = field_info->access_flags();
    int i_dsu_flags  = 0;

    if ((i_flags & JVM_ACC_STATIC) != 0) {
      // skip static field
      continue;
    }

    u4 offset   = field_info->offset();
    Symbol* s_sig = field_info->signature(ik->constants());

    BasicType type = FieldType::basic_type(s_sig);
    assert((i_dsu_flags & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) == 0, "No field in the phantom object here.");

    switch(type) {
    case T_BOOLEAN:
      dst->bool_field_put(offset, src->bool_field(offset));
      break;
    case T_BYTE:
      dst->byte_field_put(offset, src->byte_field(offset));
      break;
    case T_SHORT:
      dst->short_field_put(offset, src->short_field(offset));
      break;
    case T_CHAR:
      dst->char_field_put(offset, src->char_field(offset));
      break;
    case T_OBJECT:
    case T_ARRAY:
      dst->obj_field_put(offset, src->obj_field(offset));
      break;
    case T_INT:
      dst->int_field_put(offset, src->int_field(offset));
      break;
    case T_LONG:
      dst->long_field_put(offset, src->long_field(offset));
      break;
    case T_FLOAT:
      dst->float_field_put(offset, src->float_field(offset));
      break;
    case T_DOUBLE:
      dst->double_field_put(offset, src->double_field(offset));
      break;
    case T_VOID:
      break;
    default:
      ShouldNotReachHere();
    }
  }
}



void Javelus::copy_fields(oop src, oop dst, oop mix_dst, InstanceKlass* ik) {
  FieldInfo* field_info = NULL;
  int new_field_count = ik->java_fields_count();
  ConstantPool* constants = ik->constants();
  for (int i = 0; i < new_field_count; i++) {
    field_info = ik->field(i);
    int i_flags  = field_info->access_flags();
    int i_dsu_flags  = field_info->dsu_flags();

    if ((i_flags & JVM_ACC_STATIC) != 0) {
      // skip static field
      continue;
    }

    u4 offset   = field_info->offset();
    Symbol* s_sig = field_info->signature(ik->constants());

    BasicType type = FieldType::basic_type(s_sig);
    if ((i_dsu_flags & DSU_FLAGS_MEMBER_NEEDS_MIXED_OBJECT_CHECK) == 0) {
      Javelus::copy_field(src, dst, offset, offset, type);
    } else {
      Javelus::copy_field(src, mix_dst, offset, offset, type);
    }
  }
}


void Javelus::realloc_decreased_object(Handle obj, int old_size_in_bytes, int new_size_in_bytes) {
  assert(obj->is_instance(),"we can only update instance objects.");
  int old_size_in_words = old_size_in_bytes >> LogBytesPerWord;
  int new_size_in_words = new_size_in_bytes >> LogBytesPerWord;
  int delta = old_size_in_words - new_size_in_words;

  assert(old_size_in_bytes > new_size_in_bytes, "old size must be larger than new size");
  assert(((old_size_in_bytes - new_size_in_bytes) % 8) == 0, "delta must be in 8 alignment.");
  assert(delta >= (int)CollectedHeap::min_fill_size(), "sanity check!");

  // old objects address
  HeapWord* hw = (HeapWord*) obj();
  // residual space head
  hw = hw + new_size_in_words;
  // Now hw is pointed to the end of obj.
  CollectedHeap::fill_with_objects(hw, delta);
}

void Javelus::copy_field(oop src, oop dst, int src_offset, int dst_offset, BasicType type) {
  switch(type) {
  case T_BOOLEAN:
    dst->bool_field_put(dst_offset, src->bool_field(src_offset));
    break;
  case T_BYTE:
    dst->byte_field_put(dst_offset, src->byte_field(src_offset));
    break;
  case T_SHORT:
    dst->short_field_put(dst_offset, src->short_field(src_offset));
    break;
  case T_CHAR:
    dst->char_field_put(dst_offset, src->char_field(src_offset));
    break;
  case T_OBJECT:
  case T_ARRAY:
    if (Universe::heap()->is_in(dst)) {
      dst->obj_field_put(dst_offset, src->obj_field(src_offset));
    } else {
      dst->obj_field_put_raw(dst_offset, src->obj_field(src_offset));
    }
    break;
  case T_INT:
    dst->int_field_put(dst_offset, src->int_field(src_offset));
    break;
  case T_LONG:
    dst->long_field_put(dst_offset, src->long_field(src_offset));
    break;
  case T_FLOAT:
    dst->float_field_put(dst_offset, src->float_field(src_offset));
    break;
  case T_DOUBLE:
    dst->double_field_put(dst_offset, src->double_field(src_offset));
    break;
  case T_VOID:
    break;
  default:
    ShouldNotReachHere();
  }
}


void Javelus::read_value(Handle obj, int offset, BasicType type, JavaValue &result,TRAPS) {
  result.set_type(type);
  switch(type) {
  case T_BOOLEAN:
    result.set_jint(obj->bool_field(offset));
    break;
  case T_BYTE:
    result.set_jint(obj->byte_field(offset));
    break;
  case T_SHORT:
    result.set_jint(obj->short_field(offset));
    break;
  case T_CHAR:
    result.set_jint(obj->char_field(offset));
    break;
  case T_OBJECT:
  case T_ARRAY:
    result.set_jobject(JNIHandles::make_local(THREAD, obj->obj_field(offset)));
    break;
  case T_INT:
    result.set_jint(obj->int_field(offset));
    break;
  case T_LONG:
    result.set_jlong(obj->long_field(offset));
    break;
  case T_FLOAT:
    result.set_jfloat(obj->float_field(offset));
    break;
  case T_DOUBLE:
    result.set_jdouble(obj->double_field(offset));
    break;
  case T_VOID:
    break;
  default:
    ShouldNotReachHere();
  }
}

void Javelus::read_arg(Handle obj, int offset, BasicType type, JavaCallArguments &args, TRAPS) {
  switch(type) {
  case T_BOOLEAN:
    args.push_int(obj->bool_field(offset));
    break;
  case T_BYTE:
    args.push_int(obj->byte_field(offset));
    break;
  case T_SHORT:
    args.push_int(obj->short_field(offset));
    break;
  case T_CHAR:
    args.push_int(obj->char_field(offset));
    break;
  case T_OBJECT:
  case T_ARRAY:
    args.push_oop(Handle(THREAD, obj->obj_field(offset)));
    break;
  case T_INT:
    args.push_int(obj->int_field(offset));
    break;
  case T_LONG:
    args.push_long(obj->long_field(offset));
    break;
  case T_FLOAT:
    args.push_float(obj->float_field(offset));
    break;
  case T_DOUBLE:
    args.push_double(obj->double_field(offset));
    break;
  case T_VOID:
    break;
  default:
    ShouldNotReachHere();
  }
}

void Javelus::parse_old_field_annotation(InstanceKlass* the_class,
  InstanceKlass* transformer, Array<u1>* annotations, Array<u1>* &result, TRAPS) {
    if (annotations == NULL) {
      return;
    }

    int length = annotations->length();
    // The RuntimeVisibleParameterAnnotations attribute has the following format:
    //   RuntimeVisibleParameterAnnotations_attribute {
    //     u2 attribute_name_index;
    //     u4 attribute_length;
    //     u1 num_parameters;
    //     { u2 num_annotations;
    //       annotation annotations[num_annotations];
    //     } parameter_annotations[num_parameters];
    //   }

    enum{
      attribute_name_length = 2,
      attribute_length_length = 4,
      num_parameters_length = 1,
      num_annotations_length = 2,
      type_index_length = 2,
      num_element_value_pairs_length = 2,
      element_name_index_length = 2,
      tag_length = 1,
      const_value_index_length = 2,
    };

    enum{
      num_parameters_index = 0,
      first_parameter_annotation_index = 1,
    };

    //  annotation {
    //    u2 type_index;
    //    u2 num_element_value_pairs;
    //    { u2 element_name_index;
    //      element_value value;
    //    } element_value_pairs[num_element_value_pairs]
    //  }
    //  element_value {
    //    u1 tag;
    //    union {
    //      u2 const_value_index;
    //      { u2 type_name_index;
    //        u2 const_name_index;
    //      } enum_const_value;
    //      u2 class_info_index;
    //      annotation annotation_value;
    //      { u2 num_values;
    //        element_value values[num_values];
    //      } array_value;
    //    } value;
    //  }
    u1 num_parameters = annotations->at(num_parameters_index);
    assert(num_parameters > 0 ,"must contains the old object at least" );

    u4 attribute_length = annotations->length();

    tty->print_cr("[DSU] Original annotation: ");
    for(int i = 0; i < annotations->length(); i++) {
      tty->print("%0x ", annotations->at(i));
    }
    tty->cr();

    tty->print_cr("[DSU] Parse annotations, number of parameters %d, attribute length is %d", num_parameters, attribute_length);
    if (num_parameters > 1) {
      // set up annotation
      result = MetadataFactory::new_array<u1>(the_class->class_loader_data(), (num_parameters - 1)*DSUClass::next_transformer_arg, THREAD);

      u1 cursor = first_parameter_annotation_index;
      int transformer_field_index = 0;
      // the first parameter annotation

      int num_annotations = Bytes::get_Java_u2(annotations->adr_at(cursor));

      // TODO in template class, the first parameter is the old field.
      // But in transformer class, the first parameter is the changed object.
      // We have to shift the parameter annotations
      attribute_length -= 2;
      while(cursor < attribute_length) {
        num_annotations = Bytes::get_Java_u2(annotations->adr_at(cursor));
        tty->print_cr("[DSU] Parse annotations, cursor is %d, transformer field index is %d, num annotations is %d",
          cursor, transformer_field_index, num_annotations);

        cursor += num_annotations_length;

        assert(num_annotations == 1, "Only support parse OldField annotation");

        enum {
          type_index_length = 2,
          num_element_value_pairs_length = 2,
        };

        int type_index = Bytes::get_Java_u2(annotations->adr_at(cursor));
        cursor += type_index_length;
        //parameter_annotation_length += 2;
        int num_element_value_pairs = Bytes::get_Java_u2(annotations->adr_at(cursor));
        cursor += num_element_value_pairs_length;
        //parameter_annotation_length += 2;

        assert(num_element_value_pairs == 3, "see OldField ");

        enum{
          element_name_index_length = 2,  // 0
          // element value
          tag_length = 1, // 2
          string_const_value_index_length = 2, // 2
        };

        Symbol* holder = NULL;
        Symbol* name = NULL;
        Symbol* signature = NULL;

        for(int i=0;i<3;i++) {
          int element_name_index = Bytes::get_Java_u2(annotations->adr_at(cursor));
          cursor += element_name_index_length;
          u1 tag = annotations->at(cursor);
          cursor += tag_length;
          int string_const_value_index = Bytes::get_Java_u2(annotations->adr_at(cursor));
          cursor += string_const_value_index_length;
          assert(tag == 's', "tag must be s");

          Symbol* element_name = transformer->constants()->symbol_at(element_name_index);
          if (element_name == vmSymbols::clazz_name()) {
            holder = transformer->constants()->symbol_at(string_const_value_index);
          } else if (element_name == vmSymbols::name_name()) {
            name = transformer->constants()->symbol_at(string_const_value_index);
          } else if (element_name == vmSymbols::signature_name()) {
            signature = transformer->constants()->symbol_at(string_const_value_index);
          } else {
            ShouldNotReachHere();
          }
        }

        assert( holder!= NULL && name != NULL && signature != NULL, "the field ID must be full specified");

        InstanceKlass* super = the_class;
        fieldDescriptor fd;
        bool found = false;
        while(super != NULL && super->name() == holder) {
          found = super->find_local_field(name, signature, &fd);
          if (found) {
            break;
          }
          super = super->superklass();
        }
        if (found) {
          u1 type = fd.field_type();
          result->at_put(transformer_field_index + DSUClass::transformer_field_type, type);
          u4 offset = fd.offset();
          Bytes::put_Java_u4(result->adr_at(transformer_field_index + DSUClass::transformer_field_offset), offset);
          if (fd.dsu_flags().needs_mixed_object_check()) {
            // We use negative offset to indicate the field is a MixNewField.
            result->at_put(transformer_field_index + DSUClass::transformer_field_flag, 1);
          } else {
            result->at_put(transformer_field_index + DSUClass::transformer_field_flag, 0);
          }
          transformer_field_index += DSUClass::next_transformer_arg;
        } else {
          ShouldNotReachHere();
        }
      }
    }
}


void Javelus::oops_do(OopClosure* f) {
  if (dictionary() != NULL) {
    dictionary()->oops_do(f);
  }
}


//No synchronization here.
//We only process one DSU request at a time.
void Javelus::increment_system_rn() {
  if (_latest_rn >= Javelus::MAX_REVISION_NUMBER) {
    tty->print_cr("Revision Number Overflow!!!");
  }
  _latest_rn ++;
}

int Javelus::system_revision_number() {
  return _latest_rn;
}

void Javelus::install_dsu(DSU * dsu) {
  // current we only can make active DSU unactive
  assert(dsu == active_dsu(), "sanity");

  dsu->set_next(NULL);

  if (_first_dsu == NULL) {
    assert(_last_dsu == NULL, "sanity");
    _first_dsu = _last_dsu = dsu;
    return ;
  }

  _last_dsu->set_next(dsu);
  _last_dsu = _last_dsu->next();
}

bool Javelus::validate_DSU(DSU *dsu ) {
  if (dsu == NULL) {
    return false;
  }

  DSUClassLoader *dsu_loader = dsu->first_class_loader();
  for (; dsu_loader != NULL; dsu_loader = dsu_loader->next() ) {
    DSUClass* dsu_class = dsu_loader->first_class();
    for(;dsu_class!=NULL;dsu_class = dsu_class->next()) {
      if (dsu_class== NULL) {
        return false;
      }

      if (dsu_class->old_version_class() == NULL && dsu_class->updating_type() != DSU_CLASS_STUB) {
        DSU_TRACE_MESG(("Validate DSU: has no old class to update"));
        return false;
      }

      /*if (dsu_class->methods_count() > 0 && dsu_class->methods() == NULL) {
        tty->print_cr("Validate DSU: methods table is null");
        return false;
      }*/

      DSUMethod* dsu_method = dsu_class->first_method();
      for (;dsu_method!=NULL; dsu_method = dsu_method->next() ) {
        if ( dsu_method->method() == NULL ) {
          DSU_TRACE_MESG(("Validate DSU: method resolved failed"));
          return false;
        }
      }


      if (dsu_class->updating_type() == DSU_CLASS_MC
        || dsu_class->updating_type() == DSU_CLASS_NONE
        || dsu_class->updating_type() == DSU_CLASS_DEL) {
          continue;
      }
      if (dsu_class->stream_provider() == NULL) {
        DSU_TRACE_MESG(("Validate DSU: no stream provider"));
      }
   }
  }

  return true;
}

void Javelus::check_class_initialized(JavaThread *thread, InstanceKlass* new_ikh) {
  // inside intialize() there is a check
  new_ikh->initialize(thread);
  if (thread->has_pending_exception()) {
    Handle h_e(thread, thread->pending_exception());
    h_e->print();
    DSU_WARN(("Initialize class failed during update mixed_objects."));
  }
}

bool Javelus::is_class_not_born_at(InstanceKlass* the_class, int rn) {
  //  assert(rn>=0 && rn<=Javelus::system_revision_number(),"revison must be valid");
  return the_class->born_rn() < rn;
}

bool Javelus::is_class_born_at(InstanceKlass* the_class, int rn) {
  return the_class->born_rn() == rn;
}

bool Javelus::is_modifiable_class(oop klass_mirror) {
  // classes for primitives cannot be redefined
  if (java_lang_Class::is_primitive(klass_mirror)) {
    return false;
  }
  Klass* the_class = java_lang_Class::as_Klass(klass_mirror);
  // classes for arrays cannot be redefined
  if (the_class == NULL || !the_class->oop_is_instance()) {
    return false;
  }
  return true;
}

bool Javelus::is_class_alive(InstanceKlass* the_class, int rn) {
  return the_class->born_rn()<= rn && rn < the_class->dead_rn();
}

bool Javelus::is_class_dead_at(InstanceKlass* the_class, int rn) {
  return the_class->dead_rn() ==rn;
}

bool Javelus::is_class_dead(InstanceKlass* the_class, int rn) {
  return the_class->dead_rn() <=rn;
}


void Javelus::invoke_dsu_in_jvmti(jint class_count, const jvmtiClassDefinition* class_definitions) {
  Thread* THREAD = Thread::current();

  DSUJvmtiBuilder builder(class_count, class_definitions);
  builder.build(CHECK);

  DSU* dsu = builder.dsu();

  dsu->validate(CHECK);

  VM_DSUOperation * op = new VM_DSUOperation(dsu);
  DSUTask* task = new DSUTask(op);

  Javelus::get_dsu_thread()->add_task(task);

  {
    MutexLocker locker(DSURequest_lock);
    DSURequest_lock->wait();
  }

}


//-----------------------------------------------------------------------
// DSU Eager Update
//-----------------------------------------------------------------------




jobject*  DSUEagerUpdate::_candidates        = NULL;
int       DSUEagerUpdate::_candidates_length = 0;
volatile bool      DSUEagerUpdate::_be_updating      = false;
bool      DSUEagerUpdate::_update_pointers  = false;
volatile bool      DSUEagerUpdate::_has_been_updated = false;

void DSUEagerUpdate::initialize(TRAPS) {}


void DSUEagerUpdate::configure_eager_update(GrowableArray<jobject> * candidates, bool update_pointers) {
  //_candidates = candidates;
  const int length = candidates->length();
  _candidates_length = length;
  if (length > 0) {
    _candidates = (jobject*) os::malloc(sizeof(jobject)*length, mtInternal);
    for (int i=0;i<length;i++) {
      _candidates[i] = candidates->at(i);
    }

#ifdef ASSERT
    for (int i=0;i<length;i++) {
      oop obj = JNIHandles::resolve(_candidates[i]);
      assert(obj->is_instance(),"we only update instance object.");
    }
#endif
  } else {
    _candidates = NULL;
  }

  _update_pointers = update_pointers;
  _has_been_updated = false;
}

void DSUEagerUpdate::start_eager_update(JavaThread *thread) {
  {
    //Handle lock (thread, SystemDictionary::Object_klass());
    //ObjectLocker locker (lock, thread);
    MutexLocker ml(DSUEagerUpdate_lock);

    while(_be_updating) {
      DSUEagerUpdate_lock->wait();
      //locker.wait(thread);
    }

    if (_has_been_updated) {
      {
        ResourceMark rm(thread);
        DSU_TRACE(0x00000100,("Thread [%s] leaving without take update.", thread->get_thread_name()));
      }

      DSUEagerUpdate_lock->notify_all();
      //locker.notify_all(thread);
      return;
    }


    _be_updating = true;
    {
      ResourceMark rm(thread);
      DSU_TRACE(0x00000100,("Start Eager Update by the thread [%s].", thread->get_thread_name()));
    }

    elapsedTimer dsu_timer;

    // start timer
    DSU_TIMER_START(dsu_timer);


    int mixed_objects_size = 0;
    const int candidates_size = _candidates_length;

    if (_candidates!= NULL && candidates_size != 0) {

      HandleMark hm(thread);


      for(int i=0; i< candidates_size; i++) {
        jobject o = _candidates[i];

        oop obj = JNIHandles::resolve(o);

        if (obj != NULL ) {
          assert(obj->is_instance(),"we only transform instance objects.");
          Handle h (obj);
          Javelus::transform_object(h, thread);
          if (thread->has_pending_exception()) {
            DSU_WARN(("Transforming Objects Meets Exceptions!"));
            thread->clear_pending_exception();
            return;
          }

          // This path testing is really application dependent.
          if (h->mark()->is_mixed_object()) {
            mixed_objects_size++;
          }
          JNIHandles::destroy_weak_global(o);
        }

        _candidates[i] = NULL;
      }

      os::free(_candidates);
      _candidates = NULL;
      _candidates_length = 0;
    }

    // Report time, as our data collect script hardly depends on this.
    DSU_TIMER_STOP(dsu_timer);
    DSU_INFO(("DSU eager updating pause time: %3.7f (s). Candidate size is %d, mixed_objects size is %d.",dsu_timer.seconds(),candidates_size,mixed_objects_size));
    DSU_TIMER_START(dsu_timer);

    if (mixed_objects_size>0) {
      if (_update_pointers) {
        Universe::heap()->collect(GCCause::_jvmti_force_gc);
      } else {
        DSU_WARN(("You configure Eager Update without Updating Pointers, but we have met mixed objects."));
      }
    }

    DSU_TIMER_STOP(dsu_timer);
    DSU_INFO(("DSU eager updating total pause time: %3.7f (s).",dsu_timer.seconds()));
    if (DSU_TRACE_ENABLED(0x00000001)) {
      time_t tloc;
      time(&tloc);
      tty->print("[DSU]-[Info]: DSU Eager Object Updating finished at %s", ctime(&tloc));
    }
    _be_updating = false;
    _has_been_updated = true;

    DSUEagerUpdate_lock->notify_all();
    //locker.notify_all(thread);
  }
  {
    ResourceMark rm(thread);
    DSU_TRACE(0x00000100,("Finish Eager Update by the thread [%s].", thread->get_thread_name()));
  }
}

class DeadInstanceClosure : public ObjectClosure {
private:
  GrowableArray<jobject>* _result;
  int                 _dead_time;
  int                 _count;
public:
  DeadInstanceClosure(int dead_time, GrowableArray<jobject>* result) : _dead_time(dead_time), _result(result), _count(0) {};
  int dead_time() const {return _dead_time;}
  int count() const {return _count;}
  void do_object(oop obj) {
    _count++;
    if (obj->klass()->instances_require_update(dead_time())) {
      assert(obj->is_instance(), "currently we only update instance objects.");
      Handle o(obj);
      _result->append(JNIHandles::make_weak_global(o));
    }
  }
};

void DSUEagerUpdate::collect_dead_instances_at_safepoint(int dead_time, GrowableArray<jobject>* result, TRAPS) {
  assert(SafepointSynchronize::is_at_safepoint(), "all threads are stopped");
  HandleMark hm(THREAD);
  //Heap_lock->lock();

  //assert(Heap_lock->is_locked(), "should have the Heap_lock");

  // Ensure that the heap is parsable
  Universe::heap()->ensure_parsability(false);  // no need to retire TALBs

  // Iterate over objects in the heap
  DeadInstanceClosure dic(dead_time, result);
  // If this operation encounters a bad object when using CMS,
  // consider using safe_object_iterate() which avoids perm gen
  // objects that may contain bad references.
  Universe::heap()->object_iterate(&dic);
  DSU_DEBUG(("Total Count is %d.", dic.count()));
  //Heap_lock->unlock();
}

// Install a return barrier that waiting for the DSUEagerUpdate is finished.
void DSUEagerUpdate::install_eager_update_return_barrier(TRAPS) {

  for (JavaThread* thr = Threads::first(); thr != NULL; thr = thr->next()) {
    if (thr->is_Compiler_thread()) {
      continue;
    }

    if (thr->has_last_Java_frame()) {


      ////////////////////////////////
      // Currently We assume that there is no Native Method will calling a Java Method before eager updating has finished.
      // Note that JNI will always do object validity checks.
      // As we have not implement clear all flags after eager update and eager update pointers.
      // We just choose not to set them if we invoke VM with flag -XX:+ForceEagerUpdate and -XX:+ForceEagerUpdatePointers;
      ////////////////////////////////
      frame * callee  = NULL ;
      frame * current = NULL;
      int count = 0;
      StackFrameStream fst_callee(thr);
      for(StackFrameStream fst(thr); !fst.is_done(); fst.next()) {
        current= fst.current();
        if (count >0 ) {
          callee = fst_callee.current();
          //callee->print_on(tty);
          fst_callee.next();
        }
        count++;

        // install return barrier in the last java sp, i.e., the first executed method after
        if (current->is_interpreted_frame()) {
          // 1). fetch the Bytecode

          Method* method = current->interpreter_frame_method();
          assert(method->is_method(),"sanity check");

          if (callee != NULL) {
            if (callee->is_interpreted_frame()) {
              Bytecodes::Code code = Bytecodes::code_at(current->interpreter_frame_method(),current->interpreter_frame_bcp());
              int bci = current->interpreter_frame_bci();
              assert(bci>=0,"in the body");

              if (bci>0) {
                // in the body
                assert(code >= Bytecodes::_invokevirtual  && code <= Bytecodes::_invokedynamic, "must be an invoke"  );

                TosState tos = as_TosState(callee->interpreter_frame_method()->result_type());
                address barrier_addr;
                address normal_addr;
                if (code == Bytecodes::_invokedynamic
                  || code == Bytecodes::_invokeinterface) {
                    normal_addr = Interpreter::return_entry(tos, 5, code);
                    barrier_addr = Interpreter::return_with_barrier_entry(tos, 5, code);
                } else {
                  normal_addr = Interpreter::return_entry(tos, 3, code);
                  barrier_addr = Interpreter::return_with_barrier_entry(tos, 3, code);
                }

                assert(normal_addr == current->pc(), "sanity check");

                // TODO
                current->patch_pc(thr,barrier_addr);
                thr->set_return_barrier_type(JavaThread::_eager_update);
                // XXX Note, unlike return barrier id for wake_up_dsu, which is install at the caller of restricted method
                // here, we install the return barrier at the top most Java frame.
                thr->set_return_barrier_id(current->id());
              } else {
                // trigger on other runtime call
                tty->print_cr("trigger on other runtime call");
                thr->set_return_barrier_type(JavaThread::_eager_update);
                // XXX Note, unlike return barrier id for wake_up_dsu, which is install at the caller of restricted method
                // here, we install the return barrier at the top most Java frame.
                thr->set_return_barrier_id(current->id());
              }
            }// end of interpreted frame
            else if (callee->is_safepoint_blob_frame()) {
              assert(false,"safe point blob");
              //address barrier_addr = Interpreter::re

              //current->patch_pc(thr,barrier_addr);
              //thr->set_return_barrier_type(JavaThread::_eager_update);
              // XXX Note, unlike return barrier id for wake_up_dsu, which is install at the caller of restricted method
              // here, we install the return barrier at the top most Java frame.
              //thr->set_return_barrier_id(current->id());
            } else {
              current->print_on(tty);
              assert(false,"unhandle callee safe point blob");
            }
          }// end of if callee
          else {
            // we are return from a safe point
            thr->set_return_barrier_type(JavaThread::_eager_update);
            thr->set_return_barrier_id(current->id());
          }
        } else if (current->is_compiled_frame()) {
          thr->set_return_barrier_type(JavaThread::_eager_update);
          thr->set_return_barrier_id(current->id());
          // Do nothing.
          // XXX set the address of
          if (current->is_deoptimized_frame()) {
            // We do not depotimize a deoptimized frame.
            //tty->print_cr("[DSU] return barrier is deoptimized frame");
          } else {
            // We just deoptimize it
            //tty->print_cr("[DSU] return barrier is compiled frame");
            Deoptimization::deoptimize(thr,*current,fst.register_map());
          }
        } else {
          // skip continue frame
          continue;
        }
        // stop iterate the stack!
        break;
      }

      assert(thr->return_barrier_id() != NULL, "return barrier id must  not be null!!");
      {
        ResourceMark rm;
        DSU_TRACE(0x00000100, ("install eager update [%d] return barrier at " PTR_FORMAT " for thread %s.",
          thr->return_barrier_type(),
          p2i(thr->return_barrier_id()),
          thr->get_thread_name()));
      }
    }
    //#ifdef ASSERT
    //    thr->verify();
    //#endif
  }
}

