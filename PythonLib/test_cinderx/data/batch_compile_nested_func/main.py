# Copyright (c) Meta Platforms, Inc. and affiliates.

from mod import outer

try:
    import cinderjit
except ImportError:
    cinderjit = None

if cinderjit:
    cinderjit.precompile_all()
    cinderjit.disable()

inner = outer()

if cinderjit:
    assert cinderjit.is_jit_compiled(inner)

print(inner())
