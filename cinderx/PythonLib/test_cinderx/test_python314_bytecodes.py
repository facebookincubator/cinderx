# Copyright (c) Meta Platforms, Inc. and affiliates.

import dis
import sys
import unittest

import cinderx.test_support as cinder_support


def one():
    return 1


@unittest.skipUnless(sys.version_info >= (3, 14), "Python 3.14+ only")
class Python314Bytecodes(unittest.TestCase):
    def _assertBytecodeContains(self, func, expected_opcode, expected_oparg=None):
        try:
            inner_function = func.inner_function
        except AttributeError:
            pass
        else:
            func = inner_function

        bytecode_instructions = dis.get_instructions(func)

        if expected_oparg is None:
            opcodes = [instr.opname for instr in bytecode_instructions]
            self.assertIn(
                expected_opcode,
                opcodes,
                f"{expected_opcode} opcode should be present in {func.__name__} bytecode",
            )
        else:
            matching_instructions = [
                instr
                for instr in bytecode_instructions
                if instr.opname == expected_opcode and instr.arg == expected_oparg
            ]
            self.assertTrue(
                len(matching_instructions) > 0,
                f"{expected_opcode} opcode with oparg {expected_oparg} should be present in {func.__name__} bytecode",
            )

    def test_LOAD_SMALL_INT(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return 1

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "LOAD_SMALL_INT")

    def test_LOAD_FAST_BORROW(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            a = 1
            return a

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "LOAD_FAST_BORROW")

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

    def test_CALL_INTRINSIC_1(self):
        def y(*args):
            return sum(args)

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(*args):
            return y(1, *args)

        self.assertEqual(x(2), 3)

        self._assertBytecodeContains(x, "CALL_INTRINSIC_1")

    def test_CALL_INTRINSIC_2(self):
        # This test actually only tests JIT compilation of CALL_INTRINSIC_2.
        # The code below will deopt when the exception is raised so the
        # CALL_INTRINSIC_2 will never be executed by the JIT.
        #
        # In theory we should be able execute CALL_INTRINSIC_2 in the JIT but I
        # don't think it's possible for us to create a function which will do
        # this with 3.14. CALL_INTRINSIC_2 is mostly used as part of evaluating
        # generic type args and in handling exceptions, neither of which we do
        # in the JIT.

        # Wrap this in an exec() to avoid breaking tests for earlier versions
        # of Python which don't support the new syntax.
        locals = {}
        exec(
            """
@cinder_support.failUnlessJITCompiled
def x():
    try:
        raise ExceptionGroup(
            "test", [ValueError("error1"), TypeError("error2")]
        )
    except* ValueError:
        pass
    except* TypeError:
        pass

        """,
            globals(),
            locals,
        )
        x = locals["x"]

        try:
            x()
        except ExceptionGroup:
            pass
        self._assertBytecodeContains(x, "CALL_INTRINSIC_2")

    def test_COMPARE_OP(self):
        # This is just a very quick test to verify we're now basically using
        # the correct comparison operations. The comparison operator value was
        # off by 1 bit before we took into account the new coercion flag.

        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return 1 == 1

        self.assertEqual(x(), True)
        self._assertBytecodeContains(x, "COMPARE_OP")

    def test_COMPARE_OP_with_bool_coercion(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            if 1 == 2:
                return 2
            return 1

        self.assertEqual(x(), 1)
        self._assertBytecodeContains(x, "COMPARE_OP", 88)  # bit 5 set

    def test_COMPARE_OP_without_bool_coercion(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return 1 == 2

        self.assertEqual(x(), False)
        self._assertBytecodeContains(x, "COMPARE_OP", 72)  # bit 5 not set

    def test_TO_BOOL_and_POP_JUMP_IF_FALSE(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(a):
            if a:
                return 1

        self.assertEqual(x(2), 1)
        self._assertBytecodeContains(x, "TO_BOOL")
        self._assertBytecodeContains(x, "POP_JUMP_IF_FALSE")


if __name__ == "__main__":
    unittest.main()
