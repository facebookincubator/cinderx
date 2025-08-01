# pyre-ignore-all-errors
def f():
    yield from g
# EXPECTED:
[
    ...,
    CODE_START('f'),
    ...,
    LOAD_GLOBAL('g'),
    GET_YIELD_FROM_ITER(0),
    LOAD_CONST(None),

    __BLOCK__('send: 2'),
    SEND(Block(5)),
    ...,
    YIELD_VALUE(0),
    ...,
    RESUME(2),
    JUMP_BACKWARD_NO_INTERRUPT(Block(2)),

    __BLOCK__('exit: 5'),
    END_SEND(0),
    POP_TOP(0),
    RETURN_CONST(None),

    __BLOCK__('fail: 4'),
    CLEANUP_THROW(0),
    JUMP_BACKWARD(Block(5)),

    __BLOCK__('handler: 6'),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
]
