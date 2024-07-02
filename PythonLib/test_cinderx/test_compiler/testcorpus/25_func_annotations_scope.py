# pyre-ignore-all-errors
def foo():
    ann = None
    def bar(a: ann) -> ann:
        pass
