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
    SEND(Block(6)),
    ...,
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(2),
    JUMP_BACKWARD_NO_INTERRUPT(Block(4)),
    ...,
    JUMP_BACKWARD(Block(3)),
]
