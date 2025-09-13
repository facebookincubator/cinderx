# Portions copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import operator
from ast import cmpop, Constant, copy_location
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Mapping

from .visitor import ASTRewriter


class PyLimits:
    MAX_INT_SIZE = 128
    MAX_COLLECTION_SIZE = 256
    MAX_STR_SIZE = 4096
    MAX_TOTAL_ITEMS = 1024


# pyre-fixme[5]: Global annotation cannot contain `Any`.
UNARY_OPS: Mapping[type[ast.unaryop], Callable[[Any], object]] = {
    ast.Invert: operator.invert,
    ast.Not: operator.not_,
    ast.UAdd: operator.pos,
    ast.USub: operator.neg,
}
INVERSE_OPS: Mapping[type[cmpop], type[cmpop]] = {
    ast.Is: ast.IsNot,
    ast.IsNot: ast.Is,
    ast.In: ast.NotIn,
    ast.NotIn: ast.In,
}

BIN_OPS: Mapping[type[ast.operator], Callable[[object, object], object]] = {
    ast.Add: operator.add,
    ast.Sub: operator.sub,
    ast.Mult: lambda lhs, rhs: safe_multiply(lhs, rhs, PyLimits),
    ast.Div: operator.truediv,
    ast.FloorDiv: operator.floordiv,
    ast.Mod: lambda lhs, rhs: safe_mod(lhs, rhs, PyLimits),
    ast.Pow: lambda lhs, rhs: safe_power(lhs, rhs, PyLimits),
    ast.LShift: lambda lhs, rhs: safe_lshift(lhs, rhs, PyLimits),
    ast.RShift: operator.rshift,
    ast.BitOr: operator.or_,
    ast.BitXor: operator.xor,
    ast.BitAnd: operator.and_,
}


class DefaultLimits:
    MAX_INT_SIZE = 128
    MAX_COLLECTION_SIZE = 20
    MAX_STR_SIZE = 20
    MAX_TOTAL_ITEMS = 1024


LimitsType = type[PyLimits] | type[DefaultLimits]


# pyre-fixme[2]: Parameter annotation cannot be `Any`.
def safe_lshift(left: Any, right: Any, limits: LimitsType = DefaultLimits) -> object:
    if isinstance(left, int) and isinstance(right, int) and left and right:
        lbits = left.bit_length()
        if (
            right < 0
            or right > limits.MAX_INT_SIZE
            or lbits > limits.MAX_INT_SIZE - right
        ):
            raise OverflowError()

    return left << right


def check_complexity(obj: object, limit: int) -> int:
    if isinstance(obj, (frozenset, tuple)):
        limit -= len(obj)
        for item in obj:
            limit = check_complexity(item, limit)
            if limit < 0:
                break

    return limit


