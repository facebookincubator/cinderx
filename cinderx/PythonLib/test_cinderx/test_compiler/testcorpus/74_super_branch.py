# pyre-ignore-all-errors
class InlineRewriter(ASTRewriter):
    def visit(self, node: TAst, *args: object) -> AST:
        res = super().visit(node, *args)
        assert isinstance(res, AST)
