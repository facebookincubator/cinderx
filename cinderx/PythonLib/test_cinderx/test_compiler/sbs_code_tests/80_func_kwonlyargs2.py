# pyre-ignore-all-errors
def foo(z, *y, x=1, **c):
    a
# EXPECTED:
[
    ...,
    LOAD_CONST(('x',)),
    BUILD_CONST_KEY_MAP(1),
    LOAD_CONST(Code(('foo'))),
    ...,
    MAKE_FUNCTION(2),
    ...,
    CODE_START('foo'),
    ...,
]
