# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
import json
import os
import re
import subprocess
from functools import partial
from typing import Callable

import click

from clang.cindex import (
    Config,
    CursorKind,
    Index,
    TranslationUnit,
    TranslationUnitLoadError,
)

T_COMPILATION_DB = list[dict[str, str | list[str]]]

BORROW_CPP_DIRECTIVES_PATTERN: re.Pattern[str] = re.compile(
    r".*// @Borrow CPP directives from (\S+)(?: \[(.*?)\])?"
)
BORROW_CPP_DIRECTIVES_NOINCLUDE_PATTERN: re.Pattern[str] = re.compile(
    r".*// @Borrow CPP directives noinclude from (\S+)(?: \[(.*?)\])?"
)
BORROW_DECL_PATTERN: re.Pattern[str] = re.compile(
    r"// @Borrow (function|typedef|var) (\S+) from (\S+)(?: \[(.*?)\])?"
)

HEADER = """
// Auto-generated by UpstreamBorrow.py.
// See the Buck target fbcode//cinderx/UpstreamBorrow:gen_borrowed.c.

"""


def filter_cpp_args(args: list[str]) -> list[str]:
    new_args = []
    append_next = False
    for arg in args:
        if append_next:
            new_args.append(arg)
            append_next = False
        elif arg in ["-I", "-isystem", "-idirafter", "-iquote", "-D", "-U", "-Xclang"]:
            new_args.append(arg)
            append_next = True
        elif (
            arg
            in [
                "-nostdinc",
                "-nostdinc++",
                "-no-canonical-prefixes",
                "-pthread",
                "-no-pthread",
                "-pthreads",
            ]
            or arg.startswith("-finput-charset=")
            or arg.startswith("-I")
            or arg.startswith("-U")
            or arg.startswith("-D")
            or arg.startswith("-std=")
        ):
            new_args.append(arg)
    return new_args


class ParsedFile:
    def __init__(
        self,
        source_file: str,
        compile_commands_db: T_COMPILATION_DB,
    ) -> None:
        args = []
        for command in compile_commands_db:
            file = command["file"]
            assert type(file) is str
            if file.endswith(source_file):
                source_file = file
                args = command["arguments"]
                assert type(args) is list
                args = filter_cpp_args(args)
                break
        else:
            raise Exception(f"Could not find compile command for {source_file}")

        self.source_file = source_file
        print("Loading: ", source_file)

        index = Index.create()
        try:
            self.translation_unit: TranslationUnit = index.parse(source_file, args=args)
        except TranslationUnitLoadError as e:
            # Unfortunately this is the main exception raised from index.parse()
            # and is completely useless. Chances are the error is in the args or
            # the file path.
            raise Exception(
                f"Failed to parse file: {source_file}, used args: {args}"
            ) from e

        with open(source_file, "r") as f:
            self.file_content: str = f.read()
            self.lines = self.file_content.split("\n")


def extract_preprocessor_directives(
    parsed_file: ParsedFile, includes: bool = True
) -> list[str]:
    print(f"Extracting preprocessor directives from: {parsed_file.source_file}")

    directives = []
    continuation = False
    for line in parsed_file.lines:
        line = line.rstrip()
        if continuation or line.lstrip().startswith("#"):
            if includes or not line.startswith("#include"):
                directives.append(line)
            continuation = line.endswith("\\")

    return directives


def extract_preprocessor_directives_noinclude(
    parsed_file: ParsedFile,
) -> list[str]:
    return extract_preprocessor_directives(parsed_file, includes=False)


def extract_declaration(
    name: str, kind: CursorKind, parsed_file: ParsedFile
) -> list[str]:
    print(f"Extracting {kind} for '{name}' from {parsed_file.source_file}")
    # Find the last declaration as earlier ones might be forward declarations.
    extent = None
    for cursor in parsed_file.translation_unit.cursor.walk_preorder():
        if cursor.kind == kind and cursor.spelling == name:
            extent = cursor.extent
    if extent:
        # We don't need column info; we always extract complete lines
        content = parsed_file.lines[extent.start.line - 1 : extent.end.line]
        return content
    raise Exception(f"Could not find {kind} for '{name}' in {parsed_file.source_file}")


def parse_version_set(input_string: str | None) -> set[str]:
    if input_string:
        return {v.strip() for v in input_string.split(",")}
    else:
        return set()


