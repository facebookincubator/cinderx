# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import unittest
from re import escape

from .common import StaticTestBase


class ClassDecoratorTests(StaticTestBase):
    def test_class_decl_simple(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec(cls: TClass) -> TClass:
                cls.my_tag = 42
                return cls

            @mydec
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """

        code = self.compile(codestr, modname="foo")
        x = self.find_code(self.find_code(code, "C"), "__init__")
        self.assertInBytecode(x, "STORE_FIELD")
        with self.in_module(codestr, name="mymod") as mod:
            C = mod.C
            self.assertEqual(C.my_tag, 42)

    def test_class_decl_params(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec(x: str) -> ClassDecorator:
                def mydec(cls: TClass) -> TClass:
                    cls.foo = x
                    return cls
                return mydec

            @mydec('foo')
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """

        code = self.compile(codestr, modname="foo")
        x = self.find_code(self.find_code(code, "C"), "__init__")
        self.assertInBytecode(x, "STORE_FIELD")
        with self.in_module(codestr, name="mymod") as mod:
            C = mod.C
            self.assertEqual(C.foo, "foo")

    def test_class_decl_not_nested(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def decfunc(cls: TClass) -> TClass:
                return cls
            def mydec() -> ClassDecorator:
                return decfunc

            @mydec()
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """

        # Should be supported but isn't, we need to have each function treated as its
        # own type, or do assignment compatibility based upon values, not types. For
        # nested functions we treat each function as its own type.
        self.type_error(
            codestr,
            r"mismatched types: expected ClassDecorator because of return type, found types.FunctionType instead",
            at="return decfunc",
        )

    def test_class_decl_wrong_return_type(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec(cls: TClass) -> ClassDecorator:
                return None

            @mydec
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """
        self.type_error(
            codestr,
            r"mismatched types: expected ClassDecorator because of return type, found None instead",
            at="return None",
        )

    def test_class_decl_wrong_return_type_nested(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec() -> ClassDecorator:
                def mydec(cls: TClass) -> TClass:
                    return None
                return mydec

            @mydec
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """
        self.type_error(
            codestr,
            r"mismatched types: expected TClass because of return type, found None instead",
            at="return None",
        )

    def test_class_decl_different_tclass(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def f(x: TClass|None = None) -> TClass|None:
                return None

            def mydec(reason: str) -> ClassDecorator:
                def mydec(cls: TClass) -> TClass:
                    x = f(int)
                    if x is not None:
                        return x
                    return cls
                return mydec

            @mydec('foo')
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """
        self.type_error(
            codestr,
            escape(
                r"mismatched types: expected TClass because of return type, found Type[type] instead"
            ),
            at="return x",
        )

    def test_class_decl_different_tclass_nested(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec() -> object:
                def f(x: TClass|None = None) -> TClass|None:
                    return None

                def mydec(cls: TClass) -> TClass:
                    x = f(42)
                    if x is not None:
                        return x
                    return cls
                return mydec

            @mydec()
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """

        # We don't track nested functions well enough, so this all
        # gets treated as dynamic, and we don't compile the class dynamically.
        code = self.compile(codestr, modname="foo")
        x = self.find_code(self.find_code(code, "C"), "__init__")
        self.assertNotInBytecode(x, "STORE_FIELD")

    def test_class_decl_different_tclass_recursive(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec() -> object:
                def decfunc(cls: TClass) -> TClass:
                    x = decfunc(int)
                    if x is not None:
                        return x
                    return cls
                return decfunc

            @mydec
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """
        # This should really error out w/ an incompatible return type, but
        # the recursive function isn't currently seen.
        self.type_error(
            codestr,
            r"Name `decfunc` is not defined",
            at="decfunc(int)",
        )

    def test_class_decl_wrong_return_type_nested_bad_mutator_func(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec() -> ClassDecorator:
                def decfunc(cls: object) -> object:
                    return 42
                return decfunc

            @mydec
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """
        self.type_error(
            codestr,
            r"mismatched types: expected ClassDecorator because of return type, found mydec.decfunc instead",
            at="return decfunc",
        )

    def test_class_decl_multiple_params(self) -> None:
        codestr = """
            from __static__ import ClassDecorator, TClass

            def mydec(x: str) -> ClassDecorator:
                def mydec(cls: TClass, y: TClass = int) -> TClass:
                    cls.foo = x
                    return cls
                return mydec

            @mydec('foo')
            class C:
                def __init__(self) -> None:
                    self.x: int = 42
        """

        self.type_error(
            codestr,
            escape(r"type mismatch: Type[type] cannot be assigned to TClass"),
            at="int) -> TClass:",
        )

    def test_tclass_bound(self) -> None:
        codestr = """
            from __static__ import TClass

            def mydec(cls: TClass) -> TClass:
                cls.my_tag = 42
                return cls

            mydec(42)
        """

        self.type_error(
            codestr,
            escape(
                r"type mismatch: Literal[42] received for positional arg 'cls', expected TClass"
            ),
            at="42)",
        )

    def test_nested_function_not_typed(self) -> None:
        # We shouldn't type check the assignment from `func` to `inner_func`
        # within the function, it should be treated as dynamic.
        codestr = """
            from __static__ import TClass

            def f(func, use_func: bool) -> int:
                if not use_func:
                    def inner_func() -> int:
                        return 42
                else:
                    inner_func = func

                return inner_func()

            x = f(lambda: 100, True)
        """

        with self.in_module(codestr, name="mymod") as mod:
            self.assertEqual(mod.x, 100)


if __name__ == "__main__":
    unittest.main()
