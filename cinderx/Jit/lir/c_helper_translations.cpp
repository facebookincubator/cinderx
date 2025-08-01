// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/lir/c_helper_translations.h"

#include "cinderx/Jit/jit_rt.h"

#include <fmt/format.h>

namespace jit::lir {

const std::string* mapCHelperToLIR(uint64_t addr) {
  static const std::unordered_map<uint64_t, std::string> mapping = {
      {reinterpret_cast<uint64_t>(JITRT_Cast),
       fmt::format(
           R"(Function:
BB %0 - succs: %2 %1
       %5:Object = LoadArg 0(0x0):Object
       %6:Object = LoadArg 1(0x1):Object
       %7:Object = Move [%5:Object + {0:#x}]:Object
       %8:Object = Equal %7:Object, %6:Object
                   CondBranch %8:Object

BB %1 - preds: %0 - succs: %2 %3
      %10:Object = Call PyType_IsSubtype, %7:Object, %6:Object
                   CondBranch %10:Object

BB %2 - preds: %0 %1 - succs: %4
                   Return %5:Object

BB %3 - preds: %1 - succs: %4
      %13:Object = Move [%7:Object + {1:#x}]:Object
      %14:Object = Move [%6:Object + {1:#x}]:Object
                   Call PyErr_Format, PyExc_TypeError, "expected '%s', got '%s'", %14:Object, %13:Object
      %16:Object = Move 0(0x0):Object
                   Return %16:Object

BB %4 - preds: %2 %3
)",
           offsetof(PyObject, ob_type),
           offsetof(PyTypeObject, tp_name))}};

  auto it = mapping.find(addr);
  return it != mapping.end() ? &it->second : nullptr;
}

} // namespace jit::lir
