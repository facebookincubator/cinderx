# pyre-ignore-all-errors
def foo(z, *y, x=1, **c):
    a
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST('x'),
    LOAD_SMALL_INT(1),
    BUILD_MAP(1),    
    LOAD_CONST(Code('foo')),
    ...,
    MAKE_FUNCTION(0),
    SET_FUNCTION_ATTRIBUTE(2),
    ...,
    CODE_START('foo'),
    ...,
]
