# Copyright (c) Meta Platforms, Inc. and affiliates.
# Python test set -- part 6, built-in types

import gc
import sys
import typing
import unittest.mock

from test.support import cpython_only


T = typing.TypeVar("T")


class CinderX_UnionTests(unittest.TestCase):
    @cpython_only
    def test_or_type_operator_reference_cycle(self):
        if not hasattr(sys, "gettotalrefcount"):
            self.skipTest("Cannot get total reference count.")
        gc.collect()
        before = sys.gettotalrefcount()
        # CinderX: This is changed from 30 to 1000
        for _ in range(1000):
            T = typing.TypeVar("T")
            U = int | list[T]
            T.blah = U
            del T
            del U
        gc.collect()
        # CinderX: This is changed from 30 to 100
        leeway = 100
        self.assertLessEqual(
            sys.gettotalrefcount() - before,
            leeway,
            msg="Check for union reference leak.",
        )


if __name__ == "__main__":
    unittest.main()
