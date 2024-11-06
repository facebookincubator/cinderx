# pyre-ignore-all-errors
def f():
    return [x for x in 'abc']
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('f'),

    # __BLOCK__('entry: 0'),
    RESUME(0),
    LOAD_CONST('abc'),
    GET_ITER(0),
    LOAD_FAST_AND_CLEAR('x'),
    SWAP(2),
    SETUP_FINALLY(Block(6, label='cleanup')),
    BUILD_LIST(0),
    SWAP(2),

    # __BLOCK__('start: 1'),
    FOR_ITER(Block(5, label='anchor')),

    # __BLOCK__(': 2'),
    STORE_FAST('x'),
    LOAD_FAST('x'),
    LIST_APPEND(2),

    # __BLOCK__('if_cleanup: 4'),
    JUMP_BACKWARD(Block(1, label='start')),

    # __BLOCK__('anchor: 5'),
    END_FOR(0),
    ...,

    # __BLOCK__('end: 7'),
    SWAP(2),
    STORE_FAST('x'),
    RETURN_VALUE(0),

    # __BLOCK__('cleanup: 6'),
    SWAP(2),
    POP_TOP(0),
    SWAP(2),
    STORE_FAST('x'),
    RERAISE(0),
    ...,
]
