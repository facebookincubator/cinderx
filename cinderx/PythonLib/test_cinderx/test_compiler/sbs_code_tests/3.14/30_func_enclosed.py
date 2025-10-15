# pyre-ignore-all-errors
def foo():
    a = 1

    def inner():
        a
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST(Code((2,0))),
    MAKE_FUNCTION(0),
    ...,
    CODE_START('foo'),

    # __BLOCK__('entry: 0'),
    MAKE_CELL(1),
    ...,

    # __BLOCK__('start: 1'),
    LOAD_SMALL_INT(1),
    STORE_DEREF('a'),
    LOAD_FAST_BORROW('a'),
    BUILD_TUPLE(1),
    LOAD_CONST(Code((5,4))),
    MAKE_FUNCTION(0),
    ...,
    CODE_START('inner'),
    ...,
    # __BLOCK__('start: 1'),
    LOAD_DEREF('a'),
    ...
]
