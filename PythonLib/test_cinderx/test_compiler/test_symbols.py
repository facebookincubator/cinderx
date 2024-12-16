# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
import ast
import sys
import unittest

from ast import FunctionDef
from sys import version_info
from textwrap import dedent

from cinderx.compiler import walk
from cinderx.compiler.consts import SC_FREE, SC_GLOBAL_IMPLICIT
from cinderx.compiler.symbols import ClassScope, FunctionScope, SymbolVisitor, TypeAliasScope, TypeParamScope, TypeVarBoundScope
from unittest import skipIf

from .common import CompilerTest

PRE_312: bool = sys.version_info < (3, 12)

class SymbolVisitorTests(CompilerTest):
    def test_simple_assignments(self) -> None:
        stmts = [
            "foo = 42",
            "foo += 42",
            "class foo: pass",
            "def foo(): pass",
            "async def foo(): pass",
            "del foo",
            "import foo",
            "from foo import foo",
            "import bar as foo",
            "from bar import x as foo",
            "try:\n    pass\nexcept Exception as foo:\n    pass",
        ]
        for stmt in stmts:
            module = ast.parse(stmt)
            visitor = SymbolVisitor(0)
            walk(module, visitor)
            self.assertIn("foo", visitor.scopes[module].defs)

    def test_comp_assignments(self) -> None:
        stmts = [
            "(42 for foo in 'abc')",
            "[42 for foo in 'abc']",
            "{42 for foo in 'abc'}",
            "{42:42 for foo in 'abc'}",
        ]
        for stmt in stmts:
            module = ast.parse(stmt)
            visitor = SymbolVisitor(0)
            walk(module, visitor)
            # pyre-ignore[16]: `_ast.stmt` has no attribute `value`.
            gen = module.body[0].value
            self.assertIn("foo", visitor.scopes[gen].defs)

    def test_class_kwarg_in_nested_scope(self) -> None:
        code = """def f():
            def g():
                class C(x=foo):
                    pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(node, FunctionDef) and node.name == "f":
                self.assertEqual(scope.check_name("foo"), SC_GLOBAL_IMPLICIT)
                break
        else:
            self.fail("scope not found")

    def test_class_annotation_in_nested_scope(self) -> None:
        code = """def f():
            def g():
                @foo
                class C:
                    pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(node, FunctionDef) and node.name == "f":
                self.assertEqual(scope.check_name("foo"), SC_GLOBAL_IMPLICIT)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_type_param(self) -> None:
        code = """def f[T](): pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".defaults" in scope.defs)
                self.assertTrue(".defaults" in scope.params)

                func = scope.children[0]
                self.assertEqual(len(scope.children), 1)
                self.assertEqual(func.name, "f")
                self.assertTrue(isinstance(func, FunctionScope))
                self.assertTrue(func.nested)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_type_param_collision(self) -> None:
        code = """def f[T](T): return T"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".defaults" in scope.defs)
                self.assertTrue(".defaults" in scope.params)

                self.assertEqual(len(scope.children), 1)
                func = scope.children[0]
                self.assertEqual(func.name, "f")
                self.assertTrue(isinstance(func, FunctionScope))
                self.assertTrue(func.nested)
                self.assertIn("T", func.defs)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_dup_type_var(self) -> None:
        code = """def f[T, T](): pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        with self.assertRaisesRegex(SyntaxError, "duplicated type parameter: 'T'"):
            walk(module, visitor)

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_type_param_bound(self) -> None:
        code = """def f[T: str](): pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertEqual(len(scope.children), 2)

                bound = scope.children[0]
                self.assertEqual(bound.name, "T")
                self.assertTrue(isinstance(bound, TypeVarBoundScope))
                self.assertTrue(bound.nested)
                self.assertTrue("str" in bound.uses)
                self.assertEqual(scope.check_name("str"), SC_GLOBAL_IMPLICIT)

                func = scope.children[1]
                self.assertEqual(func.name, "f")
                self.assertTrue(isinstance(func, FunctionScope))
                self.assertTrue(func.nested)
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".defaults" in scope.defs)
                self.assertTrue(".defaults" in scope.params)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_type_var_tuple(self) -> None:
        code = """def f[*T](): pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertEqual(len(scope.children), 1)

                func = scope.children[0]
                self.assertEqual(func.name, "f")
                self.assertTrue(isinstance(func, FunctionScope))
                self.assertTrue(func.nested)
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".defaults" in scope.defs)
                self.assertTrue(".defaults" in scope.params)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_func_param_spec(self) -> None:
        code = """def f[**T](): pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertEqual(len(scope.children), 1)

                func = scope.children[0]
                self.assertEqual(func.name, "f")
                self.assertTrue(isinstance(func, FunctionScope))
                self.assertTrue(func.nested)
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".defaults" in scope.defs)
                self.assertTrue(".defaults" in scope.params)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_class_type_param(self) -> None:
        code = """class C[T]: pass"""
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "C":
                self.assertEqual(len(scope.children), 1)
                self.assertEqual(scope.children[0].name, "C")
                self.assertTrue(isinstance(scope.children[0], ClassScope))
                self.assertTrue(scope.children[0].nested)
                self.assertTrue("T" in scope.defs)
                self.assertTrue("T" in scope.type_params)
                self.assertTrue(".generic_base" in scope.defs)
                self.assertTrue(".type_params" in scope.defs)
                self.assertTrue(".type_params" in scope.cells)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_type_param_mangled(self) -> None:
        code = dedent("""
        class C:
            def f[__T](): pass
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertIn("_C__T", scope.defs)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_use_type_param(self) -> None:
        code = dedent("""
        class C[T]:
            def f(): return T
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "C":
                self.assertEqual(len(scope.children), 1)

                klass = scope.children[0]
                self.assertEqual(klass.name, "C")

                self.assertEqual(len(klass.children), 1)

                func = klass.children[0]
                self.assertTrue(func.nested)
                self.assertEqual(func.name, "f")
                self.assertEqual(func.check_name("T"), SC_FREE)

                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_refer_class_scope(self) -> None:
        code = dedent("""
        class C:
            class Nested: pass
            def f[T](self, x: Nested): pass
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeParamScope) and scope.name == "f":
                self.assertEqual(len(scope.parent.children), 2)
                self.assertEqual(len(scope.children), 1)
                self.assertIn("__classdict__", scope.uses)
                self.assertIn("__classdict__", scope.parent.defs)
                self.assertIn("Nested", scope.uses)

                func = scope.children[0]
                self.assertTrue(func.nested)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_type_alias_global(self) -> None:
        code = dedent("""
        type T = int
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeAliasScope) and scope.name == "T":
                print(scope.check_name("int"))
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_type_alias_class(self) -> None:
        code = dedent("""
        class C:
            type T = int
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeAliasScope) and scope.name == "T":
                self.assertEqual(len(scope.parent.children), 1)
                self.assertEqual(len(scope.children), 0)
                self.assertIn("__classdict__", scope.uses)
                self.assertIn("__classdict__", scope.parent.defs)
                break
        else:
            self.fail("scope not found")

    @skipIf(PRE_312, "Python 3.12+ only")
    def test_type_alias_generic_class(self) -> None:
        code = dedent("""
        class C[X]:
            type T[X] = int
        """)
        module = ast.parse(code)
        visitor = SymbolVisitor(0)
        walk(module, visitor)
        for node, scope in visitor.scopes.items():
            if isinstance(scope, TypeAliasScope) and scope.name == "T":
                self.assertEqual(len(scope.parent.children), 1)
                self.assertEqual(len(scope.children), 0)
                self.assertIn("__classdict__", scope.uses)
                self.assertIn("__classdict__", scope.parent.parent.defs)
                break
        else:
            self.fail("scope not found")

if __name__ == "__main__":
    unittest.main()
