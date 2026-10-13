# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

from types import CodeType
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from test_cinderx.test_compiler.test_static.test_pyrefly_binder import (
        PyreBinderTests,
    )


def verify(test: PyreBinderTests, code: CodeType) -> None:
    """Verify that the compiled code includes PRIMITIVE_BINARY_OP."""

    g = test.find_code(code, "f")
    test.assertInBytecode(g, "PRIMITIVE_BINARY_OP")
