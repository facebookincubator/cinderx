# pyre-strict
import json
import os
from collections import defaultdict, deque
from importlib import resources

import click

from clang.cindex import CursorKind

from .clang_parser import FileParser, ParsedFile

CallGraph = dict[str, list[str]]


def make_callgraph(parsed_file: ParsedFile) -> CallGraph:
    graph = defaultdict(list)
    for cursor in parsed_file.translation_unit.cursor.walk_preorder():
        if not (
            # pyre-ignore[16]: `CursorKind` has no attribute `FUNCTION_DECL`.
            cursor.kind == CursorKind.FUNCTION_DECL
            and cursor.extent.start.file.name.endswith(parsed_file.source_file)
        ):
            continue
        func_name = cursor.spelling
        for node in cursor.walk_preorder():
            # pyre-ignore[16]: `CursorKind` has no attribute `CALL_EXPR`.
            if node.kind == CursorKind.CALL_EXPR and node.referenced:
                graph[func_name].append(node.referenced.spelling)
    return graph


def filter_graph(graph: CallGraph) -> CallGraph:
    """Filter the graph to calls within the same file."""
    out = {}
    for k, v in graph.items():
        out[k] = [x for x in v if x in graph]
    return out


def dep_list(graph: CallGraph, fn: str) -> list[str]:
    """Get the transitive closure of a function's dependencies."""
    # TODO: The algorithm won't get the ordering right if we have a recursive
    # cycle; fix it if it becomes an issue.
    queue = deque([fn])
    seen = set()
    out = []
    while queue:
        f = queue.popleft()
        if f not in seen:
            seen.add(f)
            out.append(f)
            if f in graph:
                queue.extend(graph[f])
    out.reverse()
    return out


def abspath(relpath: str) -> str:
    return os.path.abspath(os.path.join(os.getcwd(), relpath))


def resource_path(relpath: str) -> str:
    return str(resources.files(__package__).joinpath(relpath))


@click.command()
@click.argument("input_file", type=click.Path())
@click.argument("entry_point", type=click.Path())
@click.argument("output_file", type=click.Path())
@click.option("-p", "--py_version", default="3.12")
@click.option("-b", "--borrow/--no-borrow", default=False)
def main(
    input_file: str,
    entry_point: str,
    output_file: str,
    py_version: str,
    borrow: bool,
) -> None:
    print(f"Initial directory: {os.getcwd()}")

    compile_commands = resource_path("compilation-database")
    output_file = abspath(output_file)
    fp = FileParser(compile_commands)
    parsed_file = fp.parse(input_file)
    callgraph = make_callgraph(parsed_file)
    callgraph = filter_graph(callgraph)
    deps = dep_list(callgraph, entry_point)
    with open(output_file, "w") as f:
        json.dump(deps, f)
    # Spit out borrow commands to stdout
    if borrow:
        for dep in deps:
            print(f"// @Borrow function {dep} from {input_file} [{py_version}]")
