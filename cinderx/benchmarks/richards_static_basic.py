# Copyright (c) Meta Platforms, Inc. and affiliates.

from cinderx.compiler.strict import loader as static_python_loader

static_python_loader.install()

import sys

import cinderx.jit

from .richards_static_basic_lib import Richards


if __name__ == "__main__":
    cinderx.jit.auto()

    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    Richards().run(num_iterations)
