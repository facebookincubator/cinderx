# pyre-ignore-all-errors
async def run_list():
    return [i + 1 for pair in ([10, 20], [30, 40]) if pair[0] > 10
            async for i in f(pair) if i > 30]
