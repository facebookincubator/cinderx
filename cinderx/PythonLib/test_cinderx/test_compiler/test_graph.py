# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
import sys
import unittest

from cinderx.compiler.pycodegen import CodeGenerator

from .common import CompilerTest

PRE_312: bool = sys.version_info < (3, 12)


class Block:
    __slots__ = (
        "label",
        "next",
    )

    def __init__(self, label, next=None):
        self.label = label
        self.next = next


def make_linear_graph(graph: list[str]) -> Block:
    head, *tail = graph
    if tail:
        return Block(head, make_linear_graph(tail))
    else:
        return Block(head)


def format_graph(graph: Block) -> str:
    if graph.next:
        return f"Block({repr(graph.label)}, {format_graph(graph.next)})"
    return f"Block({repr(graph.label)})"


class GraphTests(CompilerTest):
    """Performs various unit tests on the flow control graph that gets produced
    to make sure that we're linking all of our basic blocks together properly."""

    def assert_graph_equal(self, graph, expected):
        first_block = graph.ordered_blocks[0]
        try:
            self.assert_graph_equal_worker(first_block, expected)
        except AssertionError as e:
            raise AssertionError(
                e.args[0] + "\nGraph was: " + format_graph(first_block)
            ) from None

    def assert_graph_equal_worker(self, compiled, expected):
        self.assertEqual(compiled.label, expected.label)
        if expected.next:
            self.assertIsNotNone(compiled.next)
            self.assert_graph_equal_worker(compiled.next, expected.next)
        else:
            self.assertEqual(compiled.next, None)

    def get_child_graph(self, graph, name):
        for block in graph.ordered_blocks:
            for instr in block.getInstructions():
                if instr.opname == "LOAD_CONST":
                    if (
                        isinstance(instr.oparg, CodeGenerator)
                        and instr.oparg.name == name
                    ):
                        return instr.oparg.graph

    def test_if(self) -> None:
        graph = self.to_graph(
            """
        if foo:
            pass
        else:
            pass"""
        )

        expected = make_linear_graph(["entry", "", "if_else", "if_end"])
        self.assert_graph_equal(graph, expected)

    def test_try_except(self) -> None:
        graph = self.to_graph(
            """
        try:
            pass
        except:
            pass"""
        )

        if sys.version_info >= (3, 14):
            g = [
                "entry",
                "try_except",
                "try_cleanup_body_0",
                "try_except_0",
                "try_cleanup",
                "try_end",
            ]
        elif PRE_312:
            g = ["entry", "try_body", "try_handlers", "try_cleanup_body0"]
        else:
            g = ["entry", "try_body", "try_except", "try_cleanup_body_0", "try_cleanup"]
        expected = make_linear_graph(g)
        self.assert_graph_equal(graph, expected)

    def test_chained_comparison(self) -> None:
        graph = self.to_graph("a < b < c")
        expected = make_linear_graph(["entry", "compare_or_cleanup", "cleanup", "end"])
        self.assert_graph_equal(graph, expected)

    def test_async_for(self) -> None:
        graph = self.to_graph(
            """
        async def f():
            async for x in foo:
                pass"""
        )
        # graph the graph for f so we can check the async for
        graph = self.get_child_graph(graph, "f")
        if PRE_312:
            g = ["entry", "async_for_try", "except", "end"]
        else:
            g = [
                "entry",
                "start",
                "async_for_try",
                "send",
                "post send",
                "exit",
                "fail",
                "explicit_jump",
                "except",
                "end",
                "handler",
            ]
        expected = make_linear_graph(g)
        self.assert_graph_equal(graph, expected)


if __name__ == "__main__":
    unittest.main()
