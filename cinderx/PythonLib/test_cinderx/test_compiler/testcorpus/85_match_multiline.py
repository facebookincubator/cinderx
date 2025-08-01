# pyre-ignore-all-errors
match child:
   case ast.Expr(ast.Constant(value)) if isinstance(
      value, str
   ) and x:
       pass
   case ast.Constant(_):
       pass

