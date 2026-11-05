// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/Jit/profile.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <iostream>
#include <new>
#include <utility>

extern "C" {

bool isJitCompiled(const PyFunctionObject* func) {
  // Possible that this is called during finalization after the module state is
  // destroyed.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return false;
  }
  auto code_allocator = mod_state->code_allocator.get();
  return code_allocator != nullptr &&
      code_allocator->contains(reinterpret_cast<const void*>(func->vectorcall));
}

} // extern "C"

namespace cinderx::jit {

namespace {

void compiledfunc_dealloc(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  auto cf = reinterpret_cast<CompiledFunction*>(self);
  PyObject_GC_UnTrack(self);

  // Call destructor for C++ members.
  cf->~CompiledFunction();

  type->tp_free(self);

  Py_DECREF(type);
}

int compiledfunc_traverse(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  auto cf = reinterpret_cast<CompiledFunction*>(self);
  return cf->traverse(visit, arg);
}

int compiledfunc_clear(PyObject* self) {
  CompiledFunction* cf = reinterpret_cast<CompiledFunction*>(self);
  // Break the function <-> CompiledFunction cycle from this side as well: the
  // functions themselves may already have been cleared, and so may no longer be
  // able to find their way back here to release what they own.
  cf->releaseFunctionRefs();
  cf->clear();
  return 0;
}

// A CompiledFunction holds per-process JIT state (machine code, native entry
// point, CodeRuntime). It cannot and need not survive pickling or copying: the
// underlying function serializes normally and is re-JIT'd in the destination
// process. Reduce it to a call that yields a harmless None placeholder.
PyObject* compiledfunc_reduce(PyObject* /*self*/, PyObject* /*ignored*/) {
  // The reconstructor lives in the native _cinderx module (not the cinderx
  // Python layer), keeping this a native -> native reference.  It is always
  // loaded when a CompiledFunction exists.
  auto jit_module = Ref<>::steal(PyImport_ImportModule("_cinderx"));
  if (jit_module == nullptr) {
    return nullptr;
  }
  auto reconstructor = Ref<>::steal(PyObject_GetAttrString(
      jit_module.get(), "_reconstruct_pickled_compiled_function"));
  if (reconstructor == nullptr) {
    return nullptr;
  }
  // The 2-tuple (reconstructor, ()) tells pickle/copy to rebuild the value by
  // calling reconstructor(), which returns None.
  return Py_BuildValue("(O())", reconstructor.get());
}

PyMethodDef compiledfunc_methods[] = {
    {"__reduce__",
     compiledfunc_reduce,
     METH_NOARGS,
     PyDoc_STR(
         "Reduce to a picklable None placeholder, dropping JIT machine "
         "code so a JIT-compiled function can be pickled/copied.")},
    {nullptr, nullptr, 0, nullptr},
};

PyType_Slot compiled_function_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(compiledfunc_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(compiledfunc_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(compiledfunc_clear)},
    {Py_tp_methods, reinterpret_cast<void*>(compiledfunc_methods)},
    {0, nullptr}};

void compiledfuncdata_dealloc(PyObject* self) {
  PyTypeObject* type = Py_TYPE(self);
  CompiledFunctionData* cfd = reinterpret_cast<CompiledFunctionData*>(self);
  PyObject_GC_UnTrack(self);
  cfd->releaseRuntimeResources();

  // Deregister unwind info before releasing the code buffer: unwinders
  // must never resolve a reused address through a stale frame.
  cfd->unregisterUnwindInfo();

  // Release the code buffer back to the allocator.  For the contiguous
  // (immortal) case this is handled by the CompiledFunction destructor
  // instead.
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state != nullptr && cfd->code.data() != nullptr) {
    auto code_allocator = mod_state->code_allocator.get();
    if (code_allocator != nullptr) {
      code_allocator->releaseCode(const_cast<std::byte*>(cfd->code.data()));
    }
  }

  cfd->~CompiledFunctionData();
  type->tp_free(self);

