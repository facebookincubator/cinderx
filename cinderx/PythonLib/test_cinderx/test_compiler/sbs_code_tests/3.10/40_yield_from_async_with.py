# pyre-ignore-all-errors
async def f():
    async with foo as bar:
        pass
# EXPECTED:
[
    ...,
    CODE_START('f'),
    GEN_START(1),
    LOAD_GLOBAL('foo'),
    BEFORE_ASYNC_WITH(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    SETUP_ASYNC_WITH(Block(2)),
    STORE_FAST('bar'),
    POP_BLOCK(0),
    LOAD_CONST(None),
    DUP_TOP(0),
    DUP_TOP(0),
    CALL_FUNCTION(3),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    POP_TOP(0),
    LOAD_CONST(None),
    RETURN_VALUE(0),
    WITH_EXCEPT_START(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    POP_JUMP_IF_TRUE(Block(4)),
    ...,
]
