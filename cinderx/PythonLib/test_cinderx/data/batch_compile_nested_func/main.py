# Copyright (c) Meta Platforms, Inc. and affiliates.

from mod import outer

import cinderx.jit

jit_enabled = cinderx.jit.is_enabled()

if jit_enabled:
    cinderx.jit.precompile_all()
    cinderx.jit.disable()

inner = outer()

if jit_enabled:
    assert cinderx.jit.is_jit_compiled(inner)

print(inner())
