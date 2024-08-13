# pyre-ignore-all-errors
class C:
    type X = int
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_BUILD_CLASS(0),
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    LOAD_CONST('C'),
    CALL(2),
    STORE_NAME('C'),
    RETURN_CONST(None),

    CODE_START('C'),
    ...,
    LOAD_NAME('__name__'),
    STORE_NAME('__module__'),
    LOAD_CONST('C'),
    STORE_NAME('__qualname__'),
    LOAD_LOCALS(0),
    STORE_DEREF('__classdict__'),
    LOAD_CONST('X'),
    LOAD_CONST(None),
    LOAD_CLOSURE('__classdict__'),
    BUILD_TUPLE(1),
    LOAD_CONST(Code((3,4))),
    MAKE_FUNCTION(8),
    BUILD_TUPLE(3),
    CALL_INTRINSIC_1(11),
    STORE_NAME('X'),
    LOAD_CLOSURE('__classdict__'),
    STORE_NAME('__classdictcell__'),
    RETURN_CONST(None),

    CODE_START('X'),
    ...,
    LOAD_DEREF('__classdict__'),
    LOAD_FROM_DICT_OR_GLOBALS('int'),
    RETURN_VALUE(0),
]
