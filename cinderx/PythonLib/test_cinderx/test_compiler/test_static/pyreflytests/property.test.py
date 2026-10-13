# Copyright (c) Meta Platforms, Inc. and affiliates.

from __future__ import annotations

from types import CodeType
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from test_cinderx.test_compiler.test_static.test_pyrefly_binder import (
        PyreBinderTests,
    )


def verify(test: PyreBinderTests, code: CodeType) -> None:
    """Verify that the compiled code includes INVOKE_METHOD."""

    g = test.find_code(code, "g")
    test.assertInBytecode(g, "INVOKE_FUNCTION")
