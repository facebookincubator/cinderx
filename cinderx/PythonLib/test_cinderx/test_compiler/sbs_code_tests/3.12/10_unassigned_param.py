# pyre-ignore-all-errors
def f(a):
    del a
    a

# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    ...,

    # __BLOCK__('start: 1'),
    DELETE_FAST('a'),
    LOAD_FAST_CHECK('a'),
    ...,
]
