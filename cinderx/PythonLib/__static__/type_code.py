# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

TYPED_INT8 = 0
TYPED_INT16 = 0
TYPED_INT32 = 0
TYPED_INT64 = 0
TYPED_UINT8 = 0
TYPED_UINT16 = 0
TYPED_UINT32 = 0
TYPED_UINT64 = 0
TYPED_INT_64BIT = 0
TYPED_DOUBLE = 0
TYPED_SINGLE = 0
TYPED_BOOL = 0
TYPED_CHAR = 0
TYPED_OBJECT = 0

try:
    from cinderx.static import (  # noqa: F401
        set_type_code,
        TYPED_BOOL,
        TYPED_CHAR,
        TYPED_DOUBLE,
        TYPED_INT16,
        TYPED_INT32,
        TYPED_INT64,
        TYPED_INT8,
        TYPED_INT_64BIT,
        TYPED_OBJECT,
        TYPED_SINGLE,
        TYPED_UINT16,
        TYPED_UINT32,
        TYPED_UINT64,
        TYPED_UINT8,
    )
except ImportError:

    def set_type_code(func: object, code: object) -> None:
        pass


def type_code(code: int) -> object:
    def inner(func: object) -> object:
        set_type_code(func, code)
        return func

    return inner
