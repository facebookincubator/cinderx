# pyre-ignore-all-errors
async def run_dict():
    return {i + 1: i + 2 async for i in f([10, 20])}
