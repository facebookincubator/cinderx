# Copyright (c) Meta Platforms, Inc. and affiliates.
import ast
import dis
import unittest
from dis import opmap, opname
from unittest import TestCase

from cinderx.compiler.pycodegen import CodeGenerator

from .common import CompilerTest


class Block:
    def __init__(self, label, next=None):
        self.label = label
        self.next = next


class GraphTests(CompilerTest):
    """Performs various unit tests on the flow control graph that gets produced
    to make sure that we're linking all of our basic blocks together properly."""

    def format_graph(self, graph):
        if graph.next:
            return f"Block({repr(graph.label)}, {self.format_graph(graph.next)})"
        return f"Block({repr(graph.label)})"

    def assert_graph_equal(self, graph, expected):
        first_block = graph.ordered_blocks[0]
        try:
            self.assert_graph_equal_worker(first_block, expected)
        except AssertionError as e:
            raise AssertionError(
                e.args[0] + "\nGraph was: " + self.format_graph(first_block)
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

    def test_if(self):
        graph = self.to_graph(
            """
        if foo:
            pass
        else:
            pass"""
        )
        expected = Block("entry", Block("", Block("if_else", Block("if_end"))))
        self.assert_graph_equal(graph, expected)

    def test_try_except(self):
        graph = self.to_graph(
            """
        try:
            pass
        except:
            pass"""
        )

        expected = Block(
            "entry",
            Block(
                "try_body",
                Block(
                    "try_handlers",
                    Block(
                        "try_cleanup_body0",
                    ),
                ),
            ),
        )
        self.assert_graph_equal(graph, expected)

    def test_chained_comparison(self):
        graph = self.to_graph("a < b < c")
        expected = Block(
            "entry", Block("compare_or_cleanup", Block("cleanup", Block("end")))
        )
        self.assert_graph_equal(graph, expected)

    def test_async_for(self):
        graph = self.to_graph(
            """
        async def f():
            async for x in foo:
                pass"""
        )
        # graph the graph for f so we can check the async for
        graph = self.get_child_graph(graph, "f")
        expected = Block(
            "entry",
            Block("async_for_try", Block("except", Block("end"))),
        )
        self.assert_graph_equal(graph, expected)


if __name__ == "__main__":
    unittest.main()