  Py_DECREF(type);
}

int compiledfuncdata_traverse(PyObject* self, visitproc visit, void* arg) {
  Py_VISIT(Py_TYPE(self));
  auto cfd = reinterpret_cast<CompiledFunctionData*>(self);
  if (cfd->runtime != nullptr) {
#ifdef ENABLE_PREFORK_MODEL
    // During shutdown the Context (and its CodeRuntime slab) may be destroyed
    // while CFs still exist in function dicts.  Skip traversal if the JIT
    // context is gone.
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state != nullptr && mod_state->jit_context != nullptr) {
      if (const int err = cfd->runtime->traverse(visit, arg); err != 0) {
        return err;
      }
    }
#else
    if (const int err = cfd->runtime->traverse(visit, arg); err != 0) {
      return err;
    }
#endif
  }

  return 0;
}

PyType_Slot compiled_function_data_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(compiledfuncdata_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(compiledfuncdata_traverse)},
    {0, nullptr}};

PyType_Spec CompiledFunction_Spec = {
    .name = "_cinderx.CompiledFunction",
    .basicsize = sizeof(CompiledFunction),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = compiled_function_slots,
};

PyType_Spec CompiledFunctionData_Spec = {
    .name = "_cinderx.CompiledFunctionData",
    .basicsize = sizeof(CompiledFunctionData),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = compiled_function_data_slots,
};

} // namespace

int initCompiledFunctionType() {
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    PyErr_SetString(
        PyExc_RuntimeError,
        "Can't initialize compiled function types, lacking module state");
    return -1;
  }

  auto data_type = reinterpret_cast<PyTypeObject*>(
      PyType_FromSpec(&CompiledFunctionData_Spec));
  if (data_type == nullptr) {
    return -1;
  }
  mod_state->compiled_function_data_type = Ref<PyTypeObject>::steal(data_type);

  auto func_type =
      reinterpret_cast<PyTypeObject*>(PyType_FromSpec(&CompiledFunction_Spec));
  if (func_type == nullptr) {
    return -1;
  }
  mod_state->compiled_function_type = Ref<PyTypeObject>::steal(func_type);

  return 0;
}

BorrowedRef<PyTypeObject> getCompiledFunctionType() {
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return nullptr;
  }
  return mod_state->compiled_function_type;
}

CompiledFunction::CompiledFunction(
    CompiledFunctionData* data,
    CompiledFunctionOwner& owner,
    bool contiguous)
    : owner_(&owner), data_(data), contiguous_data_(contiguous) {
  data_->owner = &owner;
}

std::span<const std::byte> CompiledFunction::codeBuffer() const {
  return data_->code;
}

PyObject* CompiledFunction::invoke(
    PyObject* func,
    PyObject** args,
    Py_ssize_t nargs) const {
  return data_->vectorcall_entry(func, args, nargs, nullptr);
}

size_t CompiledFunction::codeSize() const {
  return data_->code.size();
}

int CompiledFunction::stackSize() const {
  return data_->stack_size;
}

int CompiledFunction::spillStackSize() const {
  return data_->spill_stack_size;
}

const hir::Function::InlineFunctionStats&
CompiledFunction::inlinedFunctionsStats() const {
  return data_->inline_function_stats;
}

const hir::OpcodeCounts& CompiledFunction::hirOpcodeCounts() const {
  return data_->hir_opcode_counts;
}

#ifndef ENABLE_PREFORK_MODEL
const std::vector<InlineCacheSite>& CompiledFunction::inlineCacheSites() const {
  JIT_DCHECK(
      data_ != nullptr && data_->stable_storage != nullptr,
      "stable storage is not available");
  return data_->stable_storage->inlineCacheSites();
}
#endif

Profile CompiledFunction::snapshotProfile() const {
  JIT_DCHECK(
      data_ != nullptr && data_->profile_collector != nullptr,
      "profile collector is not available");
  return data_->profile_collector->snapshot();
}

void CompiledFunction::removeProfileEntry(const ProfileLocation& location) {
  if (data_ == nullptr || data_->profile_collector == nullptr) {
    return;
  }
  data_->profile_collector->remove(location);
}

bool CompiledFunction::isContiguous() const {
  return contiguous_data_;
}

CompiledFunctionData* CompiledFunction::data() const {
  return data_;
}

CompiledFunctionData* CompiledFunction::stealData() {
  CompiledFunctionData* d = data_;
  data_ = nullptr;
  return d;
}

