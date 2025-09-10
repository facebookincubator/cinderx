# Copyright (c) Meta Platforms, Inc. and affiliates.

import dis
import opcode
import sys
import types
import unittest

import cinderx.test_support as cinder_support


def one():
    return 1


def _reassemble_for_jit(ops, shell_function):
    """
    Takes a list of (opcode: str, arg: int) pairs and and replaces the
    shell_function's code with these. Returns the shell_function
    wrapped up using the cinder_support functions failUnlessJITCompiled
    and fail_if_deopt. The shell function is modified.
    """
    code_list = []
    for op, arg in ops:
        code_list.append(opcode.opmap[op])
        code_list.append(arg)
    co = shell_function.__code__
    new_code = types.CodeType(
        co.co_argcount,
        co.co_posonlyargcount,
        co.co_kwonlyargcount,
        co.co_nlocals,
        co.co_stacksize,
        co.co_flags,
        bytes(code_list),
        co.co_consts,
        co.co_names,
        co.co_varnames,
        co.co_filename,
        co.co_name,
        co.co_qualname,
        co.co_firstlineno,
        co.co_linetable,
        co.co_exceptiontable,
        co.co_freevars,
        co.co_cellvars,
    )
    shell_function.__code__ = new_code
    f = cinder_support.failUnlessJITCompiled(shell_function)
    f = cinder_support.fail_if_deopt(f)
    return f


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

    def test_LOAD_FAST_BORROW_LOAD_FAST_BORROW(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            a = 1
            b = 2
            return a + b

        self.assertEqual(x(), 3)
        self._assertBytecodeContains(x, "LOAD_FAST_BORROW_LOAD_FAST_BORROW")

    def test_STORE_FAST_LOAD_FAST(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            return [i for i in [1, 2]]  # noqa: C416

        self.assertEqual(sum(x()), 3)
        self._assertBytecodeContains(x, "STORE_FAST_LOAD_FAST")

    def test_STORE_FAST_STORE_FAST(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            x, y = (1, 2)
            return x + y

        self.assertEqual(x(), 3)
        self._assertBytecodeContains(x, "STORE_FAST_STORE_FAST")

    def test_LOAD_FAST_LOAD_FAST(self):
        # Myself and LLMs tried really hard but we couldn't figure out a way to
        # write a function in pure Python that has LOAD_FAST_LOAD_FAST. It
        # still might be possible to generate though so here's a bytecode-level
        # test.
        def x(a, b):
            pass

        x_prime = _reassemble_for_jit(
            [
                ("RESUME", 0),
                ("LOAD_FAST_LOAD_FAST", 1),  # pushes a (arg 0) then b (arg 1)
                ("BUILD_TUPLE", 2),
                ("RETURN_VALUE", 0),
            ],
            x,
        )
        self.assertEqual(x_prime(1, 2), (1, 2))
        self._assertBytecodeContains(x, "LOAD_FAST_LOAD_FAST")

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

    def test_POP_ITER(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            for y in range(2):
                return y

        self.assertEqual(x(), 0)
        self._assertBytecodeContains(x, "POP_ITER")

    def test_BUILD_TEMPLATE(self):
        # Wrap this in an exec() to avoid breaking tests for earlier versions
        # of Python which don't support the new syntax.
        locals = {}
        exec(
            """
@cinder_support.fail_if_deopt
@cinder_support.failUnlessJITCompiled
def x():
    return t"foo"
""",
            globals(),
            locals,
        )
        x = locals["x"]

        t = x()
        self.assertEqual(t.strings, ("foo",))
        self.assertEqual(t.interpolations, ())
        self._assertBytecodeContains(x, "BUILD_TEMPLATE")

    def test_FORMAT_WITH_SPEC(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x():
            value = 42
            return f"{value:.2f}"

        self.assertEqual(x(), "42.00")
        self._assertBytecodeContains(x, "FORMAT_WITH_SPEC")

    def test_FORMAT_SIMPLE(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(forty_two):
            return f"{'42'} {42} {forty_two}"

        self.assertEqual(x("42"), "42 42 42")
        self._assertBytecodeContains(x, "FORMAT_SIMPLE")

    def test_BUILD_INTERPOLATION(self):
        # Wrap this in an exec() to avoid breaking tests for earlier versions
        # of Python which don't support the new syntax.
        locals = {"self": self}
        exec(
            """
from string.templatelib import Interpolation

@cinder_support.fail_if_deopt
@cinder_support.failUnlessJITCompiled
def x():
    return t"The value is {42} {42!r} {42.:.2f}"


t = x()
self.assertEqual(t.strings, ("The value is ", " ", " ", ""))
match t.interpolations:
    case (Interpolation(42, '42', None, ''), Interpolation(42, '42', 'r', ''), Interpolation(42.0, '42.', None, '.2f')):
        pass
    case _:
        self.fail(f"interpolations mismatch: {t.interpolations}")
self._assertBytecodeContains(x, "BUILD_INTERPOLATION")
""",
            globals(),
            locals,
        )

    def test_LOAD_COMMON_CONSTANT(self):
        @cinder_support.fail_if_deopt
        @cinder_support.failUnlessJITCompiled
        def x(a):
            assert a

        x(True)
        self._assertBytecodeContains(x, "LOAD_COMMON_CONSTANT")


if __name__ == "__main__":
    unittest.main()
