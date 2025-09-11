# pyre-ignore-all-errors
class C:
    locals().update((name, getattr(x, name))
                    for name in dir(x))
