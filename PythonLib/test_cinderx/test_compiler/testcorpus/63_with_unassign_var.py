# pyre-ignore-all-errors
def main(args=None):
    with x:
        badfile = 42
    if badfile:
        pass
