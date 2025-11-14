# Copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-unsafe

from __future__ import annotations

import functools
import random
import time
from asyncio import iscoroutinefunction
from types import UnionType as typesUnion

# pyre-ignore[21]: No _GenericAlias, _tp_cache
from typing import (
    _GenericAlias,
    _tp_cache,
    Dict,
    final,
    Literal,
    Protocol,
    Type,
    TypeVar,
    Union,
)
from weakref import WeakValueDictionary

from .enum import Enum, IntEnum, StringEnum  # noqa: F401
from .type_code import (  # noqa: F401
    type_code,
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

chkdict = dict[TypeVar("K"), TypeVar("V")]
chklist = list[TypeVar("V")]

FAST_LEN_ARRAY = 0
FAST_LEN_DICT = 0
FAST_LEN_INEXACT = 0
FAST_LEN_LIST = 0
FAST_LEN_SET = 0
FAST_LEN_STR = 0
FAST_LEN_TUPLE = 0
PRIM_OP_ADD_DBL = 0
PRIM_OP_ADD_INT = 0
PRIM_OP_AND_INT = 0
PRIM_OP_DIV_DBL = 0
PRIM_OP_DIV_INT = 0
PRIM_OP_DIV_UN_INT = 0
PRIM_OP_EQ_DBL = 0
PRIM_OP_EQ_INT = 0
PRIM_OP_GE_DBL = 0
PRIM_OP_GE_INT = 0
PRIM_OP_GE_UN_INT = 0
PRIM_OP_GT_DBL = 0
PRIM_OP_GT_INT = 0
PRIM_OP_GT_UN_INT = 0
PRIM_OP_INV_INT = 0
PRIM_OP_LE_DBL = 0
PRIM_OP_LE_INT = 0
PRIM_OP_LE_UN_INT = 0
PRIM_OP_LSHIFT_INT = 0
PRIM_OP_LT_DBL = 0
PRIM_OP_LT_INT = 0
PRIM_OP_LT_UN_INT = 0
PRIM_OP_MOD_DBL = 0
PRIM_OP_MOD_INT = 0
PRIM_OP_MOD_UN_INT = 0
PRIM_OP_MUL_DBL = 0
PRIM_OP_MUL_INT = 0
PRIM_OP_NE_DBL = 0
PRIM_OP_NE_INT = 0
PRIM_OP_NEG_DBL = 0
PRIM_OP_NEG_INT = 0
PRIM_OP_NOT_INT = 0
PRIM_OP_OR_INT = 0
PRIM_OP_POW_DBL = 0
PRIM_OP_POW_INT = 0
PRIM_OP_POW_UN_INT = 0
PRIM_OP_RSHIFT_INT = 0
PRIM_OP_RSHIFT_UN_INT = 0
PRIM_OP_SUB_DBL = 0
PRIM_OP_SUB_INT = 0
PRIM_OP_XOR_INT = 0
SEQ_ARRAY_INT64 = 0
SEQ_CHECKED_LIST = 0
SEQ_LIST = 0
SEQ_LIST_INEXACT = 0
SEQ_REPEAT_INEXACT_NUM = 0
SEQ_REPEAT_INEXACT_SEQ = 0
SEQ_REPEAT_PRIMITIVE_NUM = 0
SEQ_REPEAT_REVERSED = 0
SEQ_SUBSCR_UNCHECKED = 0
SEQ_TUPLE = 0

try:  # noqa: C901
    from cinderx.static import (  # noqa: F401
        __build_cinder_class__,
        chkdict,
        chklist,
        FAST_LEN_ARRAY,
        FAST_LEN_DICT,
        FAST_LEN_INEXACT,
        FAST_LEN_LIST,
        FAST_LEN_SET,
        FAST_LEN_STR,
        FAST_LEN_TUPLE,
        is_static_callable,
        is_static_module,
        is_type_static,
        make_context_decorator_wrapper,
        make_recreate_cm,
        posix_clock_gettime_ns,
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
        set_type_final,
        set_type_static,
        set_type_static_final,
        staticarray,
        StaticTypeError,
    )
except ImportError:
    RAND_MAX = (1 << 31) - 1
    __build_cinder_class__ = __build_class__
    static = None

    def is_type_static(_t):
        return False

    def is_static_module(_m):
        return False

    def is_static_callable(_c):
        return False

    def make_recreate_cm(_typ):
        def _recreate_cm(self):
            return self

        return _recreate_cm

    def make_context_decorator_wrapper(decorator, wrapper_func, wrapped_func):
        return wrapper_func

    def posix_clock_gettime_ns():
        return time.clock_gettime_ns(time.CLOCK_MONOTONIC)

    def rand():
        return random.randint(0, RAND_MAX)

    def set_type_final(_t):
        return _t

    def set_type_static(_t):
        return _t

    def set_type_static_final(_t):
        return _t

    class staticarray:
        def __init__(self, size: int) -> None:
            self._data = [0 for _ in range(size)]

        def __getitem__(self, idx: int) -> None:
            return self._data[idx]

        def __setitem__(self, idx: int, val: int) -> None:
            self._data[idx] = val

        def __len__(self) -> int:
            return len(self._data)

        def __class_getitem__(cls, key) -> type[staticarray]:
            return staticarray

    class StaticTypeError(TypeError):
        pass


try:
    import cinder
except ImportError:
    cinder = None


pydict = dict
PyDict = Dict

clen = len

crange = range


@set_type_final
@type_code(TYPED_UINT64)
class size_t(int):
    pass


@set_type_final
@type_code(TYPED_INT64)
class ssize_t(int):
    pass


@set_type_final
@type_code(TYPED_INT8)
class int8(int):
    pass


byte = int8


@set_type_final
@type_code(TYPED_INT16)
class int16(int):
    pass


@set_type_final
@type_code(TYPED_INT32)
class int32(int):
    pass


@set_type_final
@type_code(TYPED_INT64)
class int64(int):
    pass


@set_type_final
@type_code(TYPED_UINT8)
class uint8(int):
    pass


@set_type_final
@type_code(TYPED_UINT16)
class uint16(int):
    pass


@set_type_final
@type_code(TYPED_UINT32)
class uint32(int):
    pass


@set_type_final
@type_code(TYPED_UINT64)
class uint64(int):
    pass


@set_type_final
@type_code(TYPED_SINGLE)
class single(float):
    pass


@set_type_final
@type_code(TYPED_DOUBLE)
class double(float):
    pass


@set_type_final
@type_code(TYPED_CHAR)
class char(int):
    pass


@set_type_final
@type_code(TYPED_BOOL)
class cbool(int):
    pass


TVarOrType = Union[TypeVar, Type[object]]


def _subs_tvars(
    tp: TVarOrType,
    tvars: tuple[TVarOrType, ...],
    subs: tuple[TVarOrType, ...],
) -> type[object]:
    """Substitute type variables 'tvars' with substitutions 'subs'.
    These two must have the same length.
    """
    args = getattr(tp, "__args__", None)
    if args is None:
        # pyre-ignore[7]: Expected `Type[object]` but got `typing.Tuple[Union[Type[object], TypeVar], ...]`.
        return tp

    new_args = list(args)
    for a, arg in enumerate(new_args):
        if isinstance(arg, TypeVar):
            for i, tvar in enumerate(tvars):
                if arg == tvar:
                    if (
                        # pyre-ignore[16]: `object` has no attribute `__constraints__`.
                        tvar.__constraints__
                        and not isinstance(subs[i], TypeVar)
                        # pyre-ignore[6]: In call `issubclass`, ...
                        and not issubclass(subs[i], tvar.__constraints__)
                    ):
                        raise TypeError(
                            f"Invalid type for {tvar.__name__}: {subs[i].__name__} when instantiating {tp.__name__}"
                        )

                    new_args[a] = subs[i]
        else:
            new_args[a] = _subs_tvars(arg, tvars, subs)

    return _replace_types(tp, tuple(new_args))


def _collect_type_vars(types: tuple[TVarOrType, ...]) -> tuple[TypeVar, ...]:
    """Collect all type variable contained in types in order of
    first appearance (lexicographic order). For example::

        _collect_type_vars((T, List[S, T])) == (T, S)
    """
    tvars = []
    for t in types:
        if isinstance(t, TypeVar) and t not in tvars:
            tvars.append(t)
        if hasattr(t, "__parameters__"):
            tvars.extend([t for t in t.__parameters__ if t not in tvars])
    return tuple(tvars)


def make_generic_type(
    gen_type: type[object], params: tuple[TypeVar | type[object], ...]
) -> Type[object]:
    # pyre-ignore[16]: object has no attribute __parameters__
    if len(params) != len(gen_type.__parameters__):
        raise TypeError(f"Incorrect number of type arguments for {gen_type.__name__}")

    # Substitute params into __args__ replacing instances of __parameters__
    return _subs_tvars(
        gen_type,
        gen_type.__parameters__,
        params,
    )


def _replace_types(
    gen_type: TVarOrType, subs: tuple[type[object], ...]
) -> type[object]:
    # pyre-ignore[16]: object has no attribute __origin__
    existing_inst = gen_type.__origin__.__insts__.get(subs)

    if existing_inst is not None:
        return existing_inst

    # Check if we have a full instantation, and verify the constraints
    new_dict = dict(gen_type.__dict__)
    has_params = False
    for sub in subs:
        if isinstance(sub, TypeVar) or hasattr(sub, "__parameters__"):
            has_params = True
            continue

    # Remove the existing StaticGeneric base...
    bases = tuple(
        # pyre-ignore[16]: object has no attribute __orig_bases__
        base
        for base in gen_type.__orig_bases__
        if not isinstance(base, StaticGeneric)
    )

    new_dict["__args__"] = subs
    if not has_params:
        # Instantiated types don't have generic parameters anymore.
        del new_dict["__parameters__"]
    else:
        new_vars = _collect_type_vars(subs)
        new_gen = StaticGeneric()
        new_gen.__parameters__ = new_vars
        new_dict["__orig_bases__"] = bases + (new_gen,)
        bases += (StaticGeneric,)
        new_dict["__parameters__"] = new_vars

    # Eventually we'll want to have some processing of the members here to
    # bind the generics through.  That may be an actual process which creates
    # new objects with the generics bound, or a virtual process.  For now
    # we just propagate the members to the new type.
    param_names = ", ".join(param.__name__ for param in subs)

    res = type(f"{gen_type.__origin__.__name__}[{param_names}]", bases, new_dict)
    res.__origin__ = gen_type

    if cinder is not None:
        cinder.freeze_type(res)

    gen_type.__origin__.__insts__[subs] = res
    return res


def _runtime_impl(f):
    """marks a generic function as being runtime-implemented"""
    f.__runtime_impl__ = True
    return f


class StaticGeneric:
    """Base type used to mark static-Generic classes.  Instantations of these
    classes share different generic types and the generic type arguments can
    be accessed via __args___"""

    # pyre-ignore[16]: typing has no attribute _tp_cache
    @_tp_cache
    def __class_getitem__(
        cls, elem_type: tuple[TypeVar | type[object]]
    ) -> StaticGeneric | type[object]:
        if not isinstance(elem_type, tuple):
            # we specifically recurse to hit the type cache
            return cls[elem_type,]

        if cls is StaticGeneric:
            res = StaticGeneric()
            res.__parameters__ = elem_type
            return res

        return set_type_static_final(make_generic_type(cls, elem_type))

    def __init_subclass__(cls) -> None:
        # pyre-ignore[16]: StaticGeneric has no attribute __orig__bases__
        type_vars = _collect_type_vars(cls.__orig_bases__)
        cls.__origin__ = cls
        cls.__parameters__ = type_vars
        if not hasattr(cls, "__args__"):
            cls.__args__ = type_vars
        cls.__insts__ = WeakValueDictionary()

    def __mro_entries__(self, bases) -> tuple[type[object], ...]:
        return (StaticGeneric,)

    def __repr__(self) -> str:
        return (
            "<StaticGeneric: "
            + ", ".join([param.__name__ for param in self.__parameters__])
            + ">"
        )


def box(o):
    return o


def unbox(o):
    return o


def allow_weakrefs(klass):
    return klass


def dynamic_return(func):
    return func


def inline(func):
    return func


def _donotcompile(func):
    return func


def cast(typ, val):
    union_args = None
    if isinstance(typ, _GenericAlias):
        typ, args = typ.__origin__, typ.__args__
        if typ is Union:
            union_args = args
    elif type(typ) is typesUnion:
        union_args = typ.__args__
    if union_args:
        typ = None
        if len(union_args) == 2:
            if union_args[0] is type(None):  # noqa: E721
                typ = union_args[1]
            elif union_args[1] is type(None):  # noqa: E721
                typ = union_args[0]
        if typ is None:
            raise ValueError("cast expects type or Optional[T]")
        if val is None:
            return None

    inst_type = type(val)
    if typ not in inst_type.__mro__:
        raise TypeError(f"expected {typ.__name__}, got {type(val).__name__}")

    return val


def prod_assert(value: bool, message: str | None = None):
    if not value:
        raise AssertionError(message) if message else AssertionError()


CheckedDict = chkdict
CheckedList = chklist


async def _return_none():
    """This exists solely as a helper for the ContextDecorator C implementation"""
    pass


class ExcContextDecorator:
    def __enter__(self) -> ExcContextDecorator:
        return self

    def __exit__(self, exc_type: object, exc_value: object, traceback: object) -> bool:
        return False

    @final
    def __call__(self, func):
        if not iscoroutinefunction(func):

            @functools.wraps(func)
            def _no_profile_inner(*args, **kwds):
                with self._recreate_cm():
                    return func(*args, **kwds)

        else:

            @functools.wraps(func)
            async def _no_profile_inner(*args, **kwds):
                with self._recreate_cm():
                    return await func(*args, **kwds)

        # This will replace the vector call entry point with a C implementation.
        # We still want to return a function object because various things check
        # for functions or if an object is a co-routine.
        return make_context_decorator_wrapper(self, _no_profile_inner, func)


# pyre-ignore[16]: `Type` has no attribute `_recreate_cm`.
ExcContextDecorator._recreate_cm = make_recreate_cm(ExcContextDecorator)
set_type_static(ExcContextDecorator)


class ContextDecorator(ExcContextDecorator):
    """A ContextDecorator which cannot suppress exceptions."""

    def __exit__(
        self, exc_type: object, exc_value: object, traceback: object
    ) -> Literal[False]:
        return False


set_type_static(ContextDecorator)


def native(so_path):
    def _inner_native(func):
        return func

    return _inner_native


Array = staticarray  # noqa: F811


def mixin(cls):
    return cls


TClass = TypeVar("TClass", bound=Type[object])


class ClassDecorator(Protocol):
    def __call__(self, cls: TClass) -> TClass: ...
