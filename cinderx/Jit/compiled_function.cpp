// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/compiled_function.h"

#include "internal/pycore_object.h"

#include "cinderx/Common/dict.h"
#include "cinderx/Common/extra-py-flags.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/util.h"
#include "cinderx/Jit/disassembler.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/module_c_state.h"
#include "cinderx/module_state.h"

#include <iostream>
#include <new>

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

// The key used to store the CompiledFunction in a function's __dict__.
PyObject *kCompiledFunctionKey, *kNestedCompiledFunctionsKey = nullptr;

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
  cf->clear();
  return 0;
}

// A CompiledFunction holds per-process JIT state (machine code, native entry
// point, CodeRuntime) stashed under a function's __dict__.  It cannot and need
// not survive pickling or copying: the underlying function serializes normally
// and is re-JIT'd in the destination process.  Reduce it to a call that yields
// a harmless None placeholder so cloudpickle/pickle/copy of any JIT-compiled
// function succeed.
PyObject* compiledfunc_reduce(PyObject* /*self*/, PyObject* /*ignored*/) {
  // The reconstructor lives in the native cinderjit module (not the cinderx
  // Python layer), keeping this a native -> native reference.  It is always
  // loaded when a CompiledFunction exists.
  auto jit_module = Ref<>::steal(PyImport_ImportModule("cinderjit"));
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
    // During shutdown the Context (and its CodeRuntime slab) may be destroyed
    // while CFs still exist in function dicts.  Skip traversal if the JIT
    // context is gone.
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state != nullptr && mod_state->jit_context != nullptr) {
      return cfd->runtime->traverse(visit, arg);
    }
  }

  return 0;
}

int compiledfuncdata_clear(PyObject* /*self*/) {
  return 0;
}

PyType_Slot compiled_function_data_slots[] = {
    {Py_tp_dealloc, reinterpret_cast<void*>(compiledfuncdata_dealloc)},
    {Py_tp_traverse, reinterpret_cast<void*>(compiledfuncdata_traverse)},
    {Py_tp_clear, reinterpret_cast<void*>(compiledfuncdata_clear)},
    {0, nullptr}};

PyType_Spec CompiledFunction_Spec = {
    .name = "cinderjit.CompiledFunction",
    .basicsize = sizeof(CompiledFunction),
    .flags = Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
        Py_TPFLAGS_DISALLOW_INSTANTIATION | Py_TPFLAGS_IMMUTABLETYPE,
    .slots = compiled_function_slots,
};

PyType_Spec CompiledFunctionData_Spec = {
    .name = "cinderjit.CompiledFunctionData",
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

  // Create the key used to store CompiledFunction in function's __dict__.
  kCompiledFunctionKey =
      PyUnicode_InternFromString("__cinderx_compiled_func__");
  if (kCompiledFunctionKey == nullptr) {
    return -1;
  }

  kNestedCompiledFunctionsKey =
      PyUnicode_InternFromString("__cinderx_nested_compiled_funcs__");
  if (kNestedCompiledFunctionsKey == nullptr) {
    return -1;
  }

  return 0;
}

BorrowedRef<PyTypeObject> getCompiledFunctionType() {
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (mod_state == nullptr) {
    return nullptr;
  }
  return mod_state->compiled_function_type;
}

CompiledFunction::CompiledFunction(CompiledFunctionData* data, bool contiguous)
    : data_(data), contiguous_data_(contiguous) {}

std::span<const std::byte> CompiledFunction::codeBuffer() const {
  return data_->code;
}

vectorcallfunc CompiledFunction::vectorcallEntry() const {
  return data_->vectorcall_entry;
}

CodeRuntime* CompiledFunction::runtime() const {
  return data_->runtime;
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
      data_ != nullptr && data_->inline_cache_storage != nullptr,
      "inline cache storage is not available");
  return data_->inline_cache_storage->inlineCacheSites();
}
#endif

void CompiledFunction::setOwner(CompiledFunctionOwner* owner) {
  owner_ = owner;
}

std::unordered_set<BorrowedRef<PyFunctionObject>>&
CompiledFunction::functions() {
  return functions_;
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
    PyObject_GC_Track(reinterpret_cast<PyObject*>(cfd));

    cf = PyObject_GC_New(CompiledFunction, mod_state->compiled_function_type);
    if (cf == nullptr) {
      Py_DECREF(cfd);
      return nullptr;
    }
    new (cf) CompiledFunction(cfd, false);
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
    cf = new (raw) CompiledFunction(cfd, true);

    PyObject_Init(&cf->ob_base, mod_state->compiled_function_type);
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(&cf->ob_base);
#else
    _Py_SetImmortal(&cf->ob_base);
#endif
  }

  return Ref<CompiledFunction>::steal(cf);
}

