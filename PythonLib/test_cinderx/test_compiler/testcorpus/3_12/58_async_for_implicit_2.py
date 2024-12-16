async def run_list_inside_list_inside_list():
    return [[[i + j + k async for i in asynciter([1, 2])]
                for j in [10, 20]]
            for k in [100, 200]]
