# pyre-ignore-all-errors
def f():
    return [lambda:x for x in 'abc']
# EXPECTED:
[
    ...,
    CODE_START('f'),

    __BLOCK__('entry: 0'),
    MAKE_CELL(0),
    RESUME(0),

    __BLOCK__('start: 1'),
    LOAD_CONST('abc'),
    GET_ITER(0),
    LOAD_FAST_AND_CLEAR('x'),
    MAKE_CELL(0),
    SWAP(2),
    BUILD_LIST(0),
    SWAP(2),

    __BLOCK__('start: 2'),
    FOR_ITER(Block(6, label='anchor')),

    __BLOCK__(': 3'),
    STORE_DEREF('x'),
    LOAD_CLOSURE('x'),
    BUILD_TUPLE(1),
    LOAD_CONST(Code((3,12))),
    MAKE_FUNCTION(8),
    LIST_APPEND(2),

    __BLOCK__('if_cleanup: 5'),
    JUMP_BACKWARD(Block(2, label='start')),

    __BLOCK__('anchor: 6'),
    END_FOR(0),
    SWAP(2),
    STORE_FAST('x'),
    RETURN_VALUE(0),

    __BLOCK__('cleanup: 7'),
    SWAP(2),
    POP_TOP(0),
    SWAP(2),
    STORE_FAST('x'),
    RERAISE(0),
    ...,
]
