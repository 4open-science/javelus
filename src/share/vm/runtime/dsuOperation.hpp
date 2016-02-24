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

#ifndef SHARE_VM_RUNTIME_DSUOPERATION_HPP
#define SHARE_VM_RUNTIME_DSUOPERATION_HPP

#include "runtime/vm_operations.hpp"
#include "runtime/thread.hpp"

class VM_DSUOperation;

class DSUTask : public CHeapObj<mtInternal> {

private:
  VM_DSUOperation * _op;
  DSUTask* _next;
public:
  DSUTask(VM_DSUOperation * op);

  DSUTask* next() const            { return _next; }
  void     set_next(DSUTask* next) { _next = next; }
  VM_DSUOperation * operation()    {return _op; }
};

class VM_DSUOperation : public VM_Operation {
private:
  DSU*     _dsu;

  DSUError _res;


public:
  VM_DSUOperation(DSU * dsu);
  virtual ~VM_DSUOperation();

  VMOp_Type type() const { return VMOp_DSU; };

  virtual void doit();
  virtual bool doit_prologue();
  virtual void doit_epilogue();

  DSU* dsu() const { return _dsu; }

  //void free_memory();

  bool is_finished()    const { return _dsu->is_finished(); }
  bool is_discarded()   const { return _dsu->is_discarded(); }
  bool is_interrupted() const { return _dsu->is_interrupted(); }
  bool is_init()        const { return _dsu->is_init(); }
  void set_request_state(DSURequestState state){
    _dsu->set_request_state(state);
  }

  // check whether dsu is fully parsable
  bool     verify_dsu(TRAPS);
  // if dsu has been fully parsed, prepare this dsu
  DSUError prepare_dsu(TRAPS);
  // at VM safe points, update this dsu
  DSUError update_dsu(TRAPS);
};

class VM_RelinkMixedObject : public VM_Operation {
protected:
  Handle *_inplace_object;
  Handle *_new_phantom_object;

public:
  VM_RelinkMixedObject(Handle *inplace_object, Handle *new_phantom_object);
  virtual VMOp_Type type() const { return VMOp_RelinkMixedObject; }
  virtual bool doit_prologue();
  virtual void doit();
};

class VM_UnlinkMixedObject : public VM_Operation {
protected:
  Handle* _inplace_object;

public:
  VM_UnlinkMixedObject(Handle *inplace_object);

  virtual VMOp_Type type() const { return VMOp_UnlinkMixedObject; }
  virtual bool doit_prologue();
  virtual void doit();
};
#endif
