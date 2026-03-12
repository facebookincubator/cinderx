class C:
    def f(self) -> None:
        return None


def g() -> None:
    return C().f()
