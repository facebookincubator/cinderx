# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from dataclasses import dataclass
from typing import cast
from unittest import TestCase

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

    @staticmethod
    def _decode_item(buf: bytes) -> int:
        # Decode a single varint emitted by emit_item(value, msb=0): 6 data bits
        # per byte, continuation bit (64) set on every byte but the last.
        it = iter(buf)
        b = next(it)
        value = b & 0x3F
        while b & 64:
            b = next(it)
            value = (value << 6) | (b & 0x3F)
        return value

    def test_emit_item_roundtrip_across_group_boundaries(self) -> None:
        # emit_item packs a value 6 bits at a time. Each group threshold uses
        # `value >= (1 << i)`; the top group (bit 24) must use the same `>=`.
        # A `>` there drops bit 24 at exactly value == 1 << 24, corrupting the
        # entry (it decodes back to 0).
        values = []
        for i in (6, 12, 18, 24):
            values += [(1 << i) - 1, 1 << i, (1 << i) + 1]
        values += [0, 1, (1 << 30) - 1]
        for value in values:
            with self.subTest(value=value):
                e = ExceptionTable()
                e.emit_item(value, 0)
                self.assertEqual(self._decode_item(e.getTable()), value)

    def test_emit_item_bit24_boundary(self) -> None:
        e = ExceptionTable()
        e.emit_item(1 << 24, 0)
        self.assertEqual(self._decode_item(e.getTable()), 1 << 24)
