# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
import __future__

import ast
import inspect
import re
import sys
import unittest

from cinderx.compiler import compile, compile_code
from cinderx.test_support import passIf

from .common import CompilerTest

PRE_312: bool = sys.version_info < (3, 12)
POST_312: bool = not PRE_312


class ApiTests(CompilerTest):
    def test_bad_mode(self) -> None:
        expected = re.escape("compile() mode must be 'exec', 'eval' or 'single'")
        with self.assertRaisesRegex(ValueError, expected):
            compile("42", "foo", "foo")

    def test_compile_with_only_ast_returns_ast(self) -> None:
        code = compile("42", "foo", "exec", 0x400)  # PyCF_ONLY_AST
        self.assertIsInstance(code, ast.AST)

    def test_compile_with_unrecognized_flag_raises_value_error(self) -> None:
        expected = re.escape("compile(): unrecognised flags")
        with self.assertRaisesRegex(ValueError, expected):
            compile("42", "foo", "exec", 0x80000000000)

    def test_compile_with_future_annotations_stringifies_annotation(self) -> None:
        code = compile_code(
            "a: List[int] = []",
            "foo",
            "exec",
            # pyre-fixme[16]: Module `__future__` has no attribute `CO_FUTURE_ANNOTATIONS`
            __future__.CO_FUTURE_ANNOTATIONS,
        )
        self.assertInBytecode(code, "LOAD_CONST", "List[int]")
        self.assertNotInBytecode(code, "LOAD_NAME", "List")
        self.assertNotInBytecode(code, "LOAD_NAME", "int")
        self.assertNotInBytecode(code, "BINARY_SUBSCR")

    def test_compile_without_future_annotations_does_type_subscript(self) -> None:
        code = compile_code("a: List[int] = []", "foo", "exec", 0)
        if sys.version_info >= (3, 14):
            load = "LOAD_GLOBAL"
            code = self.find_code(code, "__annotate__")
            subscr = "BINARY_OP"
        else:
            load = "LOAD_NAME"
            subscr = "BINARY_SUBSCR"
        self.assertNotInBytecode(code, "LOAD_CONST", "List[int]")
        self.assertInBytecode(code, load, "List")
        self.assertInBytecode(code, load, "int")
        self.assertInBytecode(code, subscr)

    def test_compile_unoptimized(self) -> None:
        src_code = "assert True"
        code = compile_code(src_code, "foo", "single", optimize=0)
        self.assertNotInBytecode(code, "LOAD_GLOBAL", "AssertionError")
        self.assertNotInBytecode(code, "RAISE_VARARGS")

    def test_compile_optimized(self) -> None:
        src_code = "assert True"
        code = compile_code(src_code, "foo", "single", optimize=1)
        self.assertNotInBytecode(code, "LOAD_GLOBAL", "AssertionError")
        self.assertNotInBytecode(code, "RAISE_VARARGS")

    def test_compile_optimized_docstrings(self) -> None:
        """
        Ensure we strip docstrings with optimize=2, and retain them for
        optimize=1
        """

        src_code = "def f(): '''hi'''\n"
        code = compile_code(src_code, "foo", "single", optimize=1)
        consts = dict(zip(code.co_names, code.co_consts))
        self.assertIn("hi", consts["f"].co_consts)

        code = compile_code(src_code, "foo", "single", optimize=2)
        consts = dict(zip(code.co_names, code.co_consts))
        self.assertNotIn("hi", consts["f"].co_consts)


@passIf(POST_312, "Python 3.10- only")
class ApiTests310(CompilerTest):
    def test_compile_single(self) -> None:
        code = compile_code("300", "foo", "single")
        self.assertInBytecode(code, "LOAD_CONST", 300)
        self.assertInBytecode(code, "PRINT_EXPR")

    def test_compile_eval(self) -> None:
        code = compile_code("42", "foo", "eval")
        self.assertInBytecode(code, "LOAD_CONST", 42)
        self.assertInBytecode(code, "RETURN_VALUE")

    def test_compile_with_barry_as_bdfl_emits_ne(self) -> None:
        # pyre-fixme[16]: Module `__future__` has no attribute `CO_FUTURE_BARRY_AS_BDFL`
        code = compile_code("a <> b", "foo", "exec", __future__.CO_FUTURE_BARRY_AS_BDFL)
        self.assertInBytecode(code, "COMPARE_OP", "!=")

    def test_compile_with_annotation_in_except_handler_emits_store_annotation(
        self,
    ) -> None:
        source = inspect.cleandoc(
            """
try:
    pass
except:
    x: int = 1
"""
        )

        code = compile_code(
            source,
            "foo",
            "exec",
            0,
        )
        self.assertInBytecode(code, "SETUP_ANNOTATIONS")


@passIf(PRE_312, "Python 3.12+ only")
class ApiTests312(CompilerTest):
    def test_compile_single(self) -> None:
        code = compile_code("256", "foo", "single")
        self.assertInBytecode(code, "LOAD_CONST", 256)
        self.assertInBytecode(code, "CALL_INTRINSIC_1", 1)  # INTRINSIC_PRINT


if __name__ == "__main__":
    unittest.main()
