# Copyright (c) Meta Platforms, Inc. and affiliates.

import dis
import sys
import unittest

import cinderx.test_support as cinder_support


def one():
    return 1


@unittest.skipUnless(sys.version_info >= (3, 14), "Python 3.14+ only")
class Python314Bytecodes(unittest.TestCase):
    def _assertBytecodeContains(self, func, expected_opcode):
        try:
            inner_function = func.inner_function
        except AttributeError:
            pass
        else:
            func = inner_function
        bytecode_instructions = list(dis.get_instructions(func))
        opcodes = [instr.opname for instr in bytecode_instructions]

        self.assertIn(
            expected_opcode,
            opcodes,
            f"{expected_opcode} opcode should be present in {func.__name__} bytecode",
        )

    def test_LOAD_SMALL_INT(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return 1

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "LOAD_SMALL_INT")

    def test_BINARY_OP_oparg_SUBSCR(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return [1, 2][0]

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "BINARY_OP")

    def test_LOAD_ATTR_CALL_unbound(self):
        class C:
            def m(self, v):
                return 1 + v

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(o):
            return o.m(2)

        self.assertEqual(x(C()), 3)
        self._assertBytecodeContains(x, "LOAD_ATTR")
        self._assertBytecodeContains(x, "CALL")

    def test_LOAD_ATTR_CALL_bound(self):
        class C:
            def m(self, v):
                return 1 + v

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(o):
            return o.m(2)

        self.assertEqual(x(C()), 3)
        self._assertBytecodeContains(x, "LOAD_ATTR")
        self._assertBytecodeContains(x, "CALL")

    def test_LOAD_ATTR_CALL_bound_via_attr(self):
        class C:
            def m(self, v):
                return 1 + v

            def __getattr__(self, name):
                return self.m  # Returns bound method

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(o):
            return o.m_attr(2)

        self.assertEqual(x(C()), 3)
        self._assertBytecodeContains(x, "LOAD_ATTR")
        self._assertBytecodeContains(x, "CALL")

    def test_LOAD_ATTR_CALL_err(self):
        class C:
            def m(self, v):
                return 1 + v

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(o):
            return o.err(2)

        with self.assertRaises(AttributeError):
            x(C())

        self._assertBytecodeContains(x, "LOAD_ATTR")
        self._assertBytecodeContains(x, "CALL")

    def test_DICT_MERGE(self):
        def y(i=1, j=2):
            return i * j

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(k):
            return y(i=2, **k)

        self.assertEqual(x({"j": 3}), 6)
        self._assertBytecodeContains(x, "DICT_MERGE")

    def test_LOAD_SUPER_ATTR_CALL_bound(self):
        class A:
            attr = lambda _: 1  # noqa: E731

        class B(A):
            pass

            @cinder_support.fail_if_deopt
            @cinder_support.failUnlessJITCompiled
            def m(self):
                return super().attr()

        self.assertEqual(B().m(), 1)
        self._assertBytecodeContains(B.m, "LOAD_SUPER_ATTR")
        self._assertBytecodeContains(B.m, "CALL")

    def test_LOAD_GLOBAL_CALL(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def f():
            return one()

        self.assertEqual(f(), 1)
        self._assertBytecodeContains(f, "LOAD_GLOBAL")
        self._assertBytecodeContains(f, "CALL")


if __name__ == "__main__":
    unittest.main()
