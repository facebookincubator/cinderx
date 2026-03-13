# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import ast
import unittest

from cinderx.compiler.pycodegen import PythonCodeGenerator
from cinderx.compiler.static.types import TypedSyntaxError
from cinderx.compiler.static.visitor import GenericVisitor

from .common import StaticTestBase


def _make_import_from(code: str) -> ast.ImportFrom:
    """Parse a single 'from ... import ...' statement into an ImportFrom node."""
    mod = ast.parse(code)
    node = mod.body[0]
    assert isinstance(node, ast.ImportFrom)
    return node


class ResolveRelativeImportTests(unittest.TestCase):
    """Unit tests for GenericVisitor._resolve_relative_import name resolution.

    These test the name manipulation logic directly, without needing the full
    static compiler infrastructure. This lets us test __init__.py handling
    which the TestCompiler infra can't exercise (its _get_filename never
    produces __init__.py filenames).
    """

    def _resolve(self, module_name: str, filename: str, import_code: str) -> str:
        """Call _resolve_relative_import with the given module context."""
        node = _make_import_from(import_code)
        # Create a minimal mock that has the attributes _resolve_relative_import needs:
        # module_name, filename, and syntax_error.
        visitor = object.__new__(GenericVisitor)
        visitor.module_name = module_name
        visitor.filename = filename
        errors: list[str] = []
        # pyre-ignore[8]: Attribute has type `(self: GenericVisitor[TVisitRet], msg: str, node: AST) -> None`
        visitor.syntax_error = lambda msg, node: errors.append(msg)
        result = visitor._resolve_relative_import(node)
        if errors:
            raise ValueError(errors[0])
        return result

    # --- Non-__init__ modules (regular .py files) ---

    def test_single_dot_from_submodule(self) -> None:
        # from .foo import C in pkg.mod -> pkg.foo
        result = self._resolve("pkg.mod", "mod.py", "from .foo import C")
        self.assertEqual(result, "pkg.foo")

    def test_single_dot_no_module(self) -> None:
        # from . import foo in pkg.mod -> base module is "pkg",
        # then "foo" is resolved as a child of pkg by visitImportFrom.
        result = self._resolve("pkg.mod", "mod.py", "from . import foo")
        self.assertEqual(result, "pkg")

    def test_double_dot(self) -> None:
        # from ..foo import C in pkg.sub.mod -> pkg.foo
        result = self._resolve("pkg.sub.mod", "mod.py", "from ..foo import C")
        self.assertEqual(result, "pkg.foo")

    def test_double_dot_no_module(self) -> None:
        # from .. import foo in pkg.sub.mod -> base module is "pkg",
        # then "foo" is resolved as a child of pkg by visitImportFrom.
        result = self._resolve("pkg.sub.mod", "mod.py", "from .. import foo")
        self.assertEqual(result, "pkg")

    def test_beyond_top_level(self) -> None:
        # from ...foo import C in pkg.mod -> error (only 2 parts, 3 dots)
        with self.assertRaisesRegex(ValueError, "beyond top-level"):
            self._resolve("pkg.mod", "mod.py", "from ...foo import C")

    # --- __init__.py modules (package __init__ files) ---

    def test_init_single_dot(self) -> None:
        # from . import foo in pkg/__init__.py (module_name="pkg") -> base is "pkg",
        # then "foo" is resolved as a child of pkg by visitImportFrom.
        result = self._resolve("pkg", "__init__.py", "from . import foo")
        self.assertEqual(result, "pkg")

    def test_init_single_dot_with_module(self) -> None:
        # from .sub import C in pkg/__init__.py (module_name="pkg") -> pkg.sub
        result = self._resolve("pkg", "__init__.py", "from .sub import C")
        self.assertEqual(result, "pkg.sub")

    def test_init_double_dot(self) -> None:
        # from .. import foo in pkg.sub/__init__.py (module_name="pkg.sub") -> base is "pkg",
        # then "foo" is resolved as a child of pkg by visitImportFrom.
        result = self._resolve("pkg.sub", "__init__.py", "from .. import foo")
        self.assertEqual(result, "pkg")

    def test_init_double_dot_with_module(self) -> None:
        # from ..other import C in pkg.sub/__init__.py -> pkg.other
        result = self._resolve("pkg.sub", "__init__.py", "from ..other import C")
        self.assertEqual(result, "pkg.other")

    def test_init_beyond_top_level(self) -> None:
        # from .. import foo in top-level pkg/__init__.py -> error
        with self.assertRaisesRegex(ValueError, "beyond top-level"):
            self._resolve("pkg", "__init__.py", "from .. import foo")


class ImportTests(StaticTestBase):
    def test_unknown_import_with_fallback_is_not_allowed(self) -> None:
        codestr = """
        try:
            from unknown import foo
        except ImportError:
            def foo() -> int:
                return 0
        """
        self.type_error(codestr, r"Cannot redefine local variable foo")

    def test_function_definition_with_import_fallback_is_not_allowed(self) -> None:
        codestr = """
        try:
            def foo() -> int:
                return 0
        except ImportError:
            from unknown import foo
        """
        self.type_error(codestr, r"Cannot redefine local variable foo")

    def test_unknown_value_from_known_module_is_dynamic(self) -> None:
        acode = """
        x: int = 1
        """
        bcode = """
            from a import x, y

            reveal_type(y)
        """
        with self.assertRaisesRegex(TypedSyntaxError, "dynamic"):
            self.compiler(a=acode, b=bcode).compile_module("b")

    def test_unknown_value_from_nonstatic_module_is_dynamic(self) -> None:
        nonstatic_code = """
        pass
        """
        with self.in_module(
            nonstatic_code, code_gen=PythonCodeGenerator, name="nonstatic"
        ):
            codestr = """
            from nonstatic import x

            reveal_type(x)
            """
            self.type_error(codestr, r"reveal_type\(x\): 'dynamic'")

    def test_known_final_value_does_not_expose_final_across_modules(self) -> None:
        acode = """
        from typing import Final
        x: Final[bool] = True
        """
        bcode = """
            from a import x
            reveal_type(x)
        """
        with self.assertRaisesRegex(
            TypedSyntaxError, r"reveal_type\(x\): 'Exact\[bool\]'"
        ):
            self.compiler(a=acode, b=bcode).compile_module("b")
