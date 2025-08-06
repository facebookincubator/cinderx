# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
from .common import StaticTestBase


class FStringTests(StaticTestBase):
    def test_format_spec(self) -> None:
        codestr = """
            def f(x: float) -> str:
                return f"{x:.2f}"
        """
        with self.in_module(codestr) as mod:
            f = mod.f
            self.assertEqual(f(2.134), "2.13")
