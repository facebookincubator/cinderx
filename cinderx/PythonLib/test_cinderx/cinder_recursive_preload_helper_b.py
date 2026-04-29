#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.

# Importing helper_c is lazy too — so this whole module init runs INSIDE the
# preload of main_func from helper_main.  Then b_init() below itself triggers
# another preload (for helper_c.c_func), giving us nested preloadFuncAndDeps
# stack frames with their own deleted_units / callback save+restore.
import cinder_recursive_preload_helper_c


def b_init() -> None:
    # Fill the function freelist so that the ones we throw away below are
    # actually deallocated.  This mirrors cinder_preload_helper_b.
    funcs = []
    for _ in range(400):

        def f():
            pass

        funcs.append(f)
    del funcs

    # Now, while the OUTER preloadFuncAndDeps(main_func) frame is still alive
    # and (with the buggy diff) has installed a save/restored callback on this
    # thread, recursively trigger another preload via a lazy import.  The act
    # of touching helper_c.c_func loads helper_c, which itself destroys a
    # bunch of functions during init — those destructions fire the
    # unit_deleted_during_preload callback in the middle of nested preload.
    cinder_recursive_preload_helper_c.c_func()


def b_func() -> str:
    return "b_func!"


# Run b_init at module load time.  The whole point is that this happens during
# the OUTER preload of main_func.
b_init()
