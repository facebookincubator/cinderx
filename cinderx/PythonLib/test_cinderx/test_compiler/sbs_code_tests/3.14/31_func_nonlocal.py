# pyre-ignore-all-errors
def foo():
    a = 1

    def bar():
        nonlocal a
        a = 2
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST(Code('foo')),
    MAKE_FUNCTION(0),
    ...,
    CODE_START('foo'),

    # __BLOCK__('start: 1'),
    ...,    
    STORE_DEREF('a'),
    LOAD_FAST_BORROW('a'),
    BUILD_TUPLE(1),
    LOAD_CONST(Code('bar')),
    MAKE_FUNCTION(0),
    SET_FUNCTION_ATTRIBUTE(8),
    ...,
    CODE_START('bar'),
    ...,
    STORE_DEREF('a'),
    ...,
]
