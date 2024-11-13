# pyre-ignore-all-errors
def f():
    while True:
        yield from g
# EXPECTED:
[
    ...,
    CODE_START('f'),
    ...,
    GET_YIELD_FROM_ITER(0),
    LOAD_CONST(None),
    SEND(Block(7)),
    ...,
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(2),
    JUMP_BACKWARD_NO_INTERRUPT(Block(5)),
    ...,
    JUMP_BACKWARD(Block(4)),
    CLEANUP_THROW(0),
    JUMP_BACKWARD(Block(7)),
    ...
]
