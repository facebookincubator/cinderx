# pyre-ignore-all-errors
def foo():
    a = 1
    b = 2

    def inner():
        a, b
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
    RETURN_CONST(None),

    CODE_START('inner'),
    COPY_FREE_VARS(2),
    ...,
    RETURN_CONST(None),
]
