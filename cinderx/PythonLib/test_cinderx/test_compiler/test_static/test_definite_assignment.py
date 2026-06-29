# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

import ast
import unittest

from cinderx.compiler.static.definite_assignment_checker import (
    DefiniteAssignmentVisitor,
)
from cinderx.compiler.symbols import SymbolVisitor


class DefiniteAssignmentTests(unittest.TestCase):
    def _unassigned(self, src: str) -> list[str]:
        tree = ast.parse(src)
        symbols = SymbolVisitor(0)
        symbols.visit(tree)
        func = tree.body[0]
        assert isinstance(func, ast.FunctionDef)
        visitor = DefiniteAssignmentVisitor(symbols.scopes[func])
        visitor.analyzeFunction(func)
        return sorted(name.id for name in visitor.unassigned)

    def test_augassign_rhs_name_read_before_assignment(self) -> None:
        # `x += y` reads both the target `x` and the right-hand side `y`
        # before either is assigned, so both must be reported as unassigned.
        # Previously the bare-`Name` RHS (`y`) was dropped because the handler
        # used generic_visit(node.value) (which visits the Name's children,
        # never dispatching visit_Name) instead of visit(node.value).
        self.assertEqual(
            self._unassigned("def f():\n    x += y\n    y = 1\n"),
            ["x", "y"],
        )

    def test_augassign_rhs_binop_read_before_assignment(self) -> None:
        # A non-bare RHS (BinOp) already worked because generic_visit recurses
        # into its child Name nodes; guard against regressing it.
        self.assertEqual(
            self._unassigned("def f():\n    x += y + 1\n    y = 1\n"),
            ["x", "y"],
        )

    def test_plain_assign_rhs_name_read_before_assignment(self) -> None:
        # Control: plain assignment already handled the bare-Name RHS correctly.
        self.assertEqual(
            self._unassigned("def f():\n    x = y\n    y = 1\n"),
            ["y"],
        )


if __name__ == "__main__":
    unittest.main()
