# pyre-ignore-all-errors
def foo():
    a = 1

    def inner():
        a
# EXPECTED:
[
    ...,
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    STORE_NAME('foo'),
    ...,

    CODE_START('foo'),
    MAKE_CELL(1),
    ...,
    RETURN_CONST(None),

    CODE_START('inner'),
    COPY_FREE_VARS(1),
    ...,
]
