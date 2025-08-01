# pyre-ignore-all-errors
def f(self):
    type Alias[T] = lambda: T
    T, = Alias.__type_params__
