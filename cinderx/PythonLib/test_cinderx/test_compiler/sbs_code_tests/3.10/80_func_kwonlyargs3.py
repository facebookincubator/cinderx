# pyre-ignore-all-errors
def foo(z, *, x=1, kwo, **c):
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
