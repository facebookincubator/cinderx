# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from collections.abc import Callable

from cinderx.test_support import failUnlessJITCompiled

from .common import failUnlessHasOpcodes


class Err1(Exception):
    pass


class Err2(Exception):
    pass


class DummyContainer:
    def __len__(self) -> int:
        raise Exception("hello!")


class ExceptionInConditional(unittest.TestCase):
    @failUnlessJITCompiled
    def doit(self, x: object) -> int:
        if x:
            return 1
        return 2

    def test_exception_thrown_in_conditional(self) -> None:
        with self.assertRaisesRegex(Exception, "hello!"):
            self.doit(DummyContainer())


class ExceptionHandlingTests(unittest.TestCase):
    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def try_except(self, func: Callable[[], None]) -> bool:
        try:
            func()
        except:  # noqa: B001
            return True
        return False

    def test_raise_and_catch(self) -> None:
        def f() -> None:
            raise Exception("hello")

        self.assertTrue(self.try_except(f))

        def g() -> None:
            pass

        self.assertFalse(self.try_except(g))

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def catch_multiple(self, func: Callable[[], None]) -> int | None:
        try:
            func()
        except Err1:
            return 1
        except Err2:
            return 2

    def test_multiple_except_blocks(self) -> None:
        def f() -> None:
            raise Err1("err1")

        self.assertEqual(self.catch_multiple(f), 1)

        def g() -> None:
            raise Err2("err2")

        self.assertEqual(self.catch_multiple(g), 2)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def reraise(self, func: Callable[[], None]) -> None:
        try:
            func()
        except:  # noqa: B001
            raise

    def test_reraise(self) -> None:
        def f() -> None:
            raise Exception("hello")

        with self.assertRaisesRegex(Exception, "hello"):
            self.reraise(f)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def try_except_in_loop(self, niters: int, f: Callable[[int], None]) -> int:
        for i in range(niters):
            try:
                try:
                    f(i)
                except Err2:
                    pass
            except Err1:
                break
        return i

    def test_try_except_in_loop(self) -> None:
        def f(i: int) -> None:
            if i == 10:
                raise Err1("hello")

        self.assertEqual(self.try_except_in_loop(20, f), 10)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def nested_try_except(self, f: Callable[[], None]) -> int | None:
        try:
            try:
                try:
                    f()
                except:  # noqa: B001
                    raise
            except:  # noqa: B001
                raise
        except:  # noqa: B001
            return 100

    def test_nested_try_except(self) -> None:
        def f() -> None:
            raise Exception("hello")

        self.assertEqual(self.nested_try_except(f), 100)

    @failUnlessJITCompiled
    def try_except_in_generator(self, f):
        try:
            yield f(0)
            yield f(1)
            yield f(2)
        except:  # noqa: B001
            yield 123

    def test_except_in_generator(self) -> None:
        def f(i: int) -> None:
            if i == 1:
                raise Exception("hello")
            return

        g = self.try_except_in_generator(f)
        next(g)
        self.assertEqual(next(g), 123)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO", "RERAISE")
    def try_finally(self, should_raise: bool) -> int | None:
        result = None
        try:
            if should_raise:
                raise Exception("testing 123")
        finally:
            result = 100
        return result

    def test_try_finally(self) -> None:
        self.assertEqual(self.try_finally(False), 100)
        with self.assertRaisesRegex(Exception, "testing 123"):
            self.try_finally(True)

    @failUnlessJITCompiled
    def try_except_finally(self, should_raise: bool) -> int | None:
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

    def test_try_except_finally(self) -> None:
        self.assertEqual(self.try_except_finally(False), 100)
        self.assertEqual(self.try_except_finally(True), 200)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def return_in_finally(self, v: int) -> int:
        try:
            pass
        finally:
            # pyrefly: ignore [invalid-syntax]
            return v  # noqa: B012

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def return_in_finally2(self, v: int) -> int:
        try:
            return v
        finally:
            # pyrefly: ignore [invalid-syntax]
            return 100  # noqa: B012

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def return_in_finally3(self, v: int) -> int:
        try:
            1 / 0
        finally:
            # pyrefly: ignore [invalid-syntax]
            return v  # noqa: B012

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def return_in_finally4(self, v: int) -> int:
        try:
            return 100
        finally:
            try:
                1 / 0
            finally:
                # pyrefly: ignore [invalid-syntax]
                return v  # noqa: B012

    def test_return_in_finally(self) -> None:
        self.assertEqual(self.return_in_finally(100), 100)
        self.assertEqual(self.return_in_finally2(200), 100)
        self.assertEqual(self.return_in_finally3(300), 300)
        self.assertEqual(self.return_in_finally4(400), 400)

    @failUnlessJITCompiled
    def break_in_finally_after_return(self, x: bool) -> int | tuple[str, int, int]:
        for count in [0, 1]:
            count2 = 0
            while count2 < 20:
                count2 += 10
                try:
                    return count + count2
                finally:
                    if x:
                        # pyrefly: ignore [invalid-syntax]
                        break  # noqa: B012
        return "end", count, count2

    @failUnlessJITCompiled
    def break_in_finally_after_return2(self, x: bool) -> int | tuple[str, int, int]:
        for count in [0, 1]:
            for count2 in [10, 20]:
                try:
                    return count + count2
                finally:
                    if x:
                        # pyrefly: ignore [invalid-syntax]
                        break  # noqa: B012
        return "end", count, count2

    def test_break_in_finally_after_return(self) -> None:
        self.assertEqual(self.break_in_finally_after_return(False), 10)
        self.assertEqual(self.break_in_finally_after_return(True), ("end", 1, 10))
        self.assertEqual(self.break_in_finally_after_return2(False), 10)
        self.assertEqual(self.break_in_finally_after_return2(True), ("end", 1, 10))

    @failUnlessJITCompiled
    def continue_in_finally_after_return(self, x: bool) -> int | tuple[str, int]:
        count = 0
        while count < 100:
            count += 1
            try:
                return count
            finally:
                if x:
                    # pyrefly: ignore [invalid-syntax]
                    continue  # noqa: B012
        return "end", count

    @failUnlessJITCompiled
    def continue_in_finally_after_return2(self, x: bool) -> int | tuple[str, int]:
        for count in [0, 1]:
            try:
                return count
            finally:
                if x:
                    # pyrefly: ignore [invalid-syntax]
                    continue  # noqa: B012
        return "end", count

    def test_continue_in_finally_after_return(self) -> None:
        self.assertEqual(self.continue_in_finally_after_return(False), 1)
        self.assertEqual(self.continue_in_finally_after_return(True), ("end", 100))
        self.assertEqual(self.continue_in_finally_after_return2(False), 0)
        self.assertEqual(self.continue_in_finally_after_return2(True), ("end", 1))

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def return_in_loop_in_finally(self, x: bool) -> bool | int:
        try:
            for _ in [1, 2, 3]:
                if x:
                    return x
        finally:
            pass
        return 100

    def test_return_in_loop_in_finally(self) -> None:
        self.assertEqual(self.return_in_loop_in_finally(True), True)
        self.assertEqual(self.return_in_loop_in_finally(False), 100)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def conditional_return_in_finally(
        self, x: int | bool, y: int | bool, z: int | bool
    ) -> int | bool:
        try:
            if x:
                return x
            if y:
                return y
        finally:
            pass
        return z

    def test_conditional_return_in_finally(self) -> None:
        self.assertEqual(self.conditional_return_in_finally(100, False, False), 100)
        self.assertEqual(self.conditional_return_in_finally(False, 200, False), 200)
        self.assertEqual(self.conditional_return_in_finally(False, False, 300), 300)

    @failUnlessJITCompiled
    @failUnlessHasOpcodes("PUSH_EXC_INFO")
    def nested_finally(self, x: int | bool) -> int | bool:
        try:
            if x:
                return x
        finally:
            try:
                y = 10
            finally:
                z = y
        return z

    def test_nested_finally(self) -> None:
        self.assertEqual(self.nested_finally(100), 100)
        self.assertEqual(self.nested_finally(False), 10)
