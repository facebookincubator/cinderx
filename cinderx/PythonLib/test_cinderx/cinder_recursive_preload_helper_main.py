#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import cinder_recursive_preload_helper_b
import cinderx.jit


def main_func() -> str:
    return cinder_recursive_preload_helper_b.b_func()


# Trigger a single-function compilation.  Preloading main_func will follow the lazy
# import to helper_b, which itself triggers nested compilations from inside the lazy
# import's side-effects.
cinderx.jit.force_compile(main_func)
cinderx.jit.disable()

print(main_func())
