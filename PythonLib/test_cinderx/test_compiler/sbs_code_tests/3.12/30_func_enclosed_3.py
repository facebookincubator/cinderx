# pyre-ignore-all-errors
def foo():
    b = 1
    a = 2

    def inner():
        x = 42
        b, a
# EXPECTED:
[
    ...,
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    STORE_NAME('foo'),
    RETURN_CONST(None),

    CODE_START('foo'),
    MAKE_CELL(1),
    MAKE_CELL(2),
    ...,
    LOAD_CONST(Code((6,4))),
    MAKE_FUNCTION(8),
    STORE_FAST('inner'),
    RETURN_CONST(None),

    CODE_START('inner'),
    COPY_FREE_VARS(2),
    ...,
    RETURN_CONST(None),
]
