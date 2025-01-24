# pyre-ignore-all-errors
def f[T](): pass
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_CONST(Code('<generic parameters of f>')),
    ...,
    MAKE_FUNCTION(0),
    CALL(0),
    STORE_NAME('f'),
    RETURN_CONST(None),

    CODE_START('<generic parameters of f>'),
    ...,
    LOAD_CONST('T'),
    CALL_INTRINSIC_1(7),
    COPY(1),
    STORE_FAST('T'),
    BUILD_TUPLE(1),

    LOAD_CONST(Code(('f'))),
    ...,
    MAKE_FUNCTION(0),

    SWAP(2),
    CALL_INTRINSIC_2(4),
    RETURN_VALUE(0),
    ...,
]
