# Copyright (c) Meta Platforms, Inc. and affiliates.

from a import af

try:
    import cinderjit

    cinderjit.force_compile(af)
except ImportError:
    pass

af()
