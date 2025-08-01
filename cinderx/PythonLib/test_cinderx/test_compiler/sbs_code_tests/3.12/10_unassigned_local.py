# pyre-ignore-all-errors
def f(a):
    if a:
        b = 10
    b
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    ...,

    # __BLOCK__('start: 1'),
    LOAD_FAST('a'),
    POP_JUMP_IF_FALSE(Block(3, label='if_end')),

    # __BLOCK__(': 2'),
    LOAD_CONST(10),
    STORE_FAST('b'),

    # __BLOCK__('if_end: 3'),
    LOAD_FAST_CHECK('b'),
    POP_TOP(0),
    RETURN_CONST(None),
]
