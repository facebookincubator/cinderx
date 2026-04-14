# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

import random

try:
    from _cinderx import StaticTypeError
except ImportError:

    class StaticTypeError(TypeError):
        pass

try:
    from _static import (
        __build_cinder_class__,
        _clear_dlopen_cache,
        _clear_dlsym_cache,
        _property_missing_fget,
        _property_missing_fset,
        _sizeof_dlopen_cache,
        _sizeof_dlsym_cache,
        chkdict,
        chklist,
        FAST_LEN_ARRAY,
        FAST_LEN_DICT,
        FAST_LEN_INEXACT,
        FAST_LEN_LIST,
        FAST_LEN_SET,
        FAST_LEN_STR,
        FAST_LEN_TUPLE,
        init_subclass,
        install_sp_audit_hook,
        is_static_callable,
        is_static_module,
        is_type_static,
        lookup_native_symbol,
        make_context_decorator_wrapper,
        make_recreate_cm,
        PRIM_OP_ADD_DBL,
        PRIM_OP_ADD_INT,
        PRIM_OP_AND_INT,
        PRIM_OP_DIV_DBL,
        PRIM_OP_DIV_INT,
        PRIM_OP_DIV_UN_INT,
        PRIM_OP_EQ_DBL,
        PRIM_OP_EQ_INT,
        PRIM_OP_GE_DBL,
        PRIM_OP_GE_INT,
        PRIM_OP_GE_UN_INT,
        PRIM_OP_GT_DBL,
        PRIM_OP_GT_INT,
        PRIM_OP_GT_UN_INT,
        PRIM_OP_INV_INT,
        PRIM_OP_LE_DBL,
        PRIM_OP_LE_INT,
        PRIM_OP_LE_UN_INT,
        PRIM_OP_LSHIFT_INT,
        PRIM_OP_LT_DBL,
        PRIM_OP_LT_INT,
        PRIM_OP_LT_UN_INT,
        PRIM_OP_MOD_DBL,
        PRIM_OP_MOD_INT,
        PRIM_OP_MOD_UN_INT,
        PRIM_OP_MUL_DBL,
        PRIM_OP_MUL_INT,
        PRIM_OP_NE_DBL,
        PRIM_OP_NE_INT,
        PRIM_OP_NEG_DBL,
        PRIM_OP_NEG_INT,
        PRIM_OP_NOT_INT,
        PRIM_OP_OR_INT,
        PRIM_OP_POW_DBL,
        PRIM_OP_POW_INT,
        PRIM_OP_POW_UN_INT,
        PRIM_OP_RSHIFT_INT,
        PRIM_OP_RSHIFT_UN_INT,
        PRIM_OP_SUB_DBL,
        PRIM_OP_SUB_INT,
        PRIM_OP_XOR_INT,
        rand,
        RAND_MAX,
        resolve_primitive_descr,
        SEQ_ARRAY_INT64,
        SEQ_CHECKED_LIST,
        SEQ_LIST,
        SEQ_LIST_INEXACT,
        SEQ_REPEAT_INEXACT_NUM,
        SEQ_REPEAT_INEXACT_SEQ,
        SEQ_REPEAT_PRIMITIVE_NUM,
        SEQ_REPEAT_REVERSED,
        SEQ_SUBSCR_UNCHECKED,
        SEQ_TUPLE,
        set_type_code,
        set_type_final,
        set_type_static,
        set_type_static_final,
        staticarray,
        TYPED_ARRAY,
        TYPED_BOOL,
        TYPED_CHAR,
        TYPED_DOUBLE,
        TYPED_INT16,
        TYPED_INT32,
        TYPED_INT64,
        TYPED_INT8,
        TYPED_INT_16BIT,
        TYPED_INT_32BIT,
        TYPED_INT_64BIT,
        TYPED_OBJECT,
        TYPED_SINGLE,
        TYPED_UINT16,
        TYPED_UINT32,
        TYPED_UINT64,
        TYPED_UINT8,
    )
