# pyre-ignore-all-errors
type X = int
# EXPECTED:
[
    ...,
    LOAD_CONST('X'),
    LOAD_CONST(None),
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    BUILD_TUPLE(3),
    CALL_INTRINSIC_1(11),
    STORE_NAME('X'),
    RETURN_CONST(None),

    CODE_START('X'),
    ...,
    LOAD_GLOBAL('int'),
    RETURN_VALUE(0),
]
