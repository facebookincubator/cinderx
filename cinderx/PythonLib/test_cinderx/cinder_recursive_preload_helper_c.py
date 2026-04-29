#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.


def c_func() -> int:
    """
    Allocate then immediately drop a batch of nested closures.  Each dropped
    closure walks triggers PyEvent_FUNC_DESTROYED events and hits the
    unit_deleted_during_preload callback set by whichever preload action
    is currently active.
    """

    funcs = []
    for _ in range(500):

        def inner():
            pass

        funcs.append(inner)
    n = len(funcs)
    del funcs
    return n


# Burn another batch on import, before c_func is even called.  This ensures
# function destructions fire during the *innermost*
# preload(helper_c.__init__), at the moment when the
# unit_deleted_during_preload callback has been swapped to point to helper_c's
# nested preloadFuncAndDeps's deleted_units, which is on a stack frame that is
# about to disappear when the inner preloadFuncAndDeps returns.
_burn = []
for _i in range(500):

    def _f():
        pass

    _burn.append(_f)
del _burn
