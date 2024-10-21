# pyre-ignore-all-errors
async def f():
    await f()
# EXPECTED:
[
    ...,
    LOAD_GLOBAL('f'),
    CALL(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    SEND(Block(3)),
    SETUP_FINALLY(Block(2)),
    YIELD_VALUE(0),
    ...,
]
