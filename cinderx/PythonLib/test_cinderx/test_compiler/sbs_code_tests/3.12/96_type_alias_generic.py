# pyre-ignore-all-errors
type X[T] = int
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_CONST(Code((2,7))),
    MAKE_FUNCTION(0),
    CALL(0),
    STORE_NAME('X'),
    RETURN_CONST(None),

    CODE_START('<generic parameters of X>'),
    ...,
    LOAD_CONST('X'),
    LOAD_CONST('T'),
    CALL_INTRINSIC_1(7),
    COPY(1),
    STORE_FAST('T'),
    BUILD_TUPLE(1),

    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    BUILD_TUPLE(3),
    CALL_INTRINSIC_1(11),
    RETURN_VALUE(0),

    CODE_START('X'),
    ...,
    LOAD_GLOBAL('int'),
    RETURN_VALUE(0),
]
