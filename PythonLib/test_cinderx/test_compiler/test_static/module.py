# Copyright (c) Meta Platforms, Inc. and affiliates.
from cinderx.compiler.static.compiler import Compiler
from cinderx.compiler.static.module_table import ModuleTable, ModuleTableException
from cinderx.compiler.static.types import TypeEnvironment, Value
from cinderx.compiler.strict.compiler import Compiler as StrictCompiler

from .common import get_child, StaticTestBase, StaticTestsStrictModuleLoader


class ModuleTests(StaticTestBase):
    def decl_visit(self, **modules: str) -> Compiler:
        compiler = self.compiler(**modules)
        for name in modules.keys():
            compiler.import_module(name, optimize=0)
        return compiler

    def test_import_name(self) -> None:
        acode = """
            def foo(x: int) -> int:
               return x
        """
        bcode = """
            import a
        """
        compiler = self.decl_visit(a=acode, b=bcode)

        self.assertIn("b", compiler.modules)
        self.assertIsNotNone(get_child(compiler.modules["b"], "a"))
        self.assertEqual(
            get_child(compiler.modules["b"], "a").klass, compiler.type_env.module
        )
        self.assertEqual(get_child(compiler.modules["b"], "a").module_name, "a")

    def test_import_name_as(self) -> None:
        acode = """
            def foo(x: int) -> int:
               return x
        """
        bcode = """
            import a as foo
        """
        compiler = self.decl_visit(a=acode, b=bcode)

        foo = get_child(compiler.modules["b"], "foo")
        self.assertIsNotNone(foo)
        self.assertEqual(foo.klass, compiler.type_env.module)
        self.assertEqual(foo.module_name, "a")

    def test_import_module_within_directory(self) -> None:
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            import a.b
        """
        compiler = self.decl_visit(**{"a.b": abcode, "c": ccode})

        a = get_child(compiler.modules["c"], "a")
        self.assertIsNotNone(a)
        self.assertEqual(a.klass, compiler.type_env.module)
        self.assertEqual(a.module_name, "a")

    def test_import_module_within_directory_as(self) -> None:
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            import a.b as m
        """
        compiler = self.decl_visit(**{"a.b": abcode, "c": ccode})

        m = get_child(compiler.modules["c"], "m")
        self.assertIsNotNone(m)
        self.assertEqual(m.klass, compiler.type_env.module)
        self.assertEqual(m.module_name, "a.b")

    def test_import_module_within_directory_from(self) -> None:
        acode = """
            pass
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        b = get_child(compiler.modules["c"], "b")
        self.assertEqual(b.klass, compiler.type_env.module)
        self.assertEqual(b.module_name, "a.b")

    def test_import_module_within_directory_from_as(self) -> None:
        acode = """
            pass
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b as zoidberg
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        zoidberg = get_child(compiler.modules["c"], "zoidberg")
        self.assertIsNotNone(zoidberg)
        self.assertEqual(zoidberg.klass, compiler.type_env.module)
        self.assertEqual(zoidberg.module_name, "a.b")

    def test_import_module_within_directory_from_where_value_exists(self) -> None:
        acode = """
            b: int = 1
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        b = get_child(compiler.modules["c"], "b")
        self.assertIsNotNone(b)
        self.assertEqual(b.klass, compiler.type_env.int)

    def test_import_module_within_directory_from_where_untyped_value_exists(
        self,
    ) -> None:
        acode = """
            b = 1
        """
        abcode = """
            def foo(x: int) -> int:
               return x
        """
        ccode = """
            from a import b
        """
        compiler = self.decl_visit(**{"a": acode, "a.b": abcode, "c": ccode})

        b = get_child(compiler.modules["c"], "b")
        # Matching the runtime, the b name in a will be the int, taking precedence over
        # the module.
        self.assertEqual(b.klass, compiler.type_env.dynamic)

    def test_import_chaining(self) -> None:
        acode = """
            def foo(x: int) -> int: return x
        """
        bcode = """
            import a
        """
        ccode = """
            import b

            def f():
               return b.a.foo(1)
        """
        compiler = self.compiler(a=acode, b=bcode, c=ccode)
        f = self.find_code(compiler.compile_module("c"), "f")
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("a",), "foo"), 1))

    def test_module_special_name_access(self) -> None:
        acode = """
            def foo(x: int) -> int: return x
        """
        bcode = """
            import a
        """
        ccode = """
            import b

            def f():
               reveal_type(b.a)
        """
        compiler = self.compiler(a=acode, b=bcode, c=ccode)
        compiler.revealed_type("c", r"Exact[types.ModuleType]")

    def test_repeated_import(self) -> None:
        codestr = """
            def foo():
                pass
        """
        compiler = self.get_strict_compiler()

        compiler.load_compiled_module_from_source(
            self.clean_code(codestr), "mod.py", "mod", 1
        )
        compiler.load_compiled_module_from_source(
            self.clean_code(codestr), "mod.py", "mod", 1
        )

    def test_recursive_imports(self) -> None:
        acode = """
            from typing import TYPE_CHECKING
            if TYPE_CHECKING:
                from b import X

            class C:
                pass
        """
        bcode = """
            from a import C
            from typing import Optional
            from __static__ import cast

            class X:
                def f(self, v) -> Optional[C]:
                    if isinstance(v, C):
                        return cast(C, v)
        """
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("b")

    def test_recursive_imports_subclassing(self) -> None:
        acode = """
            from typing import TYPE_CHECKING
            if TYPE_CHECKING:
                from b import X

            class C:
                pass
        """
        bcode = """
            from a import C
            from typing import Optional
            from __static__ import cast

            class X(C):
                def f(self, v) -> Optional[C]:
                    if isinstance(v, C):
                        return cast(C, v)
        """
        # we decl-visit `a` first, making this a cycle (if imports are eager)
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("b")

    def test_recursive_imports_subclassing_cross_dependency(self) -> None:
        acode = """
            from typing import TYPE_CHECKING, Optional
            if TYPE_CHECKING:
                from b import X

            class C:
                def __init__(self):
                    self.foo: Optional[X] = None

                @property
                def prop(self) -> int:
                    return 42
        """
        bcode = """
            from a import C

            class X(C):
                @property
                def prop(self) -> int:
                    return 42
        """
        # we decl-visit `a` first, making this a cycle (if imports are eager)
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("b")

    def test_class_member_circular_reference(self) -> None:
        """Resolving the type for an attribute on a class shouldn't trigger
        the immediate import of a dependency so that a cycle should succeed"""
        acode = """
            from b import C

            class X(C):
                pass
        """
        bcode = """
            if TYPE_CHECKING:
                from a import X

            class A:
                x: X
        """
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("a")

    def test_class_member_circular_reference_generic(self) -> None:
        """Resolving the type for an attribute on a class shouldn't trigger
        the immediate import of a dependency so that a cycle should succeed"""
        acode = """
            from b import C
            from typing import Generic, TypeVar

            T = TypeVar('T')
            class X(Generic[T], C):
                pass
        """
        bcode = """
            if TYPE_CHECKING:
                from a import X

            class A:
                x: X[int]
        """
        compiler = self.decl_visit(**{"a": acode, "b": bcode})
        compiler.compile_module("a")

    def test_actual_cyclic_reference(self) -> None:
        acode = """
            from b import B

            class A(B):
                pass
        """
        bcode = """
            from a import A

            class B(A):
                pass
        """
        with self.assertRaisesRegex(ModuleTableException, "due to cyclic reference"):
            compiler = self.decl_visit(**{"a": acode, "b": bcode})