Ref<CompiledFunction> CompiledFunction::create(
    CompiledFunctionData&& compiled_func,
    CompiledFunctionOwner& owner,
    bool immortal) {
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  JIT_DCHECK(
      mod_state != nullptr && mod_state->compiled_function_type != nullptr &&
          mod_state->compiled_function_data_type != nullptr,
      "CompiledFunction types not initialized");

  CompiledFunction* cf;
  // If the CompiledFunction is not being allocated as immortal we need a second
  // object to properly track the lifetime and free the underlying code. If it
  // is immortal we'll just never free the code anyway.
  if (!immortal) {
    // Allocate CompiledFunctionData as a separate GC-tracked Python object.
    CompiledFunctionData* cfd = PyObject_GC_New(
        CompiledFunctionData, mod_state->compiled_function_data_type);
    if (cfd == nullptr) {
      return nullptr;
    }
    PyObject saved_base = cfd->ob_base;
    new (cfd) CompiledFunctionData(std::move(compiled_func));
    cfd->ob_base = saved_base;
    cfd->owner = &owner;
    PyObject_GC_Track(reinterpret_cast<PyObject*>(cfd));

    cf = PyObject_GC_New(CompiledFunction, mod_state->compiled_function_type);
    if (cf == nullptr) {
      Py_DECREF(cfd);
      return nullptr;
    }
    new (cf) CompiledFunction(cfd, owner, false);
    PyObject_GC_Track(reinterpret_cast<PyObject*>(cf));
  } else {
    // Single contiguous allocation for CompiledFunction + CompiledFunctionData.
    constexpr size_t cf_size = sizeof(CompiledFunction);
    constexpr size_t cfd_align = alignof(CompiledFunctionData);
    constexpr size_t cfd_offset = (cf_size + cfd_align - 1) & ~(cfd_align - 1);
    size_t total_size = cfd_offset + sizeof(CompiledFunctionData);

    char* raw = static_cast<char*>(::operator new(total_size));
    CompiledFunctionData* cfd =
        new (raw + cfd_offset) CompiledFunctionData(std::move(compiled_func));
    cf = new (raw) CompiledFunction(cfd, owner, true);

    PyObject_Init(&cf->ob_base, mod_state->compiled_function_type);
    PyObject_Init(&cfd->ob_base, mod_state->compiled_function_data_type);
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(&cf->ob_base);
    _Py_SetImmortalUntracked(&cfd->ob_base);
#else
    _Py_SetImmortal(&cf->ob_base);
    _Py_SetImmortal(&cfd->ob_base);
#endif
  }

  if (cf->runtime() != nullptr) {
    cf->runtime()->setCompilationData(cf->data());
  }
  return Ref<CompiledFunction>::steal(cf);
}

CompiledFunctionData::~CompiledFunctionData() {
  releaseRuntimeResources();
  unregisterUnwindInfo();
}

bool CompiledFunctionData::defer() {
  FreeThreadedJITEntrypointGuard guard;
  if (owner == nullptr || runtime == nullptr) {
    return false;
  }
  Ref<> code_ref = Ref<>::create(runtime->code());
  Ref<> builtins = Ref<>::create(runtime->builtins());
  Ref<> globals = Ref<>::create(runtime->globals());
  Py_INCREF(this);
  owner->deferCompiledData(
      std::move(code_ref), std::move(builtins), std::move(globals), this);
  return true;
}

void CompiledFunctionData::releaseRuntimeResources() {
  auto* saved_owner = std::exchange(owner, nullptr);
  if (saved_owner == nullptr) {
    return;
  }
  auto* state = cinderx::getModuleState();
  if (state == nullptr || state->jit_context == nullptr || runtime == nullptr) {
    return;
  }
  for (auto& patcher : code_patchers) {
    if (auto typed_patcher = dynamic_cast<TypeDeoptPatcher*>(patcher.get())) {
      saved_owner->unwatch(typed_patcher);
    }
    if (auto func_patcher =
            dynamic_cast<FuncCodeDeoptPatcher*>(patcher.get())) {
      saved_owner->unwatchFunc(func_patcher);
    }
  }
  releaseRuntimeReferences();
}

