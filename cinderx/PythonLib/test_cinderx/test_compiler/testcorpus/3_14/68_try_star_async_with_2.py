# pyre-ignore-all-errors
async def x(self):
    try:
        async with x:
            pass
    except* RuntimeError:
        pass
    foo()
