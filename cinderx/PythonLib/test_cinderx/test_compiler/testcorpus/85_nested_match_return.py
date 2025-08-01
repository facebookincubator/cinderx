# pyre-ignore-all-errors
def f():
    match statement:
        case ast.Expr(expr):
            match expr:
                case ast.BinOp():
                    pass

    return None
