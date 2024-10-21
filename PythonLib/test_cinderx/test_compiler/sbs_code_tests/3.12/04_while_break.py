# pyre-ignore-all-errors
for i in range(42):
    if i == 4:
        break
# EXPECTED:
[
    ...,
    FOR_ITER(Block(6)),
    STORE_NAME('i'),
    LOAD_NAME('i'),
    LOAD_CONST(4),
    COMPARE_OP('=='),
    POP_JUMP_IF_FALSE(Block(5)),
    POP_TOP(0),
    JUMP_FORWARD(Block(8)),
    JUMP_BACKWARD(Block(1)),
    END_FOR(0),
    ...,
]