void CompiledFunctionData::releaseRuntimeReferences() {
  if (runtime == nullptr) {
    return;
  }
  auto references = runtime->releaseReferences();
  runtime = nullptr;
}

CompiledFunction::~CompiledFunction() {
  clear();

  if (data_ == nullptr) {
    return;
  }

  // clear() can release the last references to our module during finalization
  auto* mod_state = cinderx::getModuleState();

  if (contiguous_data_) {
    data_->unregisterUnwindInfo();
    if (mod_state != nullptr && data_->code.data() != nullptr) {
      auto code_allocator = mod_state->code_allocator.get();
      if (code_allocator != nullptr) {
        code_allocator->releaseCode(const_cast<std::byte*>(data_->code.data()));
      }
    }
    data_->~CompiledFunctionData();
  } else {
    Py_DECREF(data_);
  }
  data_ = nullptr;
}

void CompiledFunction::disassemble() const {
  auto start = reinterpret_cast<const char*>(codeBuffer().data());
  Disassembler dis{start, codeSize()};
  dis.disassembleAll(std::cout);
}

void CompiledFunction::printHIR() const {
  JIT_CHECK(
      data_->irfunc != nullptr,
      "Can only call CompiledFunction::printHIR() from a debug build");
  hir::HIRPrinter printer{/*symbolize*/ true};
  printer.print(std::cout, *data_->irfunc);
}

std::chrono::nanoseconds CompiledFunction::compileTime() const {
  return data_->compile_time;
}

void CompiledFunction::setCompileTime(std::chrono::nanoseconds time) {
  data_->compile_time = time;
}

void CompiledFunction::setCodePatchers(
    std::vector<std::unique_ptr<CodePatcher>>&& code_patchers) {
  data_->code_patchers = std::move(code_patchers);
}

void CompiledFunction::setHirFunc(std::unique_ptr<hir::Function>&& irfunc) {
  data_->irfunc = std::move(irfunc);
}

void* CompiledFunction::staticEntry() const {
  if (data_->runtime == nullptr ||
      !(data_->runtime->code()->co_flags & CI_CO_STATICALLY_COMPILED)) {
    return nullptr;
  }

  return reinterpret_cast<void*>(
      JITRT_GET_STATIC_ENTRY(data_->vectorcall_entry));
}

void CompiledFunction::addFunction(BorrowedRef<PyFunctionObject> func) {
  // The vectorcall entry point is the marker that this function is
  // currently this compiled function.
#if Py_DEBUG
  JIT_DCHECK(!functions_.contains(func), "no duplicates");
  functions_.insert(func.get());
#endif
  num_functions_++;
  Py_INCREF(this);
  setVectorcall(func, vectorcallEntry());
}

Ref<CompiledFunction> CompiledFunction::stealFunction(
    BorrowedRef<PyFunctionObject> func) {
  if (num_functions_ == 0 || func->vectorcall != vectorcallEntry()) {
    return nullptr;
  }
#if Py_DEBUG
  JIT_DCHECK(functions_.contains(func), "should be registered");
  functions_.erase(func.get());
#endif
  num_functions_--;
  setVectorcall(func, getInterpretedVectorcall(func));
  return Ref<CompiledFunction>::steal(this);
}

void CompiledFunction::removeFunction(BorrowedRef<PyFunctionObject> func) {
  // This may be the last reference, so nothing may touch the object after
  // this point.
  stealFunction(func).reset();
}

void CompiledFunction::deoptFunction(BorrowedRef<PyFunctionObject> func) {
#if Py_DEBUG
  JIT_DCHECK(functions_.contains(func), "should be registered");
#endif
  // No refcount change: the function keeps the reference it took in
  // addFunction(), which is what holds this compile together while the JIT is
  // disabled and nothing is running it.
  setVectorcall(func, getInterpretedVectorcall(func));
}

void CompiledFunction::reoptFunction(BorrowedRef<PyFunctionObject> func) {
#if Py_DEBUG
  JIT_DCHECK(functions_.contains(func), "should be registered");
#endif
  JIT_DCHECK(num_functions_ > 0, "reopting an unregistered function");
  setVectorcall(func, vectorcallEntry());
}

