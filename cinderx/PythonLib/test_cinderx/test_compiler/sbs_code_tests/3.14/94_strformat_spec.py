# pyre-ignore-all-errors
def f(name, args):
    return f"foo.{name:0}"
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    ...,
    LOAD_CONST('foo.'),
    ...,
    LOAD_CONST('0'),
    FORMAT_WITH_SPEC(0),
    BUILD_STRING(2),
    ...,
]
