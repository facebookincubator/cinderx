# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys

import cinderx.jit
from cinderx.compiler.strict import loader as static_python_loader

from .richards_static_basic_lib import Richards


if __name__ == "__main__":
    static_python_loader.install()
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    Richards().run(num_iterations)
