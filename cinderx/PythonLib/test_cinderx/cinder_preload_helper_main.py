#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

import weakref

import cinder_preload_helper_a
import cinder_preload_helper_b
import cinderx.jit

print("resolving a_func")
ref = weakref.ref(cinder_preload_helper_a.a_func)

print("defining main_func()")


def main_func() -> str:
    return cinder_preload_helper_b.b_func()


print("disabling jit")
# Force main_func to be compiled, which will trigger preloading.
cinderx.jit.force_compile(main_func)
cinderx.jit.disable()

print("jit disabled")

print(type(ref()))
print(main_func())