except ImportError:
    import _ctypes

    __build_cinder_class__ = __build_class__

    _dlopen_cache = {}
    _dlsym_cache = {}

    chkdict = dict
    chklist = list

    FAST_LEN_INEXACT = 1 << 4
    FAST_LEN_LIST = 0
    FAST_LEN_DICT = 1
    FAST_LEN_SET = 2
    FAST_LEN_TUPLE = 3
    FAST_LEN_ARRAY = 4
    FAST_LEN_STR = 5

    PRIM_OP_ADD_INT = 0
    PRIM_OP_SUB_INT = 1
    PRIM_OP_MUL_INT = 2
    PRIM_OP_DIV_INT = 3
    PRIM_OP_DIV_UN_INT = 4
    PRIM_OP_MOD_INT = 5
    PRIM_OP_MOD_UN_INT = 6
    PRIM_OP_POW_INT = 7
    PRIM_OP_LSHIFT_INT = 8
    PRIM_OP_RSHIFT_INT = 9
    PRIM_OP_RSHIFT_UN_INT = 10
    PRIM_OP_XOR_INT = 11
    PRIM_OP_OR_INT = 12
    PRIM_OP_AND_INT = 13
    PRIM_OP_ADD_DBL = 14
    PRIM_OP_SUB_DBL = 15
    PRIM_OP_MUL_DBL = 16
    PRIM_OP_DIV_DBL = 17
    PRIM_OP_MOD_DBL = 18
    PRIM_OP_POW_DBL = 19
    PRIM_OP_POW_UN_INT = 20

    PRIM_OP_EQ_INT = 0
    PRIM_OP_NE_INT = 1
    PRIM_OP_LT_INT = 2
    PRIM_OP_LE_INT = 3
    PRIM_OP_GT_INT = 4
    PRIM_OP_GE_INT = 5
    PRIM_OP_LT_UN_INT = 6
    PRIM_OP_LE_UN_INT = 7
    PRIM_OP_GT_UN_INT = 8
    PRIM_OP_GE_UN_INT = 9
    PRIM_OP_EQ_DBL = 10
    PRIM_OP_NE_DBL = 11
    PRIM_OP_LT_DBL = 12
    PRIM_OP_LE_DBL = 13
    PRIM_OP_GT_DBL = 14
    PRIM_OP_GE_DBL = 15

    PRIM_OP_NEG_INT = 0
    PRIM_OP_INV_INT = 1
    PRIM_OP_NEG_DBL = 2
    PRIM_OP_NOT_INT = 3

    TYPED_INT_16BIT = 1
    TYPED_INT_32BIT = 2
    TYPED_INT_64BIT = 3

    TYPED_UINT8 = 0
    TYPED_INT8 = 1
    TYPED_UINT16 = 2
    TYPED_INT16 = 3
    TYPED_UINT32 = 4
    TYPED_INT32 = 5
    TYPED_UINT64 = 6
    TYPED_INT64 = 7
    TYPED_OBJECT = 0x08
    TYPED_DOUBLE = 0x09
    TYPED_SINGLE = 0x0A
    TYPED_CHAR = 0x0B
    TYPED_BOOL = 0x0C
    TYPED_ARRAY = 0x80

    SEQ_LIST = 0
    SEQ_TUPLE = 1
    SEQ_LIST_INEXACT = 2
    SEQ_SUBSCR_UNCHECKED = 1 << 3
    SEQ_REPEAT_INEXACT_SEQ = 1 << 4
    SEQ_REPEAT_INEXACT_NUM = 1 << 5
    SEQ_REPEAT_REVERSED = 1 << 6
    SEQ_REPEAT_PRIMITIVE_NUM = 1 << 7
    SEQ_ARRAY_INT64 = (TYPED_INT64 << 4) | TYPED_ARRAY
    SEQ_CHECKED_LIST = 1 << 8

    RAND_MAX = (1 << 31) - 1

    def _property_missing_fget(*args):
        raise AttributeError("property has no getter")

    def _property_missing_fset(*args):
        raise AttributeError("property has no setter")

    def _clear_dlopen_cache():
        for handle in _dlopen_cache.values():
            try:
                _ctypes.dlclose(handle)
            except OSError:
                pass
        _dlopen_cache.clear()

    def _clear_dlsym_cache():
        _dlsym_cache.clear()

    def _sizeof_dlopen_cache():
        return len(_dlopen_cache)

    def _sizeof_dlsym_cache():
        return len(_dlsym_cache)

    def init_subclass(*args, **kwargs):
        return None

    def install_sp_audit_hook():
        return None

    def is_static_callable(_c):
        return False

    def is_static_module(_m):
        return False

    def is_type_static(_t):
        return False

    def lookup_native_symbol(*args):
        if len(args) != 2:
            raise TypeError("lookup_native_symbol: Expected 2 arguments")
        lib_name, symbol_name = args
        if not isinstance(lib_name, str):
            raise TypeError(
                f"classloader: 'lib_name' must be a str, got '{type(lib_name).__name__}'"
            )
        if not isinstance(symbol_name, str):
            raise TypeError(
                f"classloader: 'symbol_name' must be a str, got '{type(symbol_name).__name__}'"
            )

        handle = _dlopen_cache.get(lib_name)
        if handle is None:
            try:
                handle = _ctypes.dlopen(lib_name)
            except OSError as e:
                raise RuntimeError(
                    f"classloader: Could not load library '{lib_name}'. {e}"
                ) from e
            _dlopen_cache[lib_name] = handle

        key = (lib_name, symbol_name)
        if key not in _dlsym_cache:
            try:
                _dlsym_cache[key] = _ctypes.dlsym(handle, symbol_name)
            except OSError as e:
                raise RuntimeError(
                    f"classloader: unable to lookup '{symbol_name}' in '{lib_name}'. {e}"
                ) from e
        return _dlsym_cache[key]

    def make_context_decorator_wrapper(decorator, wrapper_func, wrapped_func):
        return wrapper_func

    def make_recreate_cm(_typ):
        def _recreate_cm(self):
            return self

        return _recreate_cm

    def rand():
        return random.randint(0, RAND_MAX)

    def resolve_primitive_descr(descr):
        name = descr[1] if len(descr) > 1 else descr[0]
        mapping = {
            "bool": TYPED_BOOL,
            "char": TYPED_CHAR,
            "double": TYPED_DOUBLE,
            "int8": TYPED_INT8,
            "int16": TYPED_INT16,
            "int32": TYPED_INT32,
            "int64": TYPED_INT64,
            "single": TYPED_SINGLE,
            "uint8": TYPED_UINT8,
            "uint16": TYPED_UINT16,
            "uint32": TYPED_UINT32,
            "uint64": TYPED_UINT64,
        }
        try:
            return mapping[name]
        except KeyError as e:
            raise RuntimeError(f"Unsupported primitive type: {descr}") from e

    def set_type_code(func, code):
        try:
            func.__type_code__ = code
        except AttributeError:
            pass

    def set_type_final(_t):
        return _t

    def set_type_static(_t):
        return _t

    def set_type_static_final(_t):
        return _t

    class staticarray:
        def __init__(self, size):
            self._data = [0 for _ in range(size)]

        def __getitem__(self, idx):
            return self._data[idx]

        def __setitem__(self, idx, val):
            self._data[idx] = val

        def __len__(self):
            return len(self._data)

        def __class_getitem__(cls, key):
            return staticarray
