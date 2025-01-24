# pyre-ignore-all-errors
async def f():
    await f()
# EXPECTED:
[
    ...,
    LOAD_GLOBAL(('f', 1)),
    CALL(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(5)),
    YIELD_VALUE(0),
    ...,
]
