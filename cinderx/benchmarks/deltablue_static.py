# Copyright (c) Meta Platforms, Inc. and affiliates.

from cinderx.compiler.strict import loader as static_python_loader

static_python_loader.install()

import sys

import cinderx.jit
from deltablue_static_lib import delta_blue


if __name__ == "__main__":
    cinderx.jit.auto()

    n = 10000
    if len(sys.argv) > 1:
        n = int(sys.argv[1])
    delta_blue(n)
