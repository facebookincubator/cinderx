# Copyright (c) Meta Platforms, Inc. and affiliates.
# flake8: noqa
import sys

from .test_api import ApiTests

# See the note in __init__.py: test_code_sbs is excluded from the test-cinder
# targets, so importing it has to be tolerated rather than assumed.
try:
    # pyre-ignore[21]: not included in every buck test build
    # pyrefly: ignore [missing-import]
    from .test_code_sbs import CodeTests
except ImportError:
    pass

from .test_errors import ErrorTests, ErrorTestsBuiltin
from .test_exception_table import EncodingTests
from .test_flags import FlagTests
from .test_graph import GraphTests
from .test_linepos import LinePositionTests
from .test_optimizer import AstOptimizerTests
from .test_symbols import SymbolVisitorTests
from .test_unparse import UnparseTests
from .test_visitor import VisitorTests

if "cinder" in sys.version:
    from .test_static import StaticCompilationTests, StaticRuntimeTests