Ref<CompiledFunction> CompiledFunction::stealDeoptedFunction(
    BorrowedRef<PyFunctionObject> func) {
  if (num_functions_ == 0) {
    // releaseFunctionRefs() already handed every registration back.
    return nullptr;
  }
#if Py_DEBUG
  JIT_DCHECK(functions_.contains(func), "should be registered");
  functions_.erase(func.get());
#endif
  num_functions_--;
  return Ref<CompiledFunction>::steal(this);
}

void CompiledFunction::releaseDeoptedFunction(
    BorrowedRef<PyFunctionObject> func) {
  // This may be the last reference, so nothing may touch the object after
  // this point.
  stealDeoptedFunction(func).reset();
}

int CompiledFunction::traverse(visitproc visit, void* arg) {
  // Don't traverse functions_ - these are borrowed references that we don't
  // own. The functions are responsible for removing themselves via
  // funcDestroyed() when they are deallocated. Not traversing them allows
  // functions to be garbage collected independently of this CompiledFunction.

  if (data_ == nullptr) {
    return 0;
  }

  if (!contiguous_data_) {
    Py_VISIT(data_);
  } else {
    if (data_->runtime != nullptr) {
#ifdef ENABLE_PREFORK_MODEL
      // During shutdown the Context (and its CodeRuntime slab) may be destroyed
      // while CFs still exist in function dicts.  Skip traversal if the JIT
      // context is gone.
      cinderx::ModuleState* mod_state = cinderx::getModuleState();
      if (mod_state != nullptr && mod_state->jit_context != nullptr) {
        if (const int err = data_->runtime->traverse(visit, arg); err != 0) {
          return err;
        }
      }
#else
      if (const int err = data_->runtime->traverse(visit, arg); err != 0) {
        return err;
      }
#endif
    }
  }

  return 0;
}

void CompiledFunction::clear(bool context_finalizing) {
  FreeThreadedJITEntrypointGuard guard;
  if (owner_ == nullptr) {
    return;
  }

  // During shutdown the Context and its prefork stable storage may be destroyed
  // while CFs still exist in function dicts. Null out borrowed pointers to
  // avoid dereferencing them later.
  if (owner_ != nullptr) {
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state == nullptr || mod_state->jit_context == nullptr) {
      owner_ = nullptr;
#ifdef ENABLE_PREFORK_MODEL
      if (data_ != nullptr) {
        data_->runtime = nullptr;
      }
#endif
    }
  }

  auto* owner = std::exchange(owner_, nullptr);
  if (data_ != nullptr) {
    data_->retired = true;
  }
  bool deferred = false;
  if (!contiguous_data_ && !context_finalizing && owner != nullptr &&
      data_ != nullptr && data_->runtime != nullptr) {
    deferred = data_->defer();
  }

  if (owner != nullptr && !context_finalizing) {
    owner->forgetCompiledFunction(*this);
  }

  if (!deferred && data_ != nullptr) {
    if (owner != nullptr) {
      data_->releaseRuntimeResources();
    } else {
      data_->releaseRuntimeReferences();
    }
  }
}

void CompiledFunction::releaseFunctionRefs() {
  // Only for tp_clear: the collector has decided this compile is cyclic
  // garbage, so every function still using it is garbage too and none of them
  // will come back through removeFunction().
  //
  // Deliberately NOT done from clear(): forgetCode() and deoptFunc() clear a
  // compile while its functions are alive and, in the middle of a recursion,
  // still running its machine code.  Those references belong to the functions
  // until the functions themselves go away.
  size_t count = std::exchange(num_functions_, 0);
#if Py_DEBUG
  JIT_DCHECK(
      functions_.size() == count,
      "{} registered functions but a count of {}",
      functions_.size(),
      count);
  functions_.clear();
#endif
  for (size_t i = 0; i < count; i++) {
    Py_DECREF(this);
  }
}

void CompiledFunctionData::unregisterUnwindInfo() {
  for (std::unique_ptr<JitUnwindRegistration>& registration :
       unwind_registrations) {
    unregisterJitUnwindInfo(std::move(registration));
  }
  unwind_registrations.clear();
}

} // namespace cinderx::jit
