# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from dataclasses import dataclass
from unittest import TestCase
from typing import cast

from cinderx.compiler.pyassem import Block, ExceptionTable

from .common import ExceptionTableEntry as Entry, ParsedExceptionTable


@dataclass
class ExceptionHandlerInfo:
    # Contains block fields used in 3.12 for make_exception_table().
    offset: int = -1
    preserve_lasti: bool = False
    startdepth: int = -1


class EncodingTests(TestCase):
    """Test the exception table packed encoding."""

    def test_encoding(self) -> None:
        e = ExceptionTable()
        handler1 = ExceptionHandlerInfo(100, False, 3)
        handler2 = ExceptionHandlerInfo(100, True, 3)
        e.emit_entry(20, 28, cast(Block, handler1))
        e.emit_entry(20, 28, cast(Block, handler2))
        table = e.getTable()
        expected = [
            # start/end/target get converted from instructions to bytes
            # when displayed by dis.
            # depth is handler.startdepth - 1 if lasti is not set
            Entry(start=40, end=56, target=200, depth=2, lasti=False),
            # depth is handler.startdepth - 2 if lasti is set
            Entry(start=40, end=56, target=200, depth=1, lasti=True),
        ]
        actual = ParsedExceptionTable.from_bytes(table).entries
        self.assertEqual(expected, actual)
