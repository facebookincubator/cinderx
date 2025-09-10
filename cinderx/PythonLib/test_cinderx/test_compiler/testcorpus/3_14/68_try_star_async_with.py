# pyre-ignore-all-errors
async def main():
    try:
        async with x:            
            pass
    except* CustomException as err:
        pass
    else:
        self.fail('CustomException not raised')
