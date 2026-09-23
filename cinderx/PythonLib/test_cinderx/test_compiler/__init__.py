# Copyright (c) Meta Platforms, Inc. and affiliates.
# flake8: noqa

import sys

from .test_api import ApiTests

# _TEST_CINDER_COMMON_EXCLUDE keeps these two out of the test-cinder targets --
# they have their own -- so they are not importable there.  Discovery imports
# this package, so an unconditional import takes the whole listing down wherever
# the harness lists tests by importing rather than statically.
try:
    # pyre-ignore[21]: not included in every buck test build
    from .test_code_sbs import CodeTests

    # pyre-ignore[21]: not included in every buck test build
    from .test_corpus import SbsCorpusCompileTests
except ImportError:
    pass

from .test_errors import ErrorTests, ErrorTestsBuiltin
from .test_flags import FlagTests
from .test_graph import GraphTests
from .test_linepos import LinePositionTests
from .test_optimizer import AstOptimizerTests
from .test_pysourceloader import PySourceLoaderTest
from .test_symbols import SymbolVisitorTests
from .test_unparse import UnparseTests
from .test_visitor import VisitorTests

if "cinder" in sys.version:
    from .test_static import *
    from .test_strict import *
