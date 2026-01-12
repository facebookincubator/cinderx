# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from ast import AST, copy_location, iter_fields
from typing import Sequence, TypeVar

# XXX should probably rename ASTVisitor to ASTWalker
# XXX can it be made even more generic?


TAst = TypeVar("TAst", bound=AST)


class ASTVisitor:
    """Performs a depth-first walk of the AST

    The ASTVisitor is responsible for walking over the tree in the
    correct order.  For each node, it checks the visitor argument for
    a method named 'visitNodeType' where NodeType is the name of the
    node's class, e.g. Class.  If the method exists, it is called
    with the node as its sole argument.

    This is basically the same as the built-in ast.NodeVisitor except
    for the following differences:
        It accepts extra parameters through the visit methods for flowing state
        It uses "visitNodeName" instead of "visit_NodeName"
        It accepts a list to the generic_visit function rather than just nodes
    """

    def __init__(self) -> None:
        self.node: AST | None = None
        self._cache: dict[object, object] = {}

    def generic_visit(self, node: TAst, *args: object) -> object:
        """Called if no explicit visitor function exists for a node."""
        for _field, value in iter_fields(node):
            if isinstance(value, list):
                for item in value:
                    if isinstance(item, AST):
                        self.visit(item, *args)
            elif isinstance(value, AST):
                self.visit(value, *args)

    def visit(self, node: TAst, *args: object) -> object:
        if not isinstance(node, AST):
            raise TypeError(f"Expected AST node, got {node!r}")

        self.node = node
        klass = node.__class__
        meth = self._cache.get(klass, None)
        if meth is None:
            className = klass.__name__
            meth = getattr(type(self), "visit" + className, type(self).generic_visit)
            self._cache[klass] = meth
        if not args:
            return meth(self, node)
        return meth(self, node, *args)

    def visit_list(self, nodes: Sequence[TAst], *args: object) -> None:
        if not isinstance(nodes, list):
            raise TypeError(f"Expected a list of AST nodes, got {nodes!r}")

        for node in nodes:
            self.visit(node, *args)


class ASTRewriter(ASTVisitor):
    """performs rewrites on the AST, rewriting parent nodes when child nodes
    are replaced."""

    @staticmethod
    def update_node(node: TAst, **replacement: object) -> TAst:
        res = node
        for name, val in replacement.items():
            existing = getattr(res, name)
            if existing is val:
                continue

            if node is res:
                res = ASTRewriter.clone_node(node)

            setattr(res, name, val)
        return res

    @staticmethod
    def clone_node(node: TAst) -> TAst:
        attrs = []
        for name in node._fields:
            attr = getattr(node, name, None)
            if isinstance(attr, list):
                attr = list(attr)
            attrs.append(attr)

        new = type(node)(*attrs)
        return copy_location(new, node)

    def skip_field(self, node: TAst, field: str) -> bool:
        return False

    def generic_visit(self, node: TAst, *args: object) -> TAst:
        ret_node = node
        for field, old_value in iter_fields(node):
            if self.skip_field(node, field):
                continue

            if isinstance(old_value, AST):
                new_value = self.visit(old_value, *args)
            elif isinstance(old_value, list):
                new_value = self.walk_list(old_value, *args)
            else:
                continue

            assert new_value is not None, (
                f"can't remove AST nodes that aren't part of a list {old_value!r}"
            )
            if new_value is not old_value:
                if ret_node is node:
                    ret_node = self.clone_node(node)

                setattr(ret_node, field, new_value)

        return ret_node

    def walk_list(self, values: Sequence[object], *args: object) -> Sequence[object]:
        """
        Like visit_list(), but it also walks values returned by ast.iter_fields()
        so it has to handle non-AST node values.
        """

        new_values = []
        changed = False

        for value in values:
            if not isinstance(value, AST):
                new_values.append(value)
                continue

            new_value = self.visit(value, *args)
            if new_value is not None:
                new_values.append(new_value)
                changed = True

        # Reuse the existing list when possible.
        return new_values if changed else values


class ExampleASTVisitor(ASTVisitor):
    """Prints examples of the nodes that aren't visited

    This visitor-driver is only useful for development, when it's
    helpful to develop a visitor incrementally, and get feedback on what
    you still have to do.
    """

    VERBOSE: int = 0

    examples: dict[object, object] = {}

    def visit(self, node: TAst, *args: object) -> object:
        self.node = node
        meth = self._cache.get(node.__class__, None)
        className = node.__class__.__name__
        if meth is None:
            meth = getattr(self, "visit" + className, 0)
            self._cache[node.__class__] = meth
        if self.VERBOSE > 1:
            print("visit", className, meth and meth.__name__ or "")
        if meth:
            meth(node, *args)
        elif self.VERBOSE > 0:
            klass = node.__class__
            if klass not in self.examples:
                self.examples[klass] = klass
                print()
                print(self)
                print(klass)
                for attr in dir(node):
                    if attr[0] != "_":
                        print("\t", "%-12.12s" % attr, getattr(node, attr))
                print()
            return self.default(node, *args)

    def default(self, node: TAst, *args: object) -> TAst:
        return node


# XXX this is an API change


def dumpNode(node: object) -> None:
    print(node.__class__)
    for attr in dir(node):
        if attr[0] != "_":
            print("\t", "%-10.10s" % attr, getattr(node, attr))
