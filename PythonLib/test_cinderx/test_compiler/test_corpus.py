import ast
import dis
from io import StringIO
from os import path
from tokenize import detect_encoding
from unittest import TestCase

from cinderx.compiler.dis_stable import Disassembler
from cinderx.compiler.pycodegen import compile as py_compile

from .common import glob_test


class SbsCorpusCompileTests(TestCase):
    maxDiff: None = None


# Add a test case for each testcorpus/ file to SbsCorpusCompileTests.  Individual
# tests can be run with:
#  python -m test.test_compiler SbsCorpusCompileTests.test_00_const
def add_test(modname: str, fname: str) -> None:
    def test_corpus(self: SbsCorpusCompileTests) -> None:
        with open(fname, "rb") as inp:
            encoding, _lines = detect_encoding(inp.readline)
            code = b"".join(list(_lines) + inp.readlines()).decode(encoding)
            node = ast.parse(code, modname, "exec")

            orig = compile(node, modname, "exec")
            origdump = StringIO()
            Disassembler().dump_code(orig, origdump)

            codeobj = py_compile(node, modname, "exec")
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
