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

#include "precompiled.hpp"
#include "runtime/dsu.hpp"
#include "runtime/dsuOperation.hpp"
#include "interpreter/oopMapCache.hpp"
#include "interpreter/rewriter.hpp"
#include "memory/oopFactory.hpp"
#include "memory/heapInspection.hpp"
#include "classfile/javaClasses.hpp"
#include "code/codeCache.hpp"
#include "runtime/deoptimization.hpp"
#include "runtime/relocator.hpp"
#include "runtime/javaCalls.hpp"

#include "oops/klass.inline.hpp"



DSUTask::DSUTask(VM_DSUOperation *op)
: _op(op), _next(NULL) {}

VM_DSUOperation::VM_DSUOperation(DSU *dsu)
: _dsu(dsu) {}

VM_DSUOperation::~VM_DSUOperation() {
  if (_dsu != NULL) {
    delete _dsu;
  }
}

DSUError VM_DSUOperation::prepare_dsu(TRAPS) {
  DSUError ret = _dsu->prepare(THREAD);

  if (ret != DSU_ERROR_NONE) {
    return ret;
  }

  if (HAS_PENDING_EXCEPTION) {
    tty->print_cr("WARNNING: Prepare DSU Error!!");
    return DSU_ERROR_TO_BE_ADDED;
  }

  return DSU_ERROR_NONE;
}

bool VM_DSUOperation::verify_dsu(TRAPS) {
  if (_dsu == NULL) {
    _res = DSU_ERROR_NULL_POINTER;
    return false;
  }

  if (_dsu->first_class_loader() == NULL) {
    DSU_TRACE_MESG(("No class loader, empty DSU, no class to be updated!"));
    return false;
  }

  DSUClassLoader *dsu_loader = _dsu->first_class_loader();
  int to_be_updated = 0;
  for (; dsu_loader != NULL; dsu_loader = dsu_loader->next()) {
    DSUClass* dsu_class = dsu_loader->first_class();    
    for (;dsu_class != NULL; dsu_class=dsu_class->next()) {
      to_be_updated++;      
      if (dsu_class == NULL) {
        DSU_TRACE_MESG(("Null DSUClass!"));
        return false;
      }
    }
  }

  if (to_be_updated == 0) {
    DSU_TRACE_MESG(("Empty DSU, no class to be updated!"));
    return false;
  }
  return true;
}

bool VM_DSUOperation::doit_prologue() {
  Thread* thread = Thread::current();

  if (is_discarded()) {
    _res = DSU_ERROR_BAD_REQUEST_TYPE;
  }

  if (!verify_dsu(thread)) {
    return false;
  }

  // This is the first time invoking the DSU,
  // so do some init work...
  if (is_init()) {
    // first of all, set the active DSU
    // Note that now we are still in the DSUThread.
    Javelus::set_active_dsu(_dsu);

    _res = prepare_dsu(thread);

    if (_res != DSU_ERROR_NONE) {
      // Free os::malloc allocated memory in load_new_class_version.
      //free_memory();
      {
        // notify the DSU_Thread that we are fail to execute
        MutexLocker locker(DSURequest_lock);
        DSURequest_lock->notify_all();
      }
      DSU_TRACE_MESG(("Discard DSU request, Error Code %d",_res));
      set_request_state(DSU_REQUEST_DISCARDED);
      return false;
    }

    return true;
  }

  // This is another try of the same DSU.
  // This is mainly because you choose to update program only at safe point.
  // As we have prepared it before, we can just start the update anywhere.
  // TODO in fact, we need to verify whether the system has modified due to new loaded classes
  // and check the validity of the dsu.
  // To be implemented.
  if (is_interrupted()) {
    DSU_TRACE_MESG(("[DSU] Continue a interrupted DSU..."));
  }

  return true;
}

// wrapper of update DSU, may be removed in future.
DSUError VM_DSUOperation::update_dsu(TRAPS) {
  return dsu()->update(THREAD);
}

// Perform the actual update.
void VM_DSUOperation::doit() {
  Thread *thread = Thread::current();

  _res = update_dsu(thread);

  if (thread->has_pending_exception()) {
    oop exception = thread->pending_exception();
    exception->print();
    thread->clear_pending_exception();
    set_request_state(DSU_REQUEST_FAILED);
    return;
  }

  if (_res == DSU_ERROR_NONE) {
    set_request_state(DSU_REQUEST_FINISHED);
  } else {
    set_request_state(DSU_REQUEST_FAILED);
  }
}


void eager_initialize_added_class (DSUClass * dsu_class, TRAPS) {
  if (dsu_class->updating_type() == DSU_CLASS_ADD) {
    ResourceMark rm(THREAD);
    DSU_TRACE_MESG(("Eagerly initialize an added class %s.", dsu_class->name()->as_C_string()));
    instanceKlassHandle new_version (THREAD, dsu_class->new_version_class());
    new_version->initialize(CHECK);
  }
}

void VM_DSUOperation::doit_epilogue() {
// clean work to be implemented
  if (_res == DSU_ERROR_NONE) {
    // Eager initialize added class for Chun Cao's loong projects
    Thread* thread = Thread::current();
    dsu()->classes_do(eager_initialize_added_class, thread);
  }
}


VM_RelinkMixedObject::VM_RelinkMixedObject(Handle * inplace_object, Handle * new_phantom_object)
: _inplace_object(inplace_object), _new_phantom_object(new_phantom_object) {}

bool VM_RelinkMixedObject::doit_prologue() {
  if (_inplace_object == NULL) {
    return false;
  }
  if (_new_phantom_object == NULL) {
    return false;
  }

  if (!(*_inplace_object)->mark()->is_mixed_object()) {
    return false;
  }

  if ((*_new_phantom_object)->mark()->is_mixed_object()) {
    return false;
  }
  return true;
}

void VM_RelinkMixedObject::doit() {
  oop inplace_object  = (*_inplace_object)();
  oop old_phantom_object = (oop)inplace_object->mark()->decode_phantom_object_pointer();
  oop new_phantom_object = (*_new_phantom_object)();

  // set real mark to new mix new object
  markOop real_mark = old_phantom_object->mark();
  new_phantom_object->set_mark(real_mark);

  // link new mix new object to mix old object
  markOop new_mark  = markOopDesc::encode_phantom_object_pointer_as_mark(new_phantom_object);
  inplace_object->set_mark(new_mark);
}


VM_UnlinkMixedObject::VM_UnlinkMixedObject(Handle *inplace_object)
: _inplace_object(inplace_object) {}

bool VM_UnlinkMixedObject::doit_prologue() {
  return _inplace_object != NULL && (*_inplace_object)->mark()->is_mixed_object();
}

void VM_UnlinkMixedObject::doit() {
  oop inplace_object     = (*_inplace_object)();
  oop old_phantom_object = (oop)inplace_object->mark()->decode_phantom_object_pointer();

  markOop real_mark = old_phantom_object->mark();
  inplace_object->set_mark(real_mark);
}
