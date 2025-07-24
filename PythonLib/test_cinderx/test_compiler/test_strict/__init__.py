# Copyright (c) Meta Platforms, Inc. and affiliates.
# flake8: noqa

# pyre-unsafe
import sys

from .test_flag_extractor import FlagExtractorTest

from .test_remove_annotations import AnnotationRemoverTests
from .test_rewriter import (
    ImmutableModuleTestCase,
    LazyLoadingTestCases,
    RewriterTestCase,
)
from .test_strict_codegen import (
    StrictBuiltinCompilationTests,
    StrictCheckedCompilationTests,
    StrictCompilationTests,
)

if not hasattr(sys, "gettotalrefcount"):
    # TODO(T129388950) These tests are only enabled in opt builds for now.
    # We need to fix the refleak in the strict compilation tests.
    # The "leak" here is just Static Python types (e.g ResolvedTypeRef),
    # we need to clean them up after compilation is done.
    from .test_strict_compile import StrictCompileTest
