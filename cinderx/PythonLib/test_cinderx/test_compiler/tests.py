# Copyright (c) Meta Platforms, Inc. and affiliates.
# flake8: noqa
import sys

from .test_api import ApiTests

# pyre-ignore[21]: test_code_sbs not found (not included in buck test build)
from .test_code_sbs import CodeTests
from .test_errors import ErrorTests, ErrorTestsBuiltin

# pyre-ignore[21]: test_exceptiontable not found (not included in buck test build)
from .test_exceptiontable import EncodingTests
from .test_flags import FlagTests
from .test_graph import GraphTests
from .test_linepos import LinePositionTests
from .test_optimizer import AstOptimizerTests

# pyre-ignore[21]: test_peephole not found (not included in buck test build)
from .test_peephole import PeepHoleTests
from .test_symbols import SymbolVisitorTests
from .test_unparse import UnparseTests
from .test_visitor import VisitorTests

if "cinder" in sys.version:
    from .test_static import StaticCompilationTests, StaticRuntimeTests
