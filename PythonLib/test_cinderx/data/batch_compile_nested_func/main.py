# Copyright (c) Meta Platforms, Inc. and affiliates.

from mod import outer

import cinderx.jit

if cinderx.jit.is_enabled():
    cinderx.jit.precompile_all()
    cinderx.jit.disable()

inner = outer()

if cinderx.jit.is_enabled():
    assert cinderx.jit.is_jit_compiled(inner)

print(inner())
