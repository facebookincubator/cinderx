# Copyright (c) Meta Platforms, Inc. and affiliates.

import __static__


def callee(x: int) -> int:
    return x + 1


raise RuntimeError("boom")
