#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
import sys

from cinderx.compiler.strict import loader as static_python_loader

static_python_loader.install()

from fannkuch_static_basic_lib import DEFAULT_ARG, fannkuch


if __name__ == "__main__":
    num_iterations = 1
    if len(sys.argv) > 1:
        num_iterations = int(sys.argv[1])
    for _ in range(num_iterations):
        res = fannkuch(DEFAULT_ARG)
        assert res == 30
