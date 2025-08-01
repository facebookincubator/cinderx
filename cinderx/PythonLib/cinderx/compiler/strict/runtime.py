# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations


try:
    # pyre-ignore[21]: Undefined import
    from cinderx import freeze_type as cinder_freeze
except ImportError:
    cinder_freeze = None


try:
    from cinder import warn_on_inst_dict
except ImportError:
    warn_on_inst_dict = None


__all__ = [
    "freeze_type",
    "loose_slots",
    "strict_slots",
    "mutable",
    "extra_slot",
    "_mark_cached_property",
    "set_freeze_enabled",
]

TYPE_FREEZE_ENABLED = True


def set_freeze_enabled(flag: bool) -> bool:
    "returns old value"
    global TYPE_FREEZE_ENABLED
    old = TYPE_FREEZE_ENABLED
    TYPE_FREEZE_ENABLED = flag
    return old


def freeze_type(obj: type[object]) -> type[object]:
    if cinder_freeze is not None:
        if TYPE_FREEZE_ENABLED:
            cinder_freeze(obj)
        elif warn_on_inst_dict is not None:
            warn_on_inst_dict(obj)

    return obj


def loose_slots(obj: type[object]) -> type[object]:
    """Indicates that a type defined in a strict module should support assigning
    additional variables to __dict__ to support migration."""
    if warn_on_inst_dict is not None:
        warn_on_inst_dict(obj)
    return obj


def mutable(obj: type[object]) -> type[object]:
    """Marks a type defined in a strict module as supporting mutability"""
    return obj


def strict_slots(obj: type[object]) -> type[object]:
    """Marks a type defined in a strict module to get slots automatically
    and no __dict__ is created"""
    return obj


def extra_slot(obj: type[object], _name: str) -> type[object]:
    """mark `name` to be part of __slots__ in obj"""
    return obj


def _mark_cached_property(obj: object, is_async: bool, original_dec: object) -> object:
    return obj