def parse_borrow_info(
    input_string: str,
) -> tuple[CursorKind, str, str, set[str]] | None:
    if match := BORROW_DECL_PATTERN.match(input_string):
        kind_str = match.group(1)
        function_name = match.group(2)
        source_file = match.group(3)
        version_set = parse_version_set(match.group(4))

        if kind_str == "function":
            # pyre-ignore[16]: `CursorKind` has no attribute `FUNCTION_DECL`.
            kind = CursorKind.FUNCTION_DECL
        elif kind_str == "typedef":
            # pyre-ignore[16]: `CursorKind` has no attribute `TYPEDEF_DECL`.
            kind = CursorKind.TYPEDEF_DECL
        elif kind_str == "var":
            # pyre-ignore[16]: `CursorKind` has no attribute `VAR_DECL`.
            kind = CursorKind.VAR_DECL
        else:
            raise Exception(f"Unknown kind: {kind_str}")

        return kind, function_name, source_file, version_set
    else:
        return None


def process_file(
    input_filename: str,
    output_filename: str,
    compile_commands_db: T_COMPILATION_DB,
    version: str,
) -> None:
    with open(input_filename, "r") as f:
        input_lines = f.readlines()

    parsed_files: dict[str, ParsedFile] = {}

    def extract(
        extractor_func: Callable[[ParsedFile], list[str]],
        source_file: str,
        version_set: set[str],
    ) -> list[str]:
        if len(version_set) != 0 and version not in version_set:
            return []
        if parsed_files.get(source_file) is None:
            parsed_files[source_file] = ParsedFile(source_file, compile_commands_db)
        return extractor_func(parsed_files[source_file])

    output_lines = []
    for line in input_lines:
        line = line.rstrip()

        if match := parse_borrow_info(line):
            kind, name, source_file, version_set = match
            if len(version_set) == 0 or version in version_set:
                output_lines.extend(
                    extract(
                        partial(extract_declaration, name, kind),
                        source_file,
                        version_set,
                    )
                )

        elif match := BORROW_CPP_DIRECTIVES_PATTERN.match(line):
            source_file = match.group(1)
            version_set = parse_version_set(match.group(2))
            output_lines.extend(
                extract(extract_preprocessor_directives, source_file, version_set)
            )

        elif match := BORROW_CPP_DIRECTIVES_NOINCLUDE_PATTERN.match(line):
            source_file = match.group(1)
            version_set = parse_version_set(match.group(2))
            output_lines.extend(
                extract(
                    extract_preprocessor_directives_noinclude, source_file, version_set
                )
            )

        else:
            output_lines.append(line)

    with open(output_filename, "w") as f:
        f.write(HEADER)
        f.write("\n".join(output_lines))


@click.command()
@click.argument("source_file", type=click.Path())
@click.argument("output_file", type=click.Path())
@click.argument("compile_commands", type=click.Path())
@click.argument("version", type=click.Choice(["3.10", "3.12"]))
def main(
    source_file: str,
    output_file: str,
    compile_commands: str,
    version: str,
) -> None:
    print(f"Initial directory: {os.getcwd()}")

    # These must be made absolute before changing the directory below.
    source_file = os.path.abspath(os.path.join(os.getcwd(), source_file))
    output_file = os.path.abspath(os.path.join(os.getcwd(), output_file))
    compile_commands = os.path.abspath(os.path.join(os.getcwd(), compile_commands))

    # This is a hack to make sure we are exactly in the root of an fbsource
    # checkout. This is needed because the compile commands database as
    # generated by buck has directories relatively to that location, and
    # setting the library path for LLVM. This is a bit dirty because it breaks
    # things like RE. Should work well enough for the cases we care about.
    try:
        fbsource_root = subprocess.run(
            ["hg", "root"], capture_output=True, encoding="utf-8", check=True
        ).stdout.strip()
    except subprocess.CalledProcessError as e:
        raise Exception(
            f"Failing stdout:\n{e.stdout}\nFailing stderr:\n{e.stderr}\n"
        ) from e
    os.chdir(fbsource_root)

    print(f"Processing {source_file} -> {output_file}")
    print(f"Compile commands from: {compile_commands}")
    print(f"Current directory: {os.getcwd()}")

    with open(compile_commands, "r") as f:
        compile_commands_db: T_COMPILATION_DB = json.load(f)

    llvm_driver_example = compile_commands_db[0]["arguments"][1]
    m = re.match(
        r"--cc=fbcode/third-party-buck/platform(\d+)/build/llvm-fb/(\d+)/",
        llvm_driver_example,
    )
    if not m:
        raise Exception(f"Could not find LLVM version in {llvm_driver_example}")
    platform_version = m.group(1)
    llvm_version = int(m.group(2))
    # LLVM prior to 17 (specifically 15) seems to have a bug which slightly
    # breaks parsing of some files.
    llvm_version = max(llvm_version, 17)
    llvm_lib_path = f"fbcode/third-party-buck/platform{platform_version}/build/llvm-fb/{llvm_version}/lib"
    print(f"Setting LLVM library path to: {llvm_lib_path}")
    Config.set_library_path(llvm_lib_path)
    process_file(source_file, output_file, compile_commands_db, version)


if __name__ == "__main__":
    main()
