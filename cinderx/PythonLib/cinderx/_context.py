# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from __future__ import annotations

import contextlib
from collections.abc import Callable
from functools import wraps
from types import TracebackType


# The stdlib contextlib._GeneratorContextManager.__exit__ advances the wrapped
# generator with `next(self.gen)` and detects the generator finishing by
# catching StopIteration. Raising and catching StopIteration on every `with`
# exit is expensive, and in JIT-compiled code it forces a deopt (an exception
# raised inside a try block). This replacement calls the CinderX C helper
# _next_or_sentinel, which advances the generator without raising when it
# finishes, returning a sentinel object instead.


# The installed replacement, kept so install() can tell it has already run.
#
# Type dictionaries are never cleared during interpreter shutdown, so whatever
# this closure captures outlives finalize_modules(). It must not reach the
# _cinderx module so it's properly finalized.
_patched_exit: Callable[..., bool | None] | None = None


def install() -> None:
    """Replace contextlib._GeneratorContextManager.__exit__ with a version
    that avoids the StopIteration round-trip on the normal (no-exception) exit
    path. No-op if the CinderX native helper is unavailable."""
    global _patched_exit

    if _patched_exit is not None:
        return

    try:
        # pyre-ignore[21]: _cinderx is only importable where CinderX is
        # supported, which is the only place install() is called.
        from _cinderx import _next_or_sentinel, _NEXT_SENTINEL
    except ImportError:
        return

    # Captured once so the exception path keeps using the real stdlib logic
    # (which is version-specific and left unchanged here).
    original_exit = contextlib._GeneratorContextManager.__exit__

    # pyre-ignore[8]: Monkeypatching the stdlib method; the replacement is
    # behaviorally compatible but its unparameterized self type does not match
    # the generic method slot.
    @wraps(original_exit)
    def __exit__(
        # pyre-ignore[24]: The generic arity of _GeneratorContextManager
        # differs across Python versions; leave it unparameterized.
        self: contextlib._GeneratorContextManager,
        typ: type[BaseException] | None,
        value: BaseException | None,
        traceback: TracebackType | None,
    ) -> bool | None:
        if typ is None:
            if _next_or_sentinel(self.gen) is _NEXT_SENTINEL:
                return False
            # The generator yielded a second time instead of stopping.
            try:
                raise RuntimeError("generator didn't stop")
            finally:
                self.gen.close()
        # Exception path is unchanged; defer to the stdlib implementation.
        return original_exit(self, typ, value, traceback)

    _patched_exit = __exit__

    # pyre-ignore[8]: Monkeypatching the stdlib method; the replacement is
    # behaviorally compatible but its unparameterized self type does not match
    # the generic method slot.
    contextlib._GeneratorContextManager.__exit__ = __exit__
