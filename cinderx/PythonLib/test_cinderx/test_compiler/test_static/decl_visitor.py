# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import ast
import re
from textwrap import dedent

from cinderx.compiler.static import StaticCodeGenerator
from cinderx.compiler.static.compiler import Compiler
from cinderx.compiler.static.module_table import ModuleTable, ModuleTableException
from cinderx.compiler.static.types import Class, TypeName

from .common import bad_ret_type, StaticTestBase


class DeclarationVisitorTests(StaticTestBase):
    def test_cross_module(self) -> None:
        acode = """
            class C:
                def f(self):
                    return 42
        """
        bcode = """
            from a import C

            def f():
                x = C()
                return x.f()
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "f")
        self.assertInBytecode(x, "INVOKE_FUNCTION", ((("a", "C"), "f"), 1))

    def test_cross_module_nested(self) -> None:
        for parent, close in [
            ("if FOO:", ""),
            ("for x in []:", ""),
            ("while True:", ""),
            ("with foo:", ""),
            ("try:", "except: pass"),
        ]:
            with self.subTest(parent=parent, close=close):
                acode = f"""
                    {parent}
                        class C:
                            def f(self):
                                return 42
                    {close}
                """
                bcode = """
                    from a import C

                    def f():
                        x = C()
                        return x.f()
                """
                bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
                x = self.find_code(bcomp, "f")
                self.assertNotInBytecode(x, "INVOKE_METHOD", (("a", "C", "f"), 0))

    def test_cross_module_nested_in_class(self) -> None:
        acode = """
            class A:
                class B:
                    def f(self):
                        return 42
        """
        bcode = """
            from a import A

            def f():
                x = A().B()
                return x.f()
        """
        comp = self.compiler(a=acode, b=bcode)
        f = self.find_code(comp.compile_module("b"))
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("a", "A", "B"), "f"), 1))
        with comp.in_module("b") as bmod:
            self.assertEqual(bmod.f(), 42)

    def test_cross_module_inst_decl_visit_only(self) -> None:
        acode = """
            class C:
                def f(self):
                    return 42

            x: C = C()
        """
        bcode = """
            from a import x

            def f():
                return x.f()
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "f")
        self.assertInBytecode(x, "INVOKE_METHOD", ((("a", "C"), "f"), 0))

    def test_cross_module_inst_decl_final_dynamic_is_invoked(self) -> None:
        acode = """
            from typing import Final, Protocol
            def foo(x: int) -> int:
                    return x + 42

            class CallableProtocol(Protocol):
                def __call__(self, x: int) -> int:
                    pass

            f: Final[CallableProtocol] = foo
        """
        bcode = """
            from a import f

            def g():
                return f(1)
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "g")
        self.assertInBytecode(x, "INVOKE_FUNCTION")

    def test_cross_module_inst_decl_alias_is_not_invoked(self) -> None:
        acode = """
            from typing import Final, Protocol
            def foo(x: int) -> int:
                    return x + 42
            f = foo
        """
        bcode = """
            from a import f

            def g():
                return f(1)
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "g")
        self.assertNotInBytecode(x, "INVOKE_FUNCTION")

    def test_cross_module_decl_visit_type_check_methods(self) -> None:
        acode = """
            class C:
                def f(self, x: int = 42) -> int:
                    return x
        """
        bcode = """
            from a import C

            def f():
                return C().f('abc')
        """
        self.compiler(a=acode, b=bcode).type_error(
            "b",
            re.escape(
                "type mismatch: str received for positional arg 'x', expected int"
            ),
            at="'abc'",
        )

        bcode = """
            from a import C

            def f() -> str:
                return C().f(42)
        """
        self.compiler(a=acode, b=bcode).type_error(
            "b", bad_ret_type("int", "str"), at="return"
        )

    def test_cross_module_decl_visit_type_check_fields(self) -> None:
        acode = """
            class C:
                def __init__(self):
                    self.x: int = 42
        """
        bcode = """
            from a import C

            def f():
                C().x = 'abc'
        """
        self.compiler(a=acode, b=bcode).type_error(
            "b",
            re.escape("type mismatch: str cannot be assigned to int"),
            at="C().x",
        )

        bcode = """
            from a import C

            def f() -> str:
                return C().x
        """
        self.compiler(a=acode, b=bcode).type_error(
            "b", bad_ret_type("int", "str"), at="return"
        )

    def test_cross_module_import_time_resolution(self) -> None:
        acode = """
            class C:
                def f(self):
                    return 42
        """
        bcode = """
            from a import C

            def f():
                x = C()
                return x.f()
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "f")
        self.assertInBytecode(x, "INVOKE_FUNCTION", ((("a", "C"), "f"), 1))

    def test_cross_module_type_checking(self) -> None:
        acode = """
            class C:
                def f(self):
                    return 42
        """
        bcode = """
            from typing import TYPE_CHECKING

            if TYPE_CHECKING:
                from a import C

            def f(x: C):
                return x.f()
        """
        bcomp = self.compiler(a=acode, b=bcode).compile_module("b")
        x = self.find_code(bcomp, "f")
        self.assertInBytecode(x, "INVOKE_METHOD", ((("a", "C"), "f"), 0))

    def test_cross_module_rewrite(self) -> None:
        acode = """
            from b import B
            class C(B):
                def f(self):
                    return self.g()
        """
        bcode = """
            class B:
                def g(self):
                    return 1 + 2
        """
        testcase = self

        class CustomCompiler(Compiler):
            def __init__(self):
                super().__init__(StaticCodeGenerator)
                self.btree: ast.Module | None = None

            def import_module(self, name: str, optimize: int) -> ModuleTable:
                if name == "b":
                    btree = ast.parse(dedent(bcode))
                    self.btree = self.add_module(
                        "b", "b.py", btree, bcode, optimize=optimize
                    )
                    testcase.assertFalse(self.btree is btree)

        compiler = CustomCompiler()
        acomp = compiler.compile(
            "a", "a.py", ast.parse(dedent(acode)), acode, optimize=1
        )
        btree = compiler.btree
        assert isinstance(btree, ast.Module)
        compiler.compile("b", "b.py", btree, bcode, optimize=1)
        x = self.find_code(self.find_code(acomp, "C"), "f")
        self.assertInBytecode(x, "INVOKE_METHOD", ((("b", "B"), "g"), 0))

    def test_declaring_toplevel_local_after_decl_visit_error(self) -> None:
        codestr = """
        class C:
            pass
        """

        class CustomCodeGenerator(StaticCodeGenerator):
            def visitClassDef(self, node):
                super().visitClassDef(node)
                self.cur_mod.declare_class(
                    node, Class(TypeName("mod", "C"), self.compiler.type_env)
                )

        class CustomCompiler(Compiler):
            def __init__(self):
                super().__init__(CustomCodeGenerator)

            def import_module(self, name: str, optimize: int) -> ModuleTable:
                pass

        compiler = CustomCompiler()
        with self.assertRaisesRegex(
            ModuleTableException,
            "Attempted to declare a class after the declaration visit",
        ):
            compiler.compile(
                "a", "a.py", ast.parse(dedent(codestr)), codestr, optimize=1
            )

    # --- Relative import tests ---

    def test_relative_import_preserves_static_type(self) -> None:
        """from .foo import C in pkg.mod should resolve C statically,
        producing INVOKE_FUNCTION bytecode instead of a dynamic call."""
        foo_code = """
            class C:
                def f(self) -> int:
                    return 42
        """
        mod_code = """
            from .foo import C

            def g():
                x = C()
                return x.f()
        """
        comp = self.compiler(**{"pkg.foo": foo_code, "pkg.mod": mod_code})
        f = self.find_code(comp.compile_module("pkg.mod"), "g")
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("pkg.foo", "C"), "f"), 1))

    def test_relative_import_multi_level_preserves_static_type(self) -> None:
        """from ..foo import C in pkg.sub.mod should resolve to pkg.foo.C."""
        foo_code = """
            class C:
                def f(self) -> int:
                    return 42
        """
        mod_code = """
            from ..foo import C

            def g():
                x = C()
                return x.f()
        """
        comp = self.compiler(**{"pkg.foo": foo_code, "pkg.sub.mod": mod_code})
        f = self.find_code(comp.compile_module("pkg.sub.mod"), "g")
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("pkg.foo", "C"), "f"), 1))

    def test_relative_import_from_dot_no_module(self) -> None:
        """from . import foo in pkg.mod should resolve foo as a module,
        and chained attribute access foo.f() should produce INVOKE_FUNCTION."""
        pkg_code = """
            pass
        """
        foo_code = """
            def f(x: int) -> int:
                return x
        """
        mod_code = """
            from . import foo

            def g():
                return foo.f(1)
        """
        comp = self.compiler(
            **{"pkg": pkg_code, "pkg.foo": foo_code, "pkg.mod": mod_code}
        )
        f = self.find_code(comp.compile_module("pkg.mod"), "g")
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("pkg.foo",), "f"), 1))

    def test_relative_import_in_function_scope(self) -> None:
        """Function-scope relative import should work via type_binder path."""
        foo_code = """
            class C:
                def f(self) -> int:
                    return 42
        """
        mod_code = """
            def g():
                from .foo import C
                x = C()
                return x.f()
        """
        comp = self.compiler(**{"pkg.foo": foo_code, "pkg.mod": mod_code})
        f = self.find_code(comp.compile_module("pkg.mod"), "g")
        self.assertInBytecode(f, "INVOKE_FUNCTION", ((("pkg.foo", "C"), "f"), 1))

    def test_relative_import_type_checking(self) -> None:
        """Relative import under TYPE_CHECKING should resolve for type annotations."""
        foo_code = """
            class C:
                def f(self) -> int:
                    return 42
        """
        mod_code = """
            from typing import TYPE_CHECKING

            if TYPE_CHECKING:
                from .foo import C

            def g(x: C):
                return x.f()
        """
        comp = self.compiler(**{"pkg.foo": foo_code, "pkg.mod": mod_code})
        f = self.find_code(comp.compile_module("pkg.mod"), "g")
        self.assertInBytecode(f, "INVOKE_METHOD", ((("pkg.foo", "C"), "f"), 0))

    def test_relative_import_executes_compiled_code(self) -> None:
        """Relative import should work at runtime when executing compiled module."""
        foo_code = """
            class C:
                def f(self) -> int:
                    return 42
        """
        mod_code = """
            from .foo import C

            def g() -> int:
                return C().f()
        """
        comp = self.compiler(**{"pkg.foo": foo_code, "pkg.mod": mod_code})
        with comp.in_module("pkg.mod") as mod:
            self.assertEqual(mod.g(), 42)
