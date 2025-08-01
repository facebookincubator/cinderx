# pyre-ignore-all-errors
def foo(a):
    b = 2

    def inner(c, d):
        def inner_inner():
            return (a, b, c, d)
        yield inner_inner()
# EXPECTED:
[
    ...,
    CODE_START('inner'),

    COPY_FREE_VARS(2),

    MAKE_CELL(0),
    MAKE_CELL(1),

    RETURN_GENERATOR(0),
    POP_TOP(0),
    RESUME(0),
    ...,
]

