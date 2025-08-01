# pyre-ignore-all-errors
class C:
    x = 42
    [x for x in 'abc']
    print(x)
# EXPECTED:
[
    # __BLOCK__('entry: 0'),
    ...,
    CODE_START('C'),

    # __BLOCK__('entry: 0'),
    RESUME(0),
    ...,
    LOAD_CONST(42),
    STORE_NAME('x'),
    LOAD_CONST('abc'),
    GET_ITER(0),
    LOAD_FAST_AND_CLEAR('.0'),
    LOAD_FAST_AND_CLEAR('x'),
    ...,

    # __BLOCK__('start: 1'),
    FOR_ITER(Block(5, label='anchor')),

    # __BLOCK__(': 2'),
    ...,

    # __BLOCK__('end: 7'),
    SWAP(3),
    STORE_FAST('x'),
    STORE_FAST('.0'),
    POP_TOP(0),
    PUSH_NULL(0),
    LOAD_NAME('print'),
    LOAD_NAME('x'),
    ...,
]
