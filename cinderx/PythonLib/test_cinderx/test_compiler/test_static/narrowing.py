# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

from .common import StaticTestBase


class TestNarrowing(StaticTestBase):
    def test_assign(self):
        codestr = """
        def f(x: int | None) -> int:
          if (y := x) is not None:
            return y
          raise ValueError("foo")
        """
        with self.in_module(codestr):
            pass
