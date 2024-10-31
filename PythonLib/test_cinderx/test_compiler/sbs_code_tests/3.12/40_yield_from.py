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
    SEND(Block(3)),
    ...,
    YIELD_VALUE(0),
    ...,
    RESUME(2),
    JUMP_BACKWARD_NO_INTERRUPT(Block(1)),
    END_SEND(0),

    POP_TOP(0),
    RETURN_CONST(None),
    CALL_INTRINSIC_1(3),
    RERAISE(1),
    CLEANUP_THROW(0),
    JUMP_BACKWARD(Block(3)),
]
