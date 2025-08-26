# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict
import ast
import sys
from io import StringIO
from os import path
from tokenize import detect_encoding
from types import CodeType
from unittest import main, TestCase

from cinderx.compiler.dis_stable import Disassembler
from cinderx.compiler.pycodegen import compile_code

from .common import glob_test


class SbsCorpusCompileTests(TestCase):
    maxDiff: None = None


# Add a test case for each testcorpus/ file to SbsCorpusCompileTests.  Individual
# tests can be run with:
#  python -m test.test_compiler SbsCorpusCompileTests.test_00_const
def add_test(modname: str, fname: str) -> None:
    if "3_12/" in fname and sys.version_info <= (3, 12):
        return

    def test_corpus(self: SbsCorpusCompileTests) -> None:
        if "/3_12/" in fname and sys.version_info[:2] != (3, 12):
            return

        with open(fname, "rb") as inp:
            encoding, _lines = detect_encoding(inp.readline)
            code = b"".join(list(_lines) + inp.readlines()).decode(encoding)
            node = ast.parse(code, modname, "exec")

            orig = compile(node, modname, "exec")
            origdump = StringIO()
            Disassembler().dump_code(orig, origdump)

            codeobj = compile_code(node, modname, "exec")

            newdump = StringIO()
            Disassembler().dump_code(codeobj, newdump)

            self.assertEqual(
                origdump.getvalue().split("\n"), newdump.getvalue().split("\n")
            )

    # pyre-ignore[16]: Callable `test_corpus` has no attribute `__name__`.
    test_corpus.__name__ = "test_" + modname.replace("/", "_")[:-3]
    setattr(SbsCorpusCompileTests, test_corpus.__name__, test_corpus)


corpus_dir: str = path.join(path.dirname(__file__), "testcorpus")
glob_test(corpus_dir, "**/*.py", add_test)

if __name__ == "__main__":
    main(0)
