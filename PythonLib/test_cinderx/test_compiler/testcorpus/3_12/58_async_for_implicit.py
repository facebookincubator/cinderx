# pyre-unsafe
async def run_list_inside_gen():
    gen = ([i + j async for i in asynciter([1, 2])] for j in [10, 20])
