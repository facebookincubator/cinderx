# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
import json
import os
from collections import defaultdict, deque
from importlib import resources

import click

from clang.cindex import CursorKind

from .clang_parser import FileParser, ParsedFile

Graph = dict[str, list[str]]


def make_callgraph(parsed_file: ParsedFile) -> Graph:
    graph = {}
    for cursor in parsed_file.translation_unit.cursor.walk_preorder():
        if not (
            # pyre-ignore[16]: `CursorKind` has no attribute `FUNCTION_DECL`.
            cursor.kind == CursorKind.FUNCTION_DECL
            and cursor.extent.start.file.name.endswith(parsed_file.source_file)
        ):
            continue
        func_name = cursor.spelling
        graph[func_name] = []
        for node in cursor.walk_preorder():
            # pyre-ignore[16]: `CursorKind` has no attribute `CALL_EXPR`.
            if node.kind == CursorKind.CALL_EXPR and node.referenced:
                graph[func_name].append(node.referenced.spelling)
    return graph


def filter_graph(graph: Graph) -> Graph:
    """Filter the graph to calls within the same file."""
    out = {}
    for k, v in graph.items():
        out[k] = [x for x in v if x in graph]
    return out


def get_ordered_deps(graph: Graph, nodes: list[str]) -> list[str]:
    """Find and topologically sort a combined dep list for nodes."""
    visited: dict[str, bool] = {node: False for node in graph}
    out: list[str] = []

    def dfs(node: str) -> None:
        if visited[node]:
            return
        visited[node] = True
        for neighbor in graph[node]:
            dfs(neighbor)
        out.append(node)

    for node in nodes:
        dfs(node)
    return out


def abspath(relpath: str) -> str:
    return os.path.abspath(os.path.join(os.getcwd(), relpath))


def resource_path(relpath: str) -> str:
    return str(resources.files(__package__).joinpath(relpath))


class FilteredCallGraph:
    file: ParsedFile
    graph: Graph

    def __init__(self, file: ParsedFile) -> None:
        self.file = file
        self.graph = filter_graph(make_callgraph(file))

    def deps(self, fns: list[str]) -> list[str]:
        """Take a list of functions and calculate all their dependencies."""
        return get_ordered_deps(self.graph, fns)


def print_borrows(
    fp: FileParser, input_file: str, fns: list[str], py_version: str
) -> None:
    parsed_file = fp.parse(input_file)
    callgraph = FilteredCallGraph(parsed_file)
    deps = callgraph.deps(fns)
    # Spit out borrow commands to stdout
    for dep in deps:
        print(f"// @Borrow function {dep} from {input_file} [{py_version}]")


@click.command()
@click.option("-p", "--py_version", default="3.12")
@click.argument("input_file", type=click.Path())
@click.argument("entry_point", required=False)
def main(input_file: str, entry_point: str | None, py_version: str) -> None:
    compile_commands = resource_path("compilation-database")
    fp = FileParser(compile_commands)

    # If we have an entry point, the input is a filepath relative to the
    # cpython source directory.
    if entry_point:
        print_borrows(fp, input_file, [entry_point], py_version)
        return

    # If not, the input is a json file containing a dict of files to entry
    # points from which we generate a combined borrow list. See
    # `find_missing.py` for the script that generates the json file.
    with open(input_file, "r") as f:
        files = json.load(f)
    for file, fns in files.items():
        print_borrows(fp, file, fns, py_version)
        print()
