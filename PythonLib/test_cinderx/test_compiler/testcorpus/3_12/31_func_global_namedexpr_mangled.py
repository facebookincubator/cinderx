# pyre-unsafe
class Foo:
    def f(self_):
        global __x1
        [__x1 := 2 for a in [3]]
