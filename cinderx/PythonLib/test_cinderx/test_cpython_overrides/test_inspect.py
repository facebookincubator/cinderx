# Copyright (c) Meta Platforms, Inc. and affiliates.

import inspect
import sys
import types
import unittest


class CinderX_TestSignatureBind(unittest.TestCase):
    @staticmethod
    def call(func, *args, **kwargs):
        sig = inspect.signature(func)
        ba = sig.bind(*args, **kwargs)
        return func(*ba.args, **ba.kwargs)

    def test_signature_bind_implicit_arg(self) -> None:
        # Issue #19611: getcallargs should work with set comprehensions
        # CinderX: Modified for comprehension inlining
        def make_gen():
            return (z * z for z in range(5))

        gencomp_code = (
            # pyre-ignore[16]: no attribute __code__
            make_gen.__code__.co_consts[0]
            if sys.version_info >= (3, 14)
            else make_gen.__code__.co_consts[1]
        )
        gencomp_func = types.FunctionType(gencomp_code, {})

        iterator = iter(range(5))
        self.assertEqual(set(self.call(gencomp_func, iterator)), {0, 1, 4, 9, 16})
