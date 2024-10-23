# pyre-ignore-all-errors
async def f():
    async for foo in bar:
        pass
# EXPECTED:
[
    ...,
    SETUP_FINALLY(Block(5)),
    GET_ANEXT(0),
    LOAD_CONST(None),
    SEND(Block(4)),
    SETUP_FINALLY(Block(3)),
    YIELD_VALUE(0),
    ...,
    STORE_FAST('foo'),
    ...,
]
