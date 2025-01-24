# pyre-strict
import contextlib
import json
import os
import re
import subprocess
from typing import Generator

from clang.cindex import Config, Index, TranslationUnit, TranslationUnitLoadError

CompilationCommand = dict[str, str | list[str]]
CompilationDb = list[CompilationCommand]


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
        elif arg == "-DNDEBUG":
            # The compilation DB we get from buck has -DNDEBUG in its cflags,
            # but we need to run the clang parser in debug mode, otherwise some
            # functions we want to borrow are ifdef-ed out.
            pass
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
    source_file: str
    translation_unit: TranslationUnit
    lines: list[str]

    def __init__(self, command: CompilationCommand) -> None:
        source_file = command["file"]
        assert type(source_file) is str
        print("Loading: ", source_file)
        self.source_file = source_file
        args = command["arguments"]
        assert type(args) is list
        args = filter_cpp_args(args)
        self.translation_unit = self.parse_file(source_file, args)
        with open(source_file, "r") as f:
            self.lines = f.read().split("\n")

    @classmethod
    def from_db(
        cls, source_file: str, compile_commands_db: CompilationDb
    ) -> "ParsedFile":
        for command in compile_commands_db:
            file = command["file"]
            assert type(file) is str
            if file.endswith(source_file):
                return cls(command)
        raise Exception(f"Could not find compile command for {source_file}")

    def parse_file(self, source_file: str, args: list[str]) -> TranslationUnit:
        index = Index.create()
        try:
            return index.parse(source_file, args=args)
        except TranslationUnitLoadError as e:
            # Unfortunately this is the main exception raised from index.parse()
            # and is completely useless. Chances are the error is in the args or
            # the file path.
            raise Exception(
                f"Failed to parse file: {source_file}, used args: {args}"
            ) from e


@contextlib.contextmanager
def chdir(path: str) -> Generator[None, None, None]:
    d = os.getcwd()
    os.chdir(path)
    try:
        yield
    finally:
        os.chdir(d)


def make_compilation_db(compile_commands: str) -> CompilationDb:
    with open(compile_commands, "r") as f:
        compile_commands_db: CompilationDb = json.load(f)

    llvm_driver_example = compile_commands_db[0]["arguments"][1]
    m = re.match(
        r"--cc=fbcode/third-party-buck/[^/]+/build/llvm-fb/(\d+)/",
        llvm_driver_example,
    )
    if not m:
        raise Exception(f"Could not find LLVM version in {llvm_driver_example}")
    llvm_version = int(m.group(1))
    # LLVM prior to 17 (specifically 15) seems to have a bug which slightly
    # breaks parsing of some files.
    llvm_version = max(llvm_version, 17)
    Config.set_library_file(f"libclang.so.{llvm_version}")
    return compile_commands_db


class FileParser:
    def __init__(self, compile_commands: str) -> None:
        self.compilation_db: CompilationDb = make_compilation_db(compile_commands)
        self.parsed_files: dict[str, ParsedFile] = {}

    def parse(self, source_file: str) -> ParsedFile:
        if self.parsed_files.get(source_file) is None:
            self.parsed_files[source_file] = ParsedFile.from_db(
                source_file, self.compilation_db
            )
        return self.parsed_files[source_file]
