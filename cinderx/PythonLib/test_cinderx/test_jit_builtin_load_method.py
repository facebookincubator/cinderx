# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import datetime
import unittest

from cinderx.test_support import failUnlessJITCompiled


# A module-level constant whose type is an immutable *heap* type: datetime.date
# has Py_TPFLAGS_IMMUTABLETYPE set, is not a _Py_TPFLAGS_STATIC_BUILTIN, and has
# tp_dictoffset == 0. Referencing it as a global lets the JIT specialize the
# method-call receiver to an exact type, which routes BuiltinLoadMethodElimination
# through its immutableMultithreadedTypeLookup() path (as opposed to the static
# builtin cache used for str/bytes/etc.).
_IMMUTABLE_HEAP_CONST: datetime.date = datetime.date(2020, 1, 1)


class BuiltinLoadMethodEliminationTest(unittest.TestCase):
    def test_unknown_method_on_immutable_heap_type_does_not_crash(self) -> None:
        # Regression test: compiling a call to a non-existent method on an
        # exact-typed immutable-heap receiver used to SIGSEGV the JIT inside
        # BuiltinLoadMethodElimination -- immutableMultithreadedTypeLookup()
        # returns null for an unresolved name, and getMethodObjectFromType()
        # then called Py_TYPE() on that null pointer. force_compile() (via the
        # decorator) is what triggers the crash, before the function ever runs.
        @failUnlessJITCompiled
        def call_unknown() -> object:
            # pyre-ignore[16]: The missing method is the whole point.
            return _IMMUTABLE_HEAP_CONST.this_method_does_not_exist()

        # Compilation survived. The optimization correctly bails on the
        # unresolved method, so the call still raises AttributeError at runtime.
        with self.assertRaises(AttributeError):
            call_unknown()
