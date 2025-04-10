# Copyright (c) Meta Platforms, Inc. and affiliates.

import cinderx.jit
from staticmod import caller

caller  # force import of caller
cinderx.jit.disable()

print(caller(1))
