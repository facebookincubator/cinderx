# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

from __future__ import annotations

import contextlib
import os
from collections.abc import Callable
from functools import wraps
from types import TracebackType

# install() runs at CinderX startup, so avoid importing typing at runtime.
TYPE_CHECKING = False
if TYPE_CHECKING:
    from typing import TypeVar

    _T = TypeVar("_T")


# The stdlib contextlib._GeneratorContextManager.__exit__ advances the wrapped
# generator with `next(self.gen)` and detects the generator finishing by
# catching StopIteration. Raising and catching StopIteration on every `with`
# exit is expensive, and in JIT-compiled code it forces a deopt (an exception
# raised inside a try block). This replacement calls the CinderX C helper
# _next_or_sentinel, which advances the generator without raising when it
# finishes, returning a sentinel object instead.
#
# Likewise, the stdlib os._Environ.__getitem__ wraps its dict lookup in
# try/except KeyError, raising and catching KeyError on every miss. This
# replacement looks the value up with dict.get and a sentinel, so the hit
# path has no exception handling; only a genuine miss raises KeyError.
#
# Likewise, MutableMapping.get calls __getitem__ and catches KeyError on
# every miss. This replacement looks the value up directly with dict.get
# and a sentinel, so a miss returns the default with no exception handling.


# The installed replacement, kept so install() can tell it has already run.
#
# Type dictionaries are never cleared during interpreter shutdown, so whatever
# this closure captures outlives finalize_modules(). It must not reach the
# _cinderx module so it's properly finalized.
_patched_exit: Callable[..., bool | None] | None = None
_patched_getitem: Callable[..., str] | None = None
_patched_get: Callable[..., object] | None = None


def install() -> None:
    """Replace contextlib._GeneratorContextManager.__exit__ with a version
    that avoids the StopIteration round-trip on the normal (no-exception) exit
    path, os._Environ.__getitem__ with a version that avoids the
    KeyError round-trip on the hit path, and os._Environ.get with a version
    that avoids the KeyError round-trip on a miss. No-op if the CinderX
    native helper is unavailable."""
    global _patched_exit, _patched_get, _patched_getitem

    if _patched_exit is not None:
        return

    try:
        # _cinderx is only importable where CinderX is supported, which is the
        # only place install() is called.
        # pyrefly: ignore [missing-import]
        from _cinderx import _next_or_sentinel, _NEXT_SENTINEL
    except ImportError:
        return

    # Captured once so the exception path keeps using the real stdlib logic
    # (which is version-specific and left unchanged here).
    original_exit = contextlib._GeneratorContextManager.__exit__

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible but its unparameterized self type does not match the generic
    # method slot.
    @wraps(original_exit)
    def __exit__(
        # The generic arity of _GeneratorContextManager differs across Python
        # versions; leave it unparameterized.
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

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible but its unparameterized self type does not match the generic
    # method slot.
    # pyrefly: ignore [bad-assignment]
    contextlib._GeneratorContextManager.__exit__ = __exit__

    # A unique sentinel: stored environ values are never this object, so
    # getting it back from dict.get means the key was absent.
    _sentinel = object()
    original_getitem = os._Environ.__getitem__

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible.
    @wraps(original_getitem)
    def __getitem__(self: os._Environ, key: str) -> str:
        # _data is present at runtime but omitted from the typeshed stub.
        # pyrefly: ignore [missing-attribute]
        value = self._data.get(self.encodekey(key), _sentinel)
        if value is _sentinel:
            raise KeyError(key) from None
        return self.decodevalue(value)

    _patched_getitem = __getitem__

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible.
    # pyrefly: ignore [bad-assignment]
    os._Environ.__getitem__ = __getitem__

    original_get = os._Environ.get

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible. The stored value must be decoded, and a miss must return
    # the default unchanged, so dict.get alone is not enough: it would
    # return the encoded value on a hit and leave the default to be decoded
    # on a miss. The sentinel distinguishes a miss from a stored value.
    @wraps(original_get)
    def get(self: os._Environ, key: str, default: _T | None = None) -> str | _T | None:
        # _data is present at runtime but omitted from the typeshed stub.
        # pyrefly: ignore [missing-attribute]
        value = self._data.get(self.encodekey(key), _sentinel)
        if value is _sentinel:
            return default
        return self.decodevalue(value)

    _patched_get = get

    # Monkeypatching the stdlib method; the replacement is behaviorally
    # compatible.
    # pyrefly: ignore [bad-assignment]
    os._Environ.get = get
