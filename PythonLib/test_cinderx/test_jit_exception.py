# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import unittest

import cinderx.test_support as cinder_support
from .common import failUnlessHasOpcodes


POST_311 = sys.version_info >= (3, 11)

# Opcode to look for when inspecting code objects that use try/except/finally.
EXN_OPCODE = "PUSH_EXC_INFO" if POST_311 else "SETUP_FINALLY"


class Err1(Exception):
    pass


class Err2(Exception):
    pass


class DummyContainer:
    def __len__(self):
        raise Exception("hello!")


class ExceptionInConditional(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def doit(self, x):
        if x:
            return 1
        return 2

    def test_exception_thrown_in_conditional(self):
        with self.assertRaisesRegex(Exception, "hello!"):
            self.doit(DummyContainer())


class ExceptionHandlingTests(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def try_except(self, func):
        try:
            func()
        except:
            return True
        return False

    def test_raise_and_catch(self):
        def f():
            raise Exception("hello")

        self.assertTrue(self.try_except(f))

        def g():
            pass

        self.assertFalse(self.try_except(g))

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def catch_multiple(self, func):
        try:
            func()
        except Err1:
            return 1
        except Err2:
            return 2

    def test_multiple_except_blocks(self):
        def f():
            raise Err1("err1")

        self.assertEqual(self.catch_multiple(f), 1)

        def g():
            raise Err2("err2")

        self.assertEqual(self.catch_multiple(g), 2)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def reraise(self, func):
        try:
            func()
        except:
            raise

    def test_reraise(self):
        def f():
            raise Exception("hello")

        with self.assertRaisesRegex(Exception, "hello"):
            self.reraise(f)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def try_except_in_loop(self, niters, f):
        for i in range(niters):
            try:
                try:
                    f(i)
                except Err2:
                    pass
            except Err1:
                break
        return i

    def test_try_except_in_loop(self):
        def f(i):
            if i == 10:
                raise Err1("hello")

        self.assertEqual(self.try_except_in_loop(20, f), 10)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def nested_try_except(self, f):
        try:
            try:
                try:
                    f()
                except:
                    raise
            except:
                raise
        except:
            return 100

    def test_nested_try_except(self):
        def f():
            raise Exception("hello")

        self.assertEqual(self.nested_try_except(f), 100)

    @cinder_support.failUnlessJITCompiled
    def try_except_in_generator(self, f):
        try:
            yield f(0)
            yield f(1)
            yield f(2)
        except:
            yield 123

    def test_except_in_generator(self):
        def f(i):
            if i == 1:
                raise Exception("hello")
            return

        g = self.try_except_in_generator(f)
        next(g)
        self.assertEqual(next(g), 123)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE, "RERAISE")
    def try_finally(self, should_raise):
        result = None
        try:
            if should_raise:
                raise Exception("testing 123")
        finally:
            result = 100
        return result

    def test_try_finally(self):
        self.assertEqual(self.try_finally(False), 100)
        with self.assertRaisesRegex(Exception, "testing 123"):
            self.try_finally(True)

    @cinder_support.failUnlessJITCompiled
    def try_except_finally(self, should_raise):
        result = None
        try:
            if should_raise:
                raise Exception("testing 123")
        except Exception:
            result = 200
        finally:
            if result is None:
                result = 100
        return result

    def test_try_except_finally(self):
        self.assertEqual(self.try_except_finally(False), 100)
        self.assertEqual(self.try_except_finally(True), 200)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def return_in_finally(self, v):
        try:
            pass
        finally:
            return v

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def return_in_finally2(self, v):
        try:
            return v
        finally:
            return 100

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def return_in_finally3(self, v):
        try:
            1 / 0
        finally:
            return v

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def return_in_finally4(self, v):
        try:
            return 100
        finally:
            try:
                1 / 0
            finally:
                return v

    def test_return_in_finally(self):
        self.assertEqual(self.return_in_finally(100), 100)
        self.assertEqual(self.return_in_finally2(200), 100)
        self.assertEqual(self.return_in_finally3(300), 300)
        self.assertEqual(self.return_in_finally4(400), 400)

    @cinder_support.failUnlessJITCompiled
    def break_in_finally_after_return(self, x):
        for count in [0, 1]:
            count2 = 0
            while count2 < 20:
                count2 += 10
                try:
                    return count + count2
                finally:
                    if x:
                        break
        return "end", count, count2

    @cinder_support.failUnlessJITCompiled
    def break_in_finally_after_return2(self, x):
        for count in [0, 1]:
            for count2 in [10, 20]:
                try:
                    return count + count2
                finally:
                    if x:
                        break
        return "end", count, count2

    def test_break_in_finally_after_return(self):
        self.assertEqual(self.break_in_finally_after_return(False), 10)
        self.assertEqual(self.break_in_finally_after_return(True), ("end", 1, 10))
        self.assertEqual(self.break_in_finally_after_return2(False), 10)
        self.assertEqual(self.break_in_finally_after_return2(True), ("end", 1, 10))

    @cinder_support.failUnlessJITCompiled
    def continue_in_finally_after_return(self, x):
        count = 0
        while count < 100:
            count += 1
            try:
                return count
            finally:
                if x:
                    continue
        return "end", count

    @cinder_support.failUnlessJITCompiled
    def continue_in_finally_after_return2(self, x):
        for count in [0, 1]:
            try:
                return count
            finally:
                if x:
                    continue
        return "end", count

    def test_continue_in_finally_after_return(self):
        self.assertEqual(self.continue_in_finally_after_return(False), 1)
        self.assertEqual(self.continue_in_finally_after_return(True), ("end", 100))
        self.assertEqual(self.continue_in_finally_after_return2(False), 0)
        self.assertEqual(self.continue_in_finally_after_return2(True), ("end", 1))

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def return_in_loop_in_finally(self, x):
        try:
            for _ in [1, 2, 3]:
                if x:
                    return x
        finally:
            pass
        return 100

    def test_return_in_loop_in_finally(self):
        self.assertEqual(self.return_in_loop_in_finally(True), True)
        self.assertEqual(self.return_in_loop_in_finally(False), 100)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def conditional_return_in_finally(self, x, y, z):
        try:
            if x:
                return x
            if y:
                return y
        finally:
            pass
        return z

    def test_conditional_return_in_finally(self):
        self.assertEqual(self.conditional_return_in_finally(100, False, False), 100)
        self.assertEqual(self.conditional_return_in_finally(False, 200, False), 200)
        self.assertEqual(self.conditional_return_in_finally(False, False, 300), 300)

    @cinder_support.failUnlessJITCompiled
    @failUnlessHasOpcodes(EXN_OPCODE)
    def nested_finally(self, x):
        try:
            if x:
                return x
        finally:
            try:
                y = 10
            finally:
                z = y
        return z

    def test_nested_finally(self):
        self.assertEqual(self.nested_finally(100), 100)
        self.assertEqual(self.nested_finally(False), 10)
