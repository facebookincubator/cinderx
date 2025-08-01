# Copyright (c) Meta Platforms, Inc. and affiliates.

from a import af

try:
    import cinderx.jit

    cinderx.jit.force_compile(af)
except ImportError:
    pass

af()
