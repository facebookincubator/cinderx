# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from clang.cindex import TranslationUnit
from python.clang_parser.clang_parser import (
    ClangParser,
    filter_cpp_args,
    setup_libclang_dll,
)


def arg_filter(args: list[str]) -> list[str]:
    new_args = filter_cpp_args(args)
    # The compilation DB we get from buck has -DNDEBUG in its cflags,
    # but we need to run the clang parser in debug mode, otherwise some
    # functions we want to borrow are ifdef-ed out.
    return [arg for arg in new_args if arg != "-DNDEBUG"]


class ParsedFile:
    source_file: str
    translation_unit: TranslationUnit
    lines: list[str]

    def __init__(self, source_file: str, translation_unit: TranslationUnit) -> None:
        self.source_file = source_file
        self.translation_unit = translation_unit
        with open(source_file, "r") as f:
            self.lines = f.read().split("\n")


class FileParser:
    def __init__(self, compile_commands: str) -> None:
        self.parser = ClangParser(compile_commands, arg_filter=arg_filter)
        self.parsed_files: dict[str, ParsedFile] = {}
        setup_libclang_dll()

    def chdir_to_root(self) -> None:
        self.parser.chdir_to_root()

    def parse(self, source_file: str) -> ParsedFile:
        if self.parsed_files.get(source_file) is None:
            tu = self.parser.parse(source_file)
            source_path = self.parser.path_to_file(source_file)
            self.parsed_files[source_file] = ParsedFile(source_path, tu)
        return self.parsed_files[source_file]
