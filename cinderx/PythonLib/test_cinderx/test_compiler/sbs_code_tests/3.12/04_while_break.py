# pyre-ignore-all-errors
for i in range(42):
    if i == 4:
        break
# EXPECTED:
[
    ...,
    FOR_ITER(Block(6, label='for_cleanup')),
    STORE_NAME('i'),
    LOAD_NAME('i'),
    LOAD_CONST(4),
    COMPARE_OP('=='),
    POP_JUMP_IF_TRUE(Block(3)),
    JUMP_BACKWARD(Block(1, label='for_start')),
    POP_TOP(0),
    RETURN_CONST(None),
    END_FOR(0),
    ...,
]
