# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
from __future__ import annotations

import ast
import unittest

from cinderx.compiler.strict.code_gen_base import is_mutable


class IsMutableTests(unittest.TestCase):
    def test_matches_only_mutable(self) -> None:
        self.assertTrue(is_mutable(ast.Name(id="mutable")))

    def test_rejects_substrings_of_mutable(self) -> None:
        # Regression: `node.id in ("mutable")` was a substring test (the value
        # is a str, not a tuple), so any bare-Name decorator whose id is a
        # substring of "mutable" (e.g. @table, @able, @mut, @e) wrongly matched
        # and was stripped from the class, disabling freezing.
        for name in ("table", "able", "mut", "mutabl", "utable", "e", ""):
            with self.subTest(name=name):
                self.assertFalse(is_mutable(ast.Name(id=name)))

    def test_rejects_unrelated_names(self) -> None:
        self.assertFalse(is_mutable(ast.Name(id="frozen")))

    def test_rejects_non_name_nodes(self) -> None:
        self.assertFalse(is_mutable(ast.Constant(value="mutable")))


if __name__ == "__main__":
    unittest.main()
