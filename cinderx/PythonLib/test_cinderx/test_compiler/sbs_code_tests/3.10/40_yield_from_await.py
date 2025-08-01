# pyre-ignore-all-errors
async def f():
    await f()
# EXPECTED:
[
    ...,
    LOAD_GLOBAL('f'),
    CALL_FUNCTION(0),
    GET_AWAITABLE(0),
    LOAD_CONST(None),
    YIELD_FROM(0),
    ...,
]
