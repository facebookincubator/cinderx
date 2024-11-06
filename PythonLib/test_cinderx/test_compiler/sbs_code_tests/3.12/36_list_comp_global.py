# pyre-ignore-all-errors
X = 42
def f():
    return [X for y in 'abc']
# EXPECTED:
[
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST('abc'),
    GET_ITER(0),
    LOAD_FAST_AND_CLEAR('y'),
    SWAP(2),
    ...,
    BUILD_LIST(0),
    SWAP(2),

    # __BLOCK__('start: 1'),
    FOR_ITER(Block(5, label='anchor')),

    # __BLOCK__(': 2'),
    STORE_FAST('y'),
    LOAD_GLOBAL('X'),
    LIST_APPEND(2),

    # __BLOCK__('if_cleanup: 4'),
    JUMP_BACKWARD(Block(1, label='start')),

    # __BLOCK__('anchor: 5'),
    END_FOR(0),
    ...,

    # __BLOCK__('end: 7'),
    SWAP(2),
    STORE_FAST('y'),
    RETURN_VALUE(0),

    ...,
]
