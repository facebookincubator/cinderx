# pyre-ignore[3]: Returning None
def outer():    
    x = 0

    # pyre-ignore[3]: Returning None
    def inner1():
        # pyre-ignore[3]: Returning None
        # pyre-ignore[53]: Captured variable `x` is not annotated.
        def inner2():
            print(x)

        nonlocal x
        x += 1 
