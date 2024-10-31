# pyre-ignore-all-errors
(i async for i in aiter())
# EXPECTED:
[
    ...,
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(2)),
    END_SEND(0),
    POP_BLOCK(0),
    STORE_FAST('i'),
    LOAD_FAST('i'),
    CALL_INTRINSIC_1(4),
    YIELD_VALUE(0),
    RESUME(1),
    ...
]
