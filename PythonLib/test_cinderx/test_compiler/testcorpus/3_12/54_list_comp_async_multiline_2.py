# pyre-ignore-all-errors
async def run_list():
    return [i + 1 async for seq in f([(10, 20), (30,)])
            for i in seq]
