# pyre-ignore-all-errors
match x:
    case {0: ([1, 2, {}] | False)} | {1: [[]]}:
        y = 1
