# Copyright (c) Meta Platforms, Inc. and affiliates.

import ast
import unittest
from types import CodeType

from cinderx.compiler.static import StaticCodeGenerator
from cinderx.compiler.static.pyrefly_compiler import PyreflyCompiler
from cinderx.compiler.static.pyrefly_type_binder import PyreflyTypeInfo

from .common import StaticTestBase


class PyreBinderTests(StaticTestBase):
    def test_simple(self):
        code = self.compile_one(
            "x = 1 + 2",
            PyreflyTypeInfo(
                {
                    "type_table": [],
                    "locations": [],
                }
            ),
        )

    def compile_one(
        self,
        code: str,
        type_info: PyreflyTypeInfo,
        modname: str = "<module>",
        optimize: int = 0,
        ast_optimizer_enabled: bool = True,
        enable_patching: bool = False,
    ) -> CodeType:
        compiler = PyreflyCompiler(StaticCodeGenerator, type_info=type_info)
        tree = ast.parse(self.clean_code(code))
        return compiler.compile(
            modname,
            f"{modname}.py",
            tree,
            code,
            optimize,
            enable_patching=enable_patching,
        )


if __name__ == "__main__":
    unittest.main()
