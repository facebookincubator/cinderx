# pyre-ignore-all-errors
(i async for i in aiter())
# EXPECTED:
[
    ...,
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(2)),
    CLEANUP_THROW(0),
    END_SEND(0),
    POP_BLOCK(0),
    STORE_FAST('i'),
    LOAD_FAST('i'),
    YIELD_VALUE(0),
    ...,
]