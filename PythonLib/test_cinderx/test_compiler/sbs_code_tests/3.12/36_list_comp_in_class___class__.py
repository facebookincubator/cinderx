# pyre-ignore-all-errors
class C:
    [__class__ for x in 'abc']
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('C'),

    # __BLOCK__('entry: 0'),
    ...,
    LOAD_CONST('abc'),
    GET_ITER(0),
    LOAD_FAST_AND_CLEAR('.0'),
    LOAD_FAST_AND_CLEAR('x'),
    SWAP(3),
    ...,
    BUILD_LIST(0),
    SWAP(2),

    # __BLOCK__('start: 1'),
    FOR_ITER(Block(5, label='anchor')),

    # __BLOCK__(': 2'),
    STORE_FAST('x'),
    LOAD_GLOBAL('__class__'),
    LIST_APPEND(2),

    # __BLOCK__('if_cleanup: 4'),
    JUMP_BACKWARD(Block(1, label='start')),

    # __BLOCK__('anchor: 5'),
    END_FOR(0),
    ...,

    # __BLOCK__('end: 7'),
    SWAP(3),
    STORE_FAST('x'),
    STORE_FAST('.0'),
    POP_TOP(0),
    RETURN_CONST(None),

    # __BLOCK__('cleanup: 6'),
    ...,
]
