# pyre-ignore-all-errors
async def f():
    async for foo in bar:
        pass
# EXPECTED:
[
    ...,
    SETUP_FINALLY(Block(2)),
    GET_ANEXT(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    POP_BLOCK(0),
    STORE_FAST('foo'),
    ...,
]
