# pyre-ignore-all-errors
type Alias[T: [lambda: T for T in (T, [1])[1]]] = int
