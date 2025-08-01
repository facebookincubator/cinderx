# pyre-ignore-all-errors
class Outer:
    class Inner:
        nonlocal __class__
        __class__ = 42
        def f():
             __class__
