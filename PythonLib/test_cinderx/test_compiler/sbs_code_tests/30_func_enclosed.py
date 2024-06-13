# pyre-ignore-all-errors
def foo():
    a = 1

    def inner():
        a

# EXPECTED:
[
    LOAD_CONST(Code(('foo'))),
    LOAD_CONST('foo'),
    MAKE_FUNCTION(0),
    ...,
    CODE_START('foo'),
    LOAD_CONST(1),
    STORE_DEREF('a'),
    LOAD_CLOSURE('a'),
    BUILD_TUPLE(1),
    LOAD_CONST(Code(('inner'))),
    LOAD_CONST('foo.<locals>.inner'),
    MAKE_FUNCTION(8),
    ...,
    CODE_START('inner'),
    LOAD_DEREF('a'),
    ...,
]