CompiledFunction::~CompiledFunction() {
  // Capture owner and owning refs to the compilation identity before clear()
  // nullifies them.  Only safe when the JIT context is still alive — during
  // shutdown the CodeRuntime slab may already be freed.
  CompiledFunctionOwner* saved_owner = owner_;
  Ref<> saved_code;
  Ref<> saved_builtins;
  Ref<> saved_globals;
  cinderx::ModuleState* mod_state = cinderx::getModuleState();
  if (saved_owner != nullptr && data_ != nullptr && data_->runtime != nullptr &&
      mod_state != nullptr && mod_state->jit_context != nullptr) {
    saved_code = Ref<>::create(data_->runtime->code());
    saved_builtins = Ref<>::create(data_->runtime->builtins());
    saved_globals = Ref<>::create(data_->runtime->globals());
  }

  clear();

  if (data_ == nullptr) {
    return;
  }

  if (contiguous_data_) {
    if (mod_state != nullptr && data_->code.data() != nullptr) {
      auto code_allocator = mod_state->code_allocator.get();
      if (code_allocator != nullptr) {
        code_allocator->releaseCode(const_cast<std::byte*>(data_->code.data()));
      }
    }
    data_->~CompiledFunctionData();
  } else if (saved_owner != nullptr && saved_code) {
    // Check that the Context is still alive via module state rather than
    // dereferencing saved_owner which may dangle after Context destruction.
    if (mod_state != nullptr && mod_state->jit_context != nullptr) {
      saved_owner->deferCompiledData(
          std::move(saved_code),
          std::move(saved_builtins),
          std::move(saved_globals),
          data_);
    } else {
      Py_DECREF(data_);
    }
  } else {
    Py_DECREF(data_);
  }
  data_ = nullptr;
}

bool associateFunctionWithCompiled(
    BorrowedRef<PyFunctionObject> func,
    BorrowedRef<CompiledFunction> compiled,
    bool is_nested) {
  if (_Py_IsImmortal(func)) {
    // The function can never be freed, so we can keep the CompiledFunction
    // alive
#if PY_VERSION_HEX >= 0x030E0000
    _Py_SetImmortalUntracked(compiled);
#else
    _Py_SetImmortal(compiled);
#endif
    return true;
  } else if (_Py_IsImmortal(compiled)) {
    // The CompiledFunction is already immortal, no need to associate it
    return true;
  }
  // Store a reference to the CompiledFunction in the function's __dict__.
  JIT_DCHECK(
      kCompiledFunctionKey != nullptr && kNestedCompiledFunctionsKey != nullptr,
      "failed to create compilation key");
  PyObject* func_dict = func->func_dict;
  if (func_dict == nullptr) {
    func_dict = PyDict_New();
    if (func_dict == nullptr) {
      return false;
    }
    func->func_dict = func_dict;
  }

  if (is_nested) {
    // Store CompiledFunction for nested function in outer function, we'll
    // also store it in the nested function's __dict__ when we do a
    // finalizeFunc on it.
    Ref<> nested_list = getDictRef(func_dict, kNestedCompiledFunctionsKey);
    if (nested_list == nullptr || !PyList_CheckExact(nested_list)) {
      if (PyErr_Occurred()) {
        return false;
      }
      nested_list = Ref<>::steal(PyList_New(0));
      if (nested_list == nullptr) {
        return false;
      } else if (
          PyDict_SetItem(func_dict, kNestedCompiledFunctionsKey, nested_list) <
          0) {
        return false;
      }
    }
    return PyList_Append(nested_list, compiled) == 0;
  }
  // Store the reference (this increfs the CompiledFunction).
  if (PyDict_SetItem(
          func_dict,
          kCompiledFunctionKey,
          reinterpret_cast<PyObject*>(compiled.get())) < 0) {
    return false;
  }

  // Add the function to the CompiledFunction's set.
  compiled->addFunction(func);

  return true;
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
  hir::HIRPrinter printer;
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
  // Store a borrowed reference to the function. The function is responsible
  // for removing itself via funcDestroyed() when it is deallocated.
  // We don't incref to avoid preventing garbage collection of functions
  // when multiple functions share the same CompiledFunction.
  functions_.insert(func.get());
}

void CompiledFunction::removeFunction(BorrowedRef<PyFunctionObject> func) {
  // Remove the borrowed reference. No decref needed since we don't own it.
  functions_.erase(func.get());
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
  } else if (data_->runtime != nullptr) {
    // During shutdown the Context (and its CodeRuntime slab) may be destroyed
    // while CFs still exist in function dicts.  Skip traversal if the JIT
    // context is gone.
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state != nullptr && mod_state->jit_context != nullptr) {
      return data_->runtime->traverse(visit, arg);
    }
  }

  return 0;
}

void CompiledFunction::clear(bool context_finalizing) {
  // During shutdown the Context (and its CodeRuntime slab) may be destroyed
  // while CFs still exist in function dicts.  Null out owner_ and runtime
  // to avoid dereferencing dangling pointers.
  if (owner_ != nullptr) {
    cinderx::ModuleState* mod_state = cinderx::getModuleState();
    if (mod_state == nullptr || mod_state->jit_context == nullptr) {
      owner_ = nullptr;
      if (data_ != nullptr) {
        data_->runtime = nullptr;
      }
    }
  }
  if (owner_ != nullptr) {
    if (!context_finalizing) {
      owner_->forgetCompiledFunction(*this);
    }
    if (data_ != nullptr) {
      // These will be cleared on their own if we're finalizing the context,
      // let's not complicate the iteration.
      for (auto& patcher : data_->code_patchers) {
        if (auto typed_patcher =
                dynamic_cast<TypeDeoptPatcher*>(patcher.get())) {
          owner_->unwatch(typed_patcher);
        }
      }
    }

    // We only de-opt the functions if we still have our owner. This is so that
    // our multi-threaded compile tests work properly where we are clearing
    // things with functions still running.
    auto funcs_to_deopt = std::move(functions_);

    // Deopt all associated functions. No decref needed since these are borrowed
    // refs.
    for (PyFunctionObject* func : funcs_to_deopt) {
      func->vectorcall = getInterpretedVectorcall(func);
    }

    owner_ = nullptr;
  }

  // Clear all references held by the CodeRuntime.
  if (data_ != nullptr && data_->runtime != nullptr) {
    data_->runtime->releaseReferences();
    data_->runtime = nullptr;
  }
}

} // namespace cinderx::jit
