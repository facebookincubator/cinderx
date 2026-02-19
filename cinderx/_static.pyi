# (c) Meta Platforms, Inc. and affiliates. Confidential and proprietary.

# pyre-strict

from __future__ import annotations

from typing import Iterable, Iterator, NoReturn, TypeVar

TYPED_INT_UNSIGNED: int
TYPED_INT_SIGNED: int

TYPED_INT_8BIT: int
TYPED_INT_16BIT: int
TYPED_INT_32BIT: int
TYPED_INT_64BIT: int

TYPED_INT8: int
TYPED_INT16: int
TYPED_INT32: int
TYPED_INT64: int

TYPED_UINT8: int
TYPED_UINT16: int
TYPED_UINT32: int
TYPED_UINT64: int

TYPED_OBJECT: int
TYPED_DOUBLE: int
TYPED_SINGLE: int
TYPED_CHAR: int
TYPED_BOOL: int
TYPED_VOID: int
TYPED_STRING: int
TYPED_ERROR: int
TYPED_ARRAY: int

SEQ_LIST: int
SEQ_TUPLE: int
SEQ_LIST_INEXACT: int
SEQ_ARRAY_INT64: int
SEQ_SUBSCR_UNCHECKED: int

SEQ_REPEAT_INEXACT_SEQ: int
SEQ_REPEAT_INEXACT_NUM: int
SEQ_REPEAT_REVERSED: int
SEQ_REPEAT_PRIMITIVE_NUM: int

SEQ_CHECKED_LIST: int

PRIM_OP_EQ_INT: int
PRIM_OP_NE_INT: int
PRIM_OP_LT_INT: int
PRIM_OP_LE_INT: int
PRIM_OP_GT_INT: int
PRIM_OP_GE_INT: int
PRIM_OP_LT_UN_INT: int
PRIM_OP_LE_UN_INT: int
PRIM_OP_GT_UN_INT: int
PRIM_OP_GE_UN_INT: int
PRIM_OP_EQ_DBL: int
PRIM_OP_NE_DBL: int
PRIM_OP_LT_DBL: int
PRIM_OP_LE_DBL: int
PRIM_OP_GT_DBL: int
PRIM_OP_GE_DBL: int

PRIM_OP_ADD_INT: int
PRIM_OP_SUB_INT: int
PRIM_OP_MUL_INT: int
PRIM_OP_DIV_INT: int
PRIM_OP_DIV_UN_INT: int
PRIM_OP_MOD_INT: int
PRIM_OP_MOD_UN_INT: int
PRIM_OP_POW_INT: int
PRIM_OP_POW_UN_INT: int
PRIM_OP_LSHIFT_INT: int
PRIM_OP_RSHIFT_INT: int
PRIM_OP_RSHIFT_UN_INT: int
PRIM_OP_XOR_INT: int
PRIM_OP_OR_INT: int
PRIM_OP_AND_INT: int

PRIM_OP_ADD_DBL: int
PRIM_OP_SUB_DBL: int
PRIM_OP_MUL_DBL: int
PRIM_OP_DIV_DBL: int
PRIM_OP_MOD_DBL: int
PRIM_OP_POW_DBL: int

PRIM_OP_NEG_INT: int
PRIM_OP_INV_INT: int
PRIM_OP_NEG_DBL: int
PRIM_OP_NOT_INT: int

FAST_LEN_INEXACT: int
FAST_LEN_LIST: int
FAST_LEN_DICT: int
FAST_LEN_SET: int
FAST_LEN_TUPLE: int
FAST_LEN_ARRAY: int
FAST_LEN_STR: int

RAND_MAX: int

T = TypeVar("T")
K = TypeVar("K")
V = TypeVar("V")

chklist = list[T]
chkdict = dict[K, V]

class staticarray(Iterable[int]):
    @classmethod
    def __init__(cls, size: int) -> None: ...
    @classmethod
    def __class_getitem__(cls, key: type[object]) -> type[object]: ...
    def __add__(self, other: staticarray) -> staticarray: ...
    def __mul__(self, repeat: int) -> staticarray: ...
    def __rmul__(self, repeat: int) -> staticarray: ...
    def __delitem__(self, index: int) -> None: ...
    def __getitem__(self, index: int) -> int: ...
    def __setitem__(self, index: int, value: int) -> None: ...
    def __len__(self) -> int: ...
    def __repr__(self) -> str: ...

    # This is a lie, staticarray doesn't have an __iter__ method.  This is here so that
    # calling `list(static_array)` will work.  `list()` is typed as taking an
    # Iterable[T], and that doesn't understand Python's support of __getitem__()-based
    # iterables.
    def __iter__(self) -> Iterator[int]: ...

def __build_cinder_class__(
    func: object, name: str, *bases: object, **kwds: object
) -> type[object]: ...
def _clear_dlopen_cache() -> None: ...
def _clear_dlsym_cache() -> None: ...
def _property_missing_fdel(base: object, key: object) -> NoReturn: ...
def _property_missing_fget(base: object) -> NoReturn: ...
def _property_missing_fset(base: object, key: object) -> NoReturn: ...
def _sizeof_dlopen_cache() -> int: ...
def _sizeof_dlsym_cache() -> int: ...
def init_subclass(ty: type[object]) -> None: ...
def install_sp_audit_hook() -> None: ...
def is_static_callable(obj: object) -> bool: ...
def is_static_module(mod: object) -> bool: ...
def is_type_static(ty: type[object]) -> bool: ...
def lookup_native_symbol(lib: str, symbol: str) -> int: ...
def make_context_decorator_wrapper(
    ctx_wrapper: object, wrapper_func: object, func: object
) -> object: ...
def make_recreate_cm(ty: type[object]) -> object: ...
def rand() -> int: ...
def resolve_primitive_descr(descr: object) -> int: ...
def set_type_code(ty: type[object], code: int) -> None: ...
def set_type_final(ty: type[object]) -> type[object]: ...
def set_type_static(ty: type[object]) -> type[object]: ...
def set_type_static_final(ty: type[object]) -> type[object]: ...
