# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import asyncio
import gc
import sys
import traceback
import unittest

import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.jit import force_compile, is_jit_compiled, jit_suppress
from cinderx.test_support import skip_unless_jit

POLICY_DEPRECATED: bool = sys.version_info[:2] >= (3, 14)


def firstlineno(func):
    return func.__code__.co_firstlineno


def _outer(inner):
    return inner()


class GetFrameInFinalizer:
    def __del__(self):
        sys._getframe()


def _create_getframe_cycle():
    a = {"fg": GetFrameInFinalizer()}
    b = {"a": a}
    # pyrefly: ignore [bad-typed-dict-key]
    a["b"] = b
    return a


class TestException(Exception):
    # Tells pytest that this isn't a test case.
    __test__: bool = False


class GetFrameLineNumberTests(unittest.TestCase):
    def assert_code_and_lineno(self, frame, func, line_offset):
        self.assertEqual(frame.f_code, func.__code__)
        self.assertEqual(frame.f_lineno, firstlineno(func) + line_offset)

    def test_line_numbers(self) -> None:
        """Verify that line numbers are correct"""

        @cinder_support.failUnlessJITCompiled
        def g():
            return sys._getframe()

        self.assert_code_and_lineno(g(), g, 2)

    def test_line_numbers_for_running_generators(self) -> None:
        """Verify that line numbers are correct for running generator functions"""

        @cinder_support.failUnlessJITCompiled
        def g(x, y):
            yield sys._getframe()
            z = x + y
            yield sys._getframe()
            yield z

        gen = g(1, 2)
        frame = next(gen)
        self.assert_code_and_lineno(frame, g, 2)
        frame = next(gen)
        self.assert_code_and_lineno(frame, g, 4)
        self.assertEqual(next(gen), 3)

    def test_line_numbers_during_gen_throw(self) -> None:
        """Verify that line numbers are correct for suspended generator functions when
        an exception is thrown into them.
        """

        @cinder_support.failUnlessJITCompiled
        def f1(g):
            yield from g

        @cinder_support.failUnlessJITCompiled
        def f2(g):
            yield from g

        gen1, gen2 = None, None
        gen1_frame, gen2_frame = None, None

        @cinder_support.failUnlessJITCompiled
        def f3():
            nonlocal gen1_frame, gen2_frame
            try:
                yield "hello"
            except TestException:
                # pyrefly: ignore [missing-attribute]
                gen1_frame = gen1.gi_frame
                # pyrefly: ignore [missing-attribute]
                gen2_frame = gen2.gi_frame
                raise

        gen3 = f3()
        gen2 = f2(gen3)
        gen1 = f1(gen2)
        gen1.send(None)
        with self.assertRaises(TestException):
            gen1.throw(TestException())
        self.assert_code_and_lineno(gen1_frame, f1, 2)
        self.assert_code_and_lineno(gen2_frame, f2, 2)

    def test_line_numbers_from_finalizers(self) -> None:
        """Make sure we can get accurate line numbers from finalizers"""
        stack = []

        class StackGetter:
            def __del__(self):
                nonlocal stack
                stack = traceback.extract_stack()

        @cinder_support.failUnlessJITCompiled
        def double(x):
            ret = x
            tmp = StackGetter()
            del tmp
            ret += x
            return ret

        res = double(5)
        self.assertEqual(res, 10)
        self.assertEqual(stack[-1].lineno, firstlineno(StackGetter.__del__) + 2)
        self.assertEqual(stack[-2].lineno, firstlineno(double) + 4)

    @skip_unless_jit("Compares lineno behavior between JIT and interpreter")
    def test_line_numbers_after_jit_disabled(self):
        def f():
            frame = sys._getframe(0)
            return f"{frame.f_code.co_name}:{frame.f_lineno}"

        # Depending on which JIT mode is being used, f might not have been
        # compiled on the first call, but it will be after `force_compile`.
        force_compile(f)
        self.assertTrue(is_jit_compiled(f))
        compiled_out = f()

        with cinderx.jit.pause(deopt_all=True):
            self.assertFalse(is_jit_compiled(f))
            uncompiled_out = f()
            self.assertFalse(is_jit_compiled(f))

        expected_out = "f:7"

        self.assertTrue(compiled_out, expected_out)
        self.assertTrue(uncompiled_out, expected_out)


class GetFrameTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def f1(self, leaf):
        return self.f2(leaf)

    @cinder_support.failUnlessJITCompiled
    def f2(self, leaf):
        return self.f3(leaf)

    @cinder_support.failUnlessJITCompiled
    def f3(self, leaf):
        return leaf()

    def assert_frames(self, frame, names):
        actual = []
        for _ in range(len(names)):
            actual.append(frame.f_code.co_name)
            frame = frame.f_back

        self.assertEqual(names, actual)

    @cinder_support.failUnlessJITCompiled
    def simple_getframe(self):
        return sys._getframe()

    def test_simple_getframe(self) -> None:
        stack = ["simple_getframe", "f3", "f2", "f1", "test_simple_getframe"]
        frame = self.f1(self.simple_getframe)
        self.assert_frames(frame, stack)

    @cinder_support.failUnlessJITCompiled
    def consecutive_getframe(self):
        f1 = sys._getframe()
        f2 = sys._getframe()
        return f1, f2

    @cinder_support.failUnlessJITCompiled
    def test_consecutive_getframe(self) -> None:
        stack = ["consecutive_getframe", "f3", "f2", "f1", "test_consecutive_getframe"]
        frame1, frame2 = self.f1(self.consecutive_getframe)
        self.assert_frames(frame1, stack)
        # Make sure the second call to sys._getframe doesn't rematerialize
        # frames
        for _ in range(4):
            self.assertTrue(frame1 is frame2)
            frame1 = frame1.f_back
            frame2 = frame2.f_back

    @cinder_support.failUnlessJITCompiled
    def getframe_then_deopt(self):
        f = sys._getframe()
        try:
            raise Exception("testing 123")
        except:  # noqa: B001
            return f

    def test_getframe_then_deopt(self) -> None:
        # Make sure we correctly unlink a materialized frame after its function
        # deopts into the interpreter
        stack = ["getframe_then_deopt", "f3", "f2", "f1", "test_getframe_then_deopt"]
        frame = self.f1(self.getframe_then_deopt)
        self.assert_frames(frame, stack)

    @cinder_support.failUnlessJITCompiled
    def getframe_in_except(self):
        try:
            raise Exception("testing 123")
        except:  # noqa: B001
            return sys._getframe()

    def test_getframe_after_deopt(self) -> None:
        stack = ["getframe_in_except", "f3", "f2", "f1", "test_getframe_after_deopt"]
        frame = self.f1(self.getframe_in_except)
        self.assert_frames(frame, stack)

    class FrameGetter:
        def __init__(self, box):
            self.box = box

        def __del__(self):
            self.box[0] = sys._getframe()

    def do_raise(self, x):
        # Clear reference held by frame in the traceback that gets created with
        # the exception
        del x
        raise Exception("testing 123")

    @cinder_support.failUnlessJITCompiled
    def getframe_in_dtor_after_deopt(self):
        ref = ["notaframe"]
        frame_getter = self.FrameGetter(ref)  # noqa: F841
        try:
            raise Exception("testing 123")
        except:  # noqa: B001
            return ref

    def test_getframe_in_dtor_after_deopt(self) -> None:
        # Test that we can correctly walk the stack in the interpreter after
        # deopting but before returning to the caller
        frame = self.f1(self.getframe_in_dtor_after_deopt)[0]
        stack = ["__del__", "f3", "f2", "f1", "test_getframe_in_dtor_after_deopt"]
        self.assert_frames(frame, stack)

    def test_get_frame(self):
        def f():
            return sys._getframe()

        def g():
            return f()

        x = g()
        self.assertEqual(x.f_lineno, f.__code__.co_firstlineno + 1)


class GetGenFrameDuringThrowTest(unittest.TestCase):
    def setUp(self) -> None:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        self.loop = loop

    def tearDown(self):
        self.loop.close()
        if not POLICY_DEPRECATED:
            asyncio.set_event_loop_policy(None)

    @cinder_support.failUnlessJITCompiled
    async def outer_propagates_exc(self, inner):
        return await inner

    @cinder_support.failUnlessJITCompiled
    async def outer_handles_exc(self, inner):
        try:
            await inner
        except TestException:
            return 123

    async def inner(self, fut, outer_box):
        try:
            await fut
        except TestException:
            outer_coro = outer_box[0]
            outer_coro.cr_frame
            raise

    def run_test(self, outer_func):
        box = [None]
        fut = asyncio.Future()
        inner = self.inner(fut, box)
        outer = outer_func(inner)
        box[0] = outer
        outer.send(None)
        return outer.throw(TestException())

    def test_unhandled_exc(self) -> None:
        with self.assertRaises(TestException):
            self.run_test(self.outer_propagates_exc)

    def test_handled_exc(self) -> None:
        with self.assertRaises(StopIteration) as cm:
            self.run_test(self.outer_handles_exc)
        self.assertEqual(cm.exception.value, 123)
