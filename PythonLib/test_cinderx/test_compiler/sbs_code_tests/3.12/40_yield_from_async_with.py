# pyre-ignore-all-errors
async def f():
    async with foo as bar:
        pass
# EXPECTED:
[
    ...,
    CODE_START('f'),
    ...,
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(3)),
    SETUP_FINALLY(Block(2)),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(1)),
    CLEANUP_THROW(0),
    END_SEND(0),
    SETUP_WITH(Block(8)),
    STORE_FAST('bar'),
    POP_BLOCK(0),
    LOAD_CONST(None),
    LOAD_CONST(None),
    LOAD_CONST(None),
    CALL(2),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(7)),
    SETUP_FINALLY(Block(6)),
    YIELD_VALUE(0),
    POP_BLOCK(0),
    RESUME(3),
    JUMP_BACKWARD_NO_INTERRUPT(Block(5)),
    ...,
]
