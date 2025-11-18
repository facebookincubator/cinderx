# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Tests for __static__ module.

These are tests for the plain-Python fallback implementations in __static__;
the static-Python uses are tested in test_compiler/test_static.py.

"""

from __static__ import (
    allow_weakrefs,
    box,
    byte,
    cast,
    cbool,
    char,
    CheckedDict,
    chkdict,
    clen,
    double,
    dynamic_return,
    int16,
    int32,
    int64,
    int8,
    pydict,
    PyDict,
    rand,
    RAND_MAX,
    single,
    size_t,
    ssize_t,
    uint16,
    uint32,
    uint64,
    uint8,
    unbox,
)

import unittest
from typing import Dict, Generic, Optional, TypeVar, Union

try:
    from cinderx import static
except ImportError:
    static = None


class StaticTests(unittest.TestCase):
    def test_chkdict(self) -> None:
        tgt = dict if static is None else static.chkdict
        self.assertIs(CheckedDict, tgt)
        self.assertIs(chkdict, tgt)

    def test_pydict(self) -> None:
        self.assertIs(pydict, dict)
        self.assertIs(PyDict, Dict)

    def test_clen(self) -> None:
        self.assertIs(clen, len)

    def test_int_types(self) -> None:
        for typ in [
            size_t,
            ssize_t,
            int8,
            uint8,
            int16,
            uint16,
            int32,
            uint32,
            int64,
            uint64,
            byte,
            char,
            cbool,
        ]:
            with self.subTest(typ=typ):
                x = typ(1)
                self.assertEqual(x, 1)

    def test_float_types(self) -> None:
        for typ in [
            single,
            double,
        ]:
            with self.subTest(typ=typ):
                x = typ(1.0)
                self.assertEqual(x, 1.0)

    def test_box(self) -> None:
        self.assertEqual(box(1), 1)

    def test_unbox(self) -> None:
        self.assertEqual(unbox(1), 1)

    def test_allow_weakrefs(self) -> None:
        class MyClass:
            pass

        self.assertIs(MyClass, allow_weakrefs(MyClass))

    def test_dynamic_return(self) -> None:
        def foo():
            pass

        self.assertIs(foo, dynamic_return(foo))

    def test_cast(self) -> None:
        self.assertIs(cast(int, 2), 2)

    def test_cast_subtype(self) -> None:
        class Base:
            pass

        class Sub(Base):
            pass

        s = Sub()
        self.assertIs(cast(Base, s), s)

    def test_cast_fail(self) -> None:
        with self.assertRaisesRegex(TypeError, "expected int, got str"):
            cast(int, "foo")

    def test_cast_optional(self) -> None:
        self.assertIs(cast(Optional[int], None), None)
        self.assertIs(cast(int | None, None), None)
        # pyre-ignore[20]: missing arg to __ror__
        self.assertIs(cast(None | int, None), None)

    def test_cast_generic_type(self) -> None:
        T = TypeVar("T")

        class G(Generic[T]):
            pass

        g = G()

        # pyre-ignore[16]: no attribute __getitem__
        self.assertIs(cast(G[int], g), g)

    def test_cast_type_too_complex(self) -> None:
        with self.assertRaisesRegex(ValueError, r"cast expects type or Optional\[T\]"):
            cast(Union[int, str], int)

    def test_rand(self) -> None:
        # pyre-ignore[16]: no attribute RAND_MAX
        self.assertEqual(type(RAND_MAX), int)
        # pyre-ignore[16]: no attribute RAND_MAX
        self.assertLessEqual(rand(), RAND_MAX)
        self.assertGreaterEqual(rand(), 0)


if __name__ == "__main__":
    unittest.main()
