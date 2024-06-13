# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

try:
    from cinderx.compiler.strict.runtime import (
        _mark_cached_property,
        extra_slot,
        freeze_type,
        loose_slots,
        mutable,
        set_freeze_enabled,
        strict_slots,
    )
except ImportError:

    def _mark_cached_property(o: object) -> object:
        return o

    def extra_slot(cls: object, o: object) -> object:
        return cls

    def freeze_type(o: object) -> object:
        return o

    def loose_slots(o: object) -> object:
        return o

    def mutable(o: object) -> object:
        return o

    def set_freeze_enabled(o: object) -> object:
        return o

    def strict_slots(o: object) -> object:
        return o


allow_side_effects = object()
