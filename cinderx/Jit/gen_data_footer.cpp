// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/gen_data_footer.h"

#include "cinderx/Common/py-portability.h"
#include "cinderx/module_state.h"

namespace jit {
#if PY_VERSION_HEX >= 0x030C0000
GenDataFooter** jitGenDataFooterPtr(PyGenObject* gen) {
  // TASK(T209501671): This has way too much going on. If we made PyGenObject
  // use PyObject_VAR_HEAD like it probably should this would get simpler. If
  // we expanded the allocation to include the GenDataFooter it'd get simpler
  // still.
  _PyInterpreterFrame* gen_frame = generatorFrame(gen);
  BorrowedRef<PyCodeObject> gen_code = _PyFrame_GetCode(gen_frame);
  BorrowedRef<PyTypeObject> gen_type = cinderx::getModuleState()->genType();

  size_t python_frame_data_bytes =
      _PyFrame_NumSlotsForCodeObject(gen_code) * gen_type->tp_itemsize;
  // A *pointer* to JIT data comes after all the other data in the default
  // generator object.
  return reinterpret_cast<GenDataFooter**>(
      reinterpret_cast<uintptr_t>(gen) + gen_type->tp_basicsize +
      python_frame_data_bytes);
}

GenDataFooter* jitGenDataFooter(PyGenObject* gen) {
  return *jitGenDataFooterPtr(gen);
}
#endif
} // namespace jit
