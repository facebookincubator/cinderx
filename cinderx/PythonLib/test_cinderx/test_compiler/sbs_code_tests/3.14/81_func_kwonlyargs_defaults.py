# pyre-ignore-all-errors
def foo():
    class Foo:
        def bar(a=b.c, *, b=c.d):
            pass
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('foo'),

    ...,
    LOAD_CONST('Foo'),
    ...,
    LOAD_NAME('b'),
    LOAD_ATTR('c'),
    BUILD_TUPLE(1),
    LOAD_CONST('b'),
    LOAD_NAME('c'),
    LOAD_ATTR('d'),
    BUILD_MAP(1),
    ...,
    MAKE_FUNCTION(0),
    SET_FUNCTION_ATTRIBUTE(2),
    SET_FUNCTION_ATTRIBUTE(1),
    ...,
    CODE_START('bar'),
    ...,
]
