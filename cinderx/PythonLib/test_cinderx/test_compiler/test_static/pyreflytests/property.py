class C:
    @property
    def f(self) -> int:
        return 42


def g() -> int:
    return C().f
