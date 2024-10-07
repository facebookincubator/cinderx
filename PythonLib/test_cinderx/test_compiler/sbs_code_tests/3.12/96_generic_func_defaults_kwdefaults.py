# pyre-ignore-all-errors
def f[T](x=42, *, y=42): pass
# EXPECTED:
[
    ...,
    PUSH_NULL(0),
    LOAD_CONST((42,)),
    LOAD_CONST(42),
    LOAD_CONST(('y',)),
    BUILD_CONST_KEY_MAP(1),
    SWAP(2),
    LOAD_CONST(Code(("<generic parameters of f>"))),
    ...,
    MAKE_FUNCTION(0),
    SWAP(3),
    CALL(2),
    STORE_NAME('f'),
    RETURN_CONST(None),

    CODE_START('<generic parameters of f>'),
    ...,
    LOAD_CONST('T'),
    CALL_INTRINSIC_1(7),
    COPY(1),
    STORE_FAST('T'),
    BUILD_TUPLE(1),
    LOAD_FAST(0),
    LOAD_FAST(1),
    LOAD_CONST(Code("f")),
    ...,
    MAKE_FUNCTION(3),
    SWAP(2),
    CALL_INTRINSIC_2(4),
    RETURN_VALUE(0),

    CODE_START('f'),
    ...,
    RETURN_CONST(None),
]
