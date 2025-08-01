# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

"""Parser for future statements"""

import ast

from .visitor import ASTVisitor


class FutureParser(ASTVisitor):
    features = (
        "nested_scopes",
        "generators",
        "division",
        "absolute_import",
        "with_statement",
        "print_function",
        "unicode_literals",
        "generator_stop",
        "barry_as_FLUFL",
        "annotations",
    )

    def __init__(self) -> None:
        super().__init__()
        self.valid: set[ast.ImportFrom] = set()
        self.found_features: set[str] = set()
        self.possible_docstring = True

    def visitModule(self, node: ast.Module) -> None:
        for s in node.body:
            if (
                self.possible_docstring
                and isinstance(s, ast.Expr)
                and isinstance(s.value, ast.Constant)
                and isinstance(s.value.value, str)
            ):
                self.possible_docstring = False
                continue
            # no docstring after first statement
            self.possible_docstring = False
            if not self.check_stmt(s):
                break

    def check_stmt(self, stmt: ast.stmt) -> bool:
        if isinstance(stmt, ast.ImportFrom) and stmt.module == "__future__":
            for alias in stmt.names:
                name = alias.name
                if name in self.features:
                    self.found_features.add(name)
                elif name == "braces":
                    raise SyntaxError("not a chance")
                else:
                    raise SyntaxError("future feature %s is not defined" % name)
            self.valid.add(stmt)
            return True
        return False

    def get_features(self) -> set[str]:
        """Return set of features enabled by future statements"""
        return self.found_features

    def get_valid(self) -> set[ast.ImportFrom]:
        """Return set of valid future statements"""
        return self.valid


class BadFutureParser(ASTVisitor):
    """Check for invalid future statements"""

    def __init__(self, valid_parser: FutureParser) -> None:
        super().__init__()
        self.valid_parser = valid_parser

    def visitImportFrom(self, node: ast.ImportFrom) -> None:
        if node.module != "__future__":
            return
        if node in self.valid_parser.get_valid():
            return
        raise SyntaxError(
            "from __future__ imports must occur at the beginning of the file"
        )


def find_futures(node: ast.AST) -> set[str]:
    p1 = FutureParser()
    p2 = BadFutureParser(p1)
    p1.visit(node)
    p2.visit(node)
    return p1.get_features()
