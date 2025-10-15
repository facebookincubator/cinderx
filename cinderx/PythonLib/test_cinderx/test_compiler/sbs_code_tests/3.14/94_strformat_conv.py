# pyre-ignore-all-errors
def f(name, args):
    return f"foo.{name!r}{name!s}{name!a}"
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),
    ...,

    # __BLOCK__('start: 1'),
    LOAD_CONST('foo.'),
    ...,
    CONVERT_VALUE(2),
    FORMAT_SIMPLE(0),
    ...,
    CONVERT_VALUE(1),
    FORMAT_SIMPLE(0),
    ...,
    CONVERT_VALUE(3),
    FORMAT_SIMPLE(0),
    ...,
]
