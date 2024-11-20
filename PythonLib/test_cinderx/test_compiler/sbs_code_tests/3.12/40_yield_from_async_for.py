# pyre-ignore-all-errors
async def f():
    async for foo in bar:
        pass
# EXPECTED:
[
    ...,
    GET_ANEXT(0),
    LOAD_CONST(None),
    SEND(Block(5)),
    YIELD_VALUE(0),
    ...,
    STORE_FAST('foo'),
    ...,
]