# pyre-fixme[2]: Parameter annotation cannot be `Any`.
def safe_multiply(left: Any, right: Any, limits: LimitsType = DefaultLimits) -> object:
    if isinstance(left, int) and isinstance(right, int) and left and right:
        lbits = left.bit_length()
        rbits = right.bit_length()
        if lbits + rbits > limits.MAX_INT_SIZE:
            raise OverflowError()
    elif isinstance(left, int) and isinstance(right, (tuple, frozenset)):
        rsize = len(right)
        if rsize:
            if left < 0 or left > limits.MAX_COLLECTION_SIZE / rsize:
                raise OverflowError()
            if left:
                if check_complexity(right, limits.MAX_TOTAL_ITEMS // left) < 0:
                    raise OverflowError()
    elif isinstance(left, int) and isinstance(right, (str, bytes)):
        rsize = len(right)
        if rsize:
            if left < 0 or left > limits.MAX_STR_SIZE / rsize:
                raise OverflowError()
    elif isinstance(right, int) and isinstance(left, (tuple, frozenset, str, bytes)):
        return safe_multiply(right, left, limits)

    return left * right


# pyre-fixme[2]: Parameter annotation cannot be `Any`.
def safe_power(left: Any, right: Any, limits: LimitsType = DefaultLimits) -> object:
    if isinstance(left, int) and isinstance(right, int) and left and right > 0:
        lbits = left.bit_length()
        if lbits > limits.MAX_INT_SIZE / right:
            raise OverflowError()

    return left**right


# pyre-fixme[2]: Parameter annotation cannot be `Any`.
def safe_mod(left: Any, right: Any, limits: LimitsType = DefaultLimits) -> object:
    if isinstance(left, (str, bytes)):
        raise OverflowError()

    return left % right


class AstOptimizer(ASTRewriter):
    def __init__(self, optimize: bool = False, string_anns: bool = False) -> None:
        super().__init__()
        self.optimize = optimize
        self.string_anns = string_anns

    def skip_field(self, node: ast.AST, field: str) -> bool:
        if self.string_anns:
            if (
                isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
                and field == "returns"
            ):
                return True
            if isinstance(node, ast.arg) and field == "annotation":
                return True
            if isinstance(node, ast.AnnAssign) and field == "annotation":
                return True
        return False

    def visitUnaryOp(self, node: ast.UnaryOp) -> ast.expr:
        op = self.visit(node.operand)
        if isinstance(op, Constant):
            conv = UNARY_OPS[type(node.op)]
            try:
                return copy_location(Constant(conv(op.value)), node)
            except Exception:
                pass
        elif (
            isinstance(node.op, ast.Not)
            and isinstance(op, ast.Compare)
            and len(op.ops) == 1
        ):
            cmp_op = op.ops[0]
            new_op = INVERSE_OPS.get(type(cmp_op))
            if new_op is not None:
                return self.update_node(op, ops=[new_op()])

        return self.update_node(node, operand=op)

    def visitBinOp(self, node: ast.BinOp) -> ast.expr:
        left = self.visit(node.left)
        right = self.visit(node.right)

        if isinstance(left, Constant) and isinstance(right, Constant):
            handler = BIN_OPS.get(type(node.op))
            if handler is not None:
                try:
                    return copy_location(
                        Constant(handler(left.value, right.value)), node
                    )
                except Exception:
                    pass

        return self.update_node(node, left=left, right=right)

    def makeConstTuple(self, elts: Iterable[ast.expr]) -> Constant | None:
        if all(isinstance(elt, Constant) for elt in elts):
            # pyre-ignore[16]: each elt is a constant at this point.
            return Constant(tuple(elt.value for elt in elts))

        return None

    def visitTuple(self, node: ast.Tuple) -> ast.expr:
        elts = self.walk_list(node.elts)

        if isinstance(node.ctx, ast.Load):
            # pyre-ignore[6]: Can't type walk_list fully yet.
            res = self.makeConstTuple(elts)
            if res is not None:
                return copy_location(res, node)

        return self.update_node(node, elts=elts)

    def visitSubscript(self, node: ast.Subscript) -> ast.expr:
        value = self.visit(node.value)
        slice = self.visit(node.slice)

        if (
            isinstance(node.ctx, ast.Load)
            and isinstance(value, Constant)
            and isinstance(slice, Constant)
        ):
            try:
                return copy_location(
                    Constant(value.value[slice.value]),
                    node,
                )
            except Exception:
                pass

        return self.update_node(node, value=value, slice=slice)

    def _visitIter(self, node: ast.expr) -> ast.expr:
        if isinstance(node, ast.List):
            elts = self.walk_list(node.elts)
            # pyre-ignore[6]: Can't type walk_list fully yet.
            res = self.makeConstTuple(elts)
            if res is not None:
                return copy_location(res, node)
            if not any(isinstance(e, ast.Starred) for e in elts):
                # pyre-fixme[6]: For 1st argument expected `List[expr]` but got
                #  `Sequence[expr]`.
                return copy_location(ast.Tuple(elts=elts, ctx=node.ctx), node)
            return self.update_node(node, elts=elts)
        elif isinstance(node, ast.Set):
            elts = self.walk_list(node.elts)
            # pyre-ignore[6]: Can't type walk_list fully yet.
            res = self.makeConstTuple(elts)
            if res is not None:
                return copy_location(Constant(frozenset(res.value)), node)

            return self.update_node(node, elts=elts)

        return self.generic_visit(node)

    def visitcomprehension(self, node: ast.comprehension) -> ast.comprehension:
        target = self.visit(node.target)
        iter = self.visit(node.iter)
        assert isinstance(iter, ast.expr)
        ifs = self.walk_list(node.ifs)
        iter = self._visitIter(iter)

        return self.update_node(node, target=target, iter=iter, ifs=ifs)

    def visitFor(self, node: ast.For) -> ast.For:
        target = self.visit(node.target)
        iter = self.visit(node.iter)
        assert isinstance(iter, ast.expr)
        body = self.walk_list(node.body)
        orelse = self.walk_list(node.orelse)

        iter = self._visitIter(iter)
        return self.update_node(
            node, target=target, iter=iter, body=body, orelse=orelse
        )

    def visitCompare(self, node: ast.Compare) -> ast.expr:
        left = self.visit(node.left)
        comparators = self.walk_list(node.comparators)

        if isinstance(node.ops[-1], (ast.In, ast.NotIn)):
            # pyre-ignore[6]: Can't type walk_list fully yet.
            new_iter = self._visitIter(comparators[-1])
            if new_iter is not None and new_iter is not comparators[-1]:
                comparators = list(comparators)
                comparators[-1] = new_iter

        return self.update_node(node, left=left, comparators=comparators)

    def visitName(self, node: ast.Name) -> ast.Name | ast.Constant:
        if node.id == "__debug__":
            return copy_location(Constant(not self.optimize), node)

        return self.generic_visit(node)

    def visitNamedExpr(self, node: ast.NamedExpr) -> ast.NamedExpr:
        return self.generic_visit(node)


F_LJUST: int = 1 << 0
F_SIGN: int = 1 << 1
F_BLANK: int = 1 << 2
F_ALT: int = 1 << 3
F_ZERO: int = 1 << 4

FLAG_DICT: dict[str, int] = {
    "-": F_LJUST,
    "+": F_SIGN,
    " ": F_BLANK,
    "#": F_ALT,
    "0": F_ZERO,
}
MAXDIGITS = 3


class UnsupportedFormat(Exception):
    pass


@dataclass(frozen=True)
class FormatInfo:
    spec: str
    flags: int = 0
    width: int | None = None
    prec: int | None = None

    def as_formatted_value(self, arg: ast.expr) -> ast.FormattedValue | None:
        if self.spec not in "sra":
            return None

        res = []
        if not (self.flags & F_LJUST) and self.width:
            res.append(">")
        if self.width is not None:
            res.append(str(self.width))
        if self.prec is not None:
            res.append(f".{self.prec}")

        return copy_location(
            ast.FormattedValue(
                arg,
                ord(self.spec),
                copy_location(ast.Constant("".join(res)), arg) if res else None,
            ),
            arg,
        )


class FormatParser:
    def __init__(self, val: str) -> None:
        self.val = val
        self.size: int = len(val)
        self.pos = 0

    def next_ch(self) -> str:
        """Gets the next character in the format string, or raises an
        exception if we've run out of characters"""
        if self.pos >= self.size:
            raise UnsupportedFormat()
        self.pos += 1
        return self.val[self.pos]

    def parse_int(self) -> int:
        """Parses an integer from the format string"""
        res = 0
        digits = 0
        ch = self.val[self.pos]
        while "0" <= ch <= "9":
            res = res * 10 + ord(ch) - ord("0")
            ch = self.next_ch()
            digits += 1
            if digits >= MAXDIGITS:
                raise UnsupportedFormat()
        return res

    def parse_flags(self, ch: str) -> int:
        """Parse any flags (-, +, " ", #, 0)"""
        flags = 0
        while True:
            flag_val = FLAG_DICT.get(ch)
            if flag_val is not None:
                flags |= flag_val
                ch = self.next_ch()
            else:
                break
        return flags

    def parse_str(self) -> str:
        """Parses a string component of the format string up to a %"""
        has_percents = False
        start = self.pos
        while self.pos < self.size:
            ch = self.val[self.pos]
            if ch != "%":
                self.pos += 1
            elif self.pos + 1 < self.size and self.val[self.pos + 1] == "%":
                has_percents = True
                self.pos += 2
            else:
                break

        component = self.val[start : self.pos]
        if has_percents:
            component = component.replace("%%", "%")
        return component

    def enum_components(self) -> Iterable[str | FormatInfo]:
        """Enumerates the components of the format string and returns a stream
        of interleaved strings and FormatInfo objects"""
        # Parse the string up to the format specifier
        ch = None
        while self.pos < self.size:
            yield self.parse_str()

            if self.pos == self.size:
                return

            assert self.val[self.pos] == "%"

            flags = self.parse_flags(self.next_ch())

            # Parse width
            width = None
            if "0" <= self.val[self.pos] <= "9":
                width = self.parse_int()

            prec = None
            if self.val[self.pos] == ".":
                self.next_ch()
                prec = self.parse_int()

            yield FormatInfo(self.val[self.pos], flags, width, prec)
            self.pos += 1


def enum_format_str_components(val: str) -> Iterable[str | FormatInfo]:
    return FormatParser(val).enum_components()


def set_no_locations(node: ast.expr) -> ast.expr:
    node.lineno = -1
    node.end_lineno = -1
    node.col_offset = -1
    node.end_col_offset = -1
    return node


class AstOptimizer312(AstOptimizer):
    def visitBinOp(self, node: ast.BinOp) -> ast.expr:
        res = super().visitBinOp(node)

        if (
            not isinstance(res, ast.BinOp)
            or not isinstance(res.op, ast.Mod)
            or not isinstance(res.left, ast.Constant)
            or not isinstance(res.left.value, str)
            or not isinstance(res.right, ast.Tuple)
            or any(isinstance(e, ast.Starred) for e in res.right.elts)
        ):
            return res

        return self.optimize_format(res)

    def optimize_format(self, node: ast.BinOp) -> ast.expr:
        left = node.left
        right = node.right
        assert isinstance(left, ast.Constant)
        assert isinstance(right, ast.Tuple)
        assert isinstance(left.value, str)

        try:
            seq = []
            cnt = 0
            for item in enum_format_str_components(left.value):
                if isinstance(item, str):
                    if item:
                        seq.append(set_no_locations(ast.Constant(item)))
                    continue

                if cnt >= len(right.elts):
                    # More format units than items.
                    return node

                formatted = item.as_formatted_value(right.elts[cnt])
                if formatted is None:
                    return node
                seq.append(formatted)
                cnt += 1

            if cnt < len(right.elts):
                # More items than format units.
                return node

            return copy_location(ast.JoinedStr(seq), node)

        except UnsupportedFormat:
            return node


class AstOptimizer314(AstOptimizer312):
    def has_starred(self, e: ast.Tuple) -> bool:
        return any(isinstance(e, ast.Starred) for e in e.elts)

    def visitUnaryOp(self, node: ast.UnaryOp) -> ast.expr:
        op = self.visit(node.operand)
        return self.update_node(node, operand=op)

    def visitBinOp(self, node: ast.BinOp) -> ast.expr:
        lhs = self.visit(node.left)
        rhs = self.visit(node.right)
        if (
            isinstance(lhs, ast.Constant)
            and isinstance(rhs, ast.Tuple)
            and isinstance(lhs.value, str)
            and not self.has_starred(rhs)
        ):
            return self.optimize_format(self.update_node(node, left=lhs, right=rhs))

        return self.update_node(node, left=lhs, right=rhs)

    def visitSubscript(self, node: ast.Subscript) -> ast.expr:
        value = self.visit(node.value)
        slice = self.visit(node.slice)

        return self.update_node(node, value=value, slice=slice)

    def visitMatchValue(self, node: ast.MatchValue) -> ast.MatchValue:
        return self.update_node(node, value=self.fold_const_match_patterns(node.value))

    def visitMatchMapping(self, node: ast.MatchMapping) -> ast.MatchMapping:
        keys = [self.fold_const_match_patterns(key) for key in node.keys]
        patterns = self.walk_list(node.patterns)
        return self.update_node(node, keys=keys, patterns=patterns)

    def fold_const_match_patterns(self, node: ast.expr) -> ast.expr:
        if isinstance(node, ast.UnaryOp):
            if isinstance(node.op, ast.USub) and isinstance(node.operand, ast.Constant):
                return super().visitUnaryOp(node)
        elif isinstance(node, ast.BinOp) and isinstance(node.op, (ast.Add, ast.Sub)):
            if isinstance(node.right, ast.Constant):
                node = self.update_node(
                    node, left=self.fold_const_match_patterns(node.left)
                )
                if isinstance(node.left, ast.Constant):
                    return super().visitBinOp(node)

        return node

    def visitTuple(self, node: ast.Tuple) -> ast.expr:
        elts = self.walk_list(node.elts)

        return self.update_node(node, elts=elts)

    def _visitIter(self, node: ast.expr) -> ast.expr:
        # This optimization has been removed in 3.14
        return node
