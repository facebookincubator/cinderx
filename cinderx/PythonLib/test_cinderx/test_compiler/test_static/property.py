# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
import asyncio
import sys

from cinderx.compiler.errors import TypedSyntaxError

from .common import bad_ret_type, StaticTestBase


class PropertyTests(StaticTestBase):
    def test_property_getter(self):
        codestr = """
            from typing import final
            class C:
                @final
                @property
                def foo(self) -> int:
                    return 42
                def bar(self) -> int:
                    return 0

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertEqual(f(C()), 42)
            self.assertInBytecode(f, "INVOKE_FUNCTION")

    def test_property_deleter(self):
        codestr = """
            class C:
                x: int = 42

                @property
                def foo(self) -> int:
                    if not hasattr(self, "x"):
                        self.x = 42
                    return self.x

                @foo.setter
                def foo(self, value: int) -> None:
                    self.x = value

                @foo.deleter
                def foo(self) -> None:
                    del self.x

            def bar(c: C) -> None:
                del c.foo

        """
        with self.in_module(codestr) as mod:
            C = mod.C
            c = C()
            c.foo = 50
            self.assertEqual(c.foo, 50)

            del c.foo
            self.assertEqual(c.foo, 42)

            bar = mod.bar
            self.assertInBytecode(bar, "DELETE_ATTR", "foo")

    def test_property_getter_known_exact(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar() -> int:
                return C().foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            self.assertEqual(f(), 42)
            self.assertInBytecode(f, "INVOKE_FUNCTION")

    def test_property_getter_final_class(self):
        codestr = """
            from typing import final
            @final
            class C:
                @property
                def foo(self) -> int:
                    return 42
                def bar(self) -> int:
                    return 0

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertEqual(f(C()), 42)
            self.assertInBytecode(f, "INVOKE_FUNCTION")

    def test_property_getter_non_final(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertNotInBytecode(f, "INVOKE_FUNCTION")
            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(f(C()), 42)

    def test_property_getter_async(self):
        codestr = """
            class C:
                @property
                async def foo(self) -> int:
                    return 42

            async def bar(c: C) -> int:
                return await c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertNotInBytecode(f, "INVOKE_FUNCTION")
            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(asyncio.run(f(C())), 42)

    def test_property_getter_inheritance(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            class D(C):
                @property
                def foo(self) -> int:
                    return 43

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            D = mod.D
            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(f(D()), 43)

    def test_property_getter_inheritance_no_override(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            class D(C):
                pass

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            D = mod.D
            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(f(D()), 42)

    def test_property_getter_non_static_inheritance(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            class D(C):
                @property
                def foo(self) -> int:
                    return 43

            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(f(D()), 43)

    def test_property_getter_non_static_inheritance_with_non_property(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            class D(C):
                def foo(self) -> int:
                    return 43

            self.assertInBytecode(f, "INVOKE_METHOD")
            x = D()
            with self.assertRaisesRegex(
                TypeError, "unexpected return type from D.foo, expected int, got method"
            ):
                f(x)

    def test_property_getter_non_static_inheritance_with_non_descr(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            class D(C):
                foo = 100

            self.assertInBytecode(f, "INVOKE_METHOD")
            x = D()
            self.assertEqual(f(x), 100)

    def test_property_getter_non_static_inheritance_with_non_descr_set(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

                @foo.setter
                def foo(self, value) -> None:
                    pass

            def bar(c: C):
                c.foo = 42
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            class D(C):
                foo = object()

            self.assertInBytecode(f, "INVOKE_METHOD")
            x = D()
            with self.assertRaisesRegex(TypeError, "'object' doesn't support __set__"):
                f(x)

    def test_property_getter_non_static_inheritance_with_non_property_setter(self):
        codestr = """
            class C:
                def __init__(self):
                    self.value = 0

                @property
                def foo(self) -> int:
                    return self.value

                @foo.setter
                def foo(self, value: int):
                    self.value = value

            def bar(c: C):
                c.foo = 42
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            called = False

            class Desc:
                def __get__(self, inst, ctx):
                    return 42

                def __set__(self, inst, value):
                    nonlocal called
                    called = True

            class D(C):
                foo = Desc()

            self.assertInBytecode(f, "INVOKE_METHOD")
            x = D()
            f(x)
            self.assertTrue(called)

    def test_property_getter_non_static_inheritance_with_get_descriptor(self):
        codestr = """
            class C:
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> int:
                return c.foo
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C

            class MyDesc:
                def __get__(self, inst, ctx):
                    return 43

            class D(C):
                foo = MyDesc()

            self.assertInBytecode(f, "INVOKE_METHOD")
            self.assertEqual(f(D()), 43)

    def test_property_getter_type_error(self):
        codestr = """
            from typing import final
            class C:
                @final
                @property
                def foo(self) -> int:
                    return 42

            def bar(c: C) -> str:
                return c.foo
        """
        self.type_error(codestr, bad_ret_type("int", "str"))

    def test_property_class_type_error(self):
        codestr = """
            @property
            class C:
                def foo(self) -> int:
                    return 42

        """
        self.type_error(codestr, "Cannot decorate a class with @property")

    def test_property_setter(self):
        codestr = """
            from typing import final
            class C:
                def __init__(self, x: int) -> None:
                    self.x = x

                @final
                @property
                def foo(self) -> int:
                    return -self.x

                @foo.setter
                def foo(self, x: int) -> None:
                    self.x = x

            def bar(c: C) -> None:
                c.foo = 3
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertInBytecode(f, "INVOKE_FUNCTION")
            c = C(2)
            self.assertEqual(f(c), None)
            self.assertEqual(c.foo, -3)

    def test_property_setter_inheritance(self):
        codestr = """
            from typing import final
            class C:
                def __init__(self, x: int) -> None:
                    self.x = x

                @property
                def foo(self) -> int:
                    return self.x

                @foo.setter
                def foo(self, x: int) -> None:
                    self.x = x

            class D(C):
                @property
                def foo(self) -> int:
                    return self.x

                @foo.setter
                def foo(self, x: int) -> None:
                    self.x = x + 1


            def bar(c: C) -> None:
                c.foo = 3
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            D = mod.D
            self.assertInBytecode(f, "INVOKE_METHOD")
            d = D(2)
            self.assertEqual(f(d), None)
            self.assertEqual(d.foo, 4)

    def test_property_setter_non_static_inheritance(self):
        codestr = """
            from typing import final
            class C:
                def __init__(self, x: int) -> None:
                    self.x = x

                @property
                def foo(self) -> int:
                    return self.x

                @foo.setter
                def foo(self, x: int) -> None:
                    self.x = x

            def bar(c: C) -> None:
                c.foo = 3
        """
        with self.in_module(codestr) as mod:
            f = mod.bar
            C = mod.C
            self.assertInBytecode(f, "INVOKE_METHOD")

            class D(C):
                @property
                def foo(self) -> int:
                    return self.x

                @foo.setter
                def foo(self, x: int) -> None:
                    self.x = x + 10

            d = D(2)
            self.assertEqual(f(d), None)
            self.assertEqual(d.x, 13)

    def test_property_no_setter(self):
        codestr = """
            class C:
                @property
                def prop(self) -> int:
                    return 1

                def set(self, val: int) -> None:
                    self.prop = val
        """
        with self.in_module(codestr) as mod:
            c = mod.C()
            if sys.version_info >= (3, 12):
                err = "property 'prop' of 'C' object has no setter"
            else:
                err = "can't set attribute"
            with self.assertRaisesRegex(AttributeError, err):
                c.prop = 2
            with self.assertRaisesRegex(AttributeError, "can't set attribute"):
                c.set(2)

    def test_property_call(self):
        codestr = """
            class C:
                a = property(lambda: "A")

            def f(x: C):
                return x.a
        """
        with self.in_module(codestr) as mod:
            self.assertInBytecode(mod.f, "LOAD_ATTR", "a")

    def test_property_override_attribute_call_as_parent_class(self):
        codestr = """
            class C:
                def __init__(self):
                    self.foo = 42

            class D(C):
                @property
                def foo(self) -> int:
                    return 43

            def bar(c: C) -> int:
                return c.foo
        """
        with self.assertRaisesRegex(
            TypedSyntaxError, "Cannot override attribute with property method"
        ):
            self.compile(codestr)

    def test_property_override_attribute_call_as_child_class(self):
        codestr = """
            class C:
                def __init__(self):
                    self.foo = 42

            class D(C):
                @property
                def foo(self) -> int:
                    return 43

            def bar(d: D) -> int:
                return d.foo
        """
        with self.assertRaisesRegex(
            TypedSyntaxError, "Cannot override attribute with property method"
        ):
            self.compile(codestr)

    def test_property_override_different_types(self):
        codestr = """
            class C:
                def __init__(self):
                    self.foo = 42

            class D(C):
                @property
                def foo(self) -> str:
                    return "43"

            def bar(d: D) -> str:
                return d.foo
        """
        with self.assertRaisesRegex(
            TypedSyntaxError, "Cannot override attribute with property method"
        ):
            self.compile(codestr)

    def test_property_getter_typechecks(self):
        codestr = """
            class C:
                x: int = 42

                @property
                def foo(self) -> str:
                    if not hasattr(self, "x"):
                        self.x = 42
                    return self.x
        """
        self.type_error(
            codestr,
            bad_ret_type("int", "str"),
            "return self.x",
        )

    def test_property_setter_typechecks(self):
        codestr = """
            class C:
                x: int = 42

                @property
                def foo(self) -> int:
                    if not hasattr(self, "x"):
                        self.x = 42
                    return self.x

                @foo.setter
                def foo(self, value: int) -> int:
                    self.x = value
        """
        self.type_error(
            codestr,
            r"Function has declared return type 'int' but can implicitly return None",
            "def foo(self, value: int) -> int:",
        )

    def test_property_deleter_typechecks(self):
        codestr = """
            class C:
                x: int = 42

                @property
                def foo(self) -> int:
                    if not hasattr(self, "x"):
                        self.x = 42
                    return self.x

                @foo.setter
                def foo(self, value: int) -> None:
                    self.x = value

                @foo.deleter
                def foo(self) -> str:
                    del self.x
        """
        self.type_error(
            codestr,
            r"Function has declared return type 'str' but can implicitly return None",
            "def foo(self) -> str",
        )

    def test_property_with_class_decorator(self):
        codestr = """
            from typing import TypeVar

            _T = TypeVar("_T")
            def f(klass: _T) -> _T:
                return klass

            @f
            class C:
                x: int = 42

                @property
                def foo(self) -> int:
                    if not hasattr(self, "x"):
                        self.x = 42
                    return self.x

                @foo.setter
                def foo(self, value: int) -> None:
                    self.x = value

                @foo.deleter
                def foo(self) -> None:
                    del self.x
        """

        with self.in_module(codestr) as mod:
            C = mod.C
            c = C()
            c.foo = 50
            self.assertEqual(c.foo, 50)

            del c.foo
            self.assertEqual(c.foo, 42)
