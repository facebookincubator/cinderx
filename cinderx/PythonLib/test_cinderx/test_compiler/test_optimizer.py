# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe
import ast
import math
import sys

from cinderx.compiler.flow_graph_optimizer import (
    _common_constant_oparg,
    CONSTANT_EMPTY_STR,
    CONSTANT_EMPTY_TUPLE,
    CONSTANT_FALSE,
    CONSTANT_MINUS_ONE,
    CONSTANT_NONE,
    CONSTANT_TRUE,
    convert_load_const_to_load_common_constant,
    FlowGraphConstOptimizer315,
    FlowGraphConstOptimizer316,
)
from cinderx.compiler.optimizer import (
    AstOptimizer,
    enum_format_str_components,
    F_LJUST,
    FormatInfo,
    UnsupportedFormat,
)
from cinderx.compiler.pycodegen import CodeGenerator
from cinderx.compiler.unparse import to_expr
from cinderx.test_support import passIf

from .common import CompilerTest


class AstOptimizerTests(CompilerTest):
    class _Comparer:
        def __init__(self, code, test):
            self.code = code
            self.test = test
            self.opt = self.test.to_graph(code)
            self.notopt = self.test.to_graph_no_opt(code)

        def assert_both(self, *args):
            self.test.assertInGraph(
                self.notopt, *args
            )  # should be present w/o peephole
            self.test.assertInGraph(self.opt, *args)  # should be present w/ peephole

        def assert_neither(self, *args):
            self.test.assertNotInGraph(
                self.notopt, *args
            )  # should be absent w/o peephole
            self.test.assertNotInGraph(self.opt, *args)  # should be absent w/ peephole

        def assert_removed(self, *args):
            self.test.assertInGraph(
                self.notopt, *args
            )  # should be present w/o peephole
            self.test.assertNotInGraph(self.opt, *args)  # should be removed w/ peephole

        def assert_added(self, *args):
            self.test.assertInGraph(self.opt, *args)  # should be added w/ peephole
            self.test.assertNotInGraph(
                self.notopt, *args
            )  # should be absent w/o peephole

        def get_instructions(self, graph):
            return [
                instr
                for block in graph.getBlocks()
                for instr in block.getInstructions()
            ]

        def assert_all_removed(self, *args):
            for instr in self.get_instructions(self.opt):
                for arg in args:
                    self.test.assertFalse(instr.opname.startswith(arg))
            for instr in self.get_instructions(self.notopt):
                for arg in args:
                    if instr.opname.startswith(arg):
                        return
            disassembly = self.test.dump_graph(self.notopt)
            self.test.fail(
                "no args were present: " + ", ".join(args) + "\n" + disassembly
            )

        def assert_in_opt(self, *args):
            self.test.assertInGraph(self.opt, *args)

        def assert_not_in_opt(self, *args):
            self.test.assertNotInGraph(self.opt, *args)

        def assert_instr_count(self, opcode, before, after):
            before_instrs = [
                instr
                for instr in self.get_instructions(self.notopt)
                if instr.opname == opcode
            ]
            self.test.assertEqual(len(before_instrs), before)
            after_instrs = [
                instr
                for instr in self.get_instructions(self.opt)
                if instr.opname == opcode
            ]
            self.test.assertEqual(len(after_instrs), after)

    def compare_graph(self, code):
        return AstOptimizerTests._Comparer(code, self)

    def to_graph_no_opt(self, code):
        return self.to_graph(code, ast_optimizer_enabled=False)

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_compile_opt_enabled(self):
        graph = self.to_graph("x = -1")
        self.assertNotInGraph(graph, "UNARY_NEGATIVE")

        graph = self.to_graph_no_opt("x = -1")
        self.assertInGraph(graph, "UNARY_NEGATIVE")

    def test_opt_debug(self):
        graph = self.to_graph("if not __debug__:\n    x = 42")
        self.assertNotInGraph(graph, "STORE_NAME")

        graph = self.to_graph_no_opt("if not __debug__:\n    x = 42")
        self.assertInGraph(graph, "STORE_NAME")

    def test_opt_debug_del(self):
        code = "def f(): del __debug__"
        if sys.version_info >= (3, 15):
            load_true_op = "LOAD_COMMON_CONSTANT"
            load_true_arg = 9
        else:
            load_true_op = "LOAD_CONST"
            load_true_arg = True
        outer_graph = self.to_graph(code)
        for outer_instr in self.graph_to_instrs(outer_graph):
            if outer_instr.opname == "LOAD_CONST" and isinstance(
                outer_instr.oparg, CodeGenerator
            ):
                graph = outer_instr.oparg.graph
                self.assertInGraph(graph, load_true_op, load_true_arg)
                self.assertNotInGraph(graph, "DELETE_FAST", "__debug__")

        outer_graph = self.to_graph_no_opt(code)
        for outer_instr in self.graph_to_instrs(outer_graph):
            if outer_instr.opname == "LOAD_CONST" and isinstance(
                outer_instr.oparg, CodeGenerator
            ):
                graph = outer_instr.oparg.graph
                self.assertNotInGraph(graph, load_true_op, load_true_arg)
                self.assertInGraph(graph, "DELETE_FAST", "__debug__")

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_const_fold(self):
        code = self.compile("x = 0.0\ny=-0.0")
        self.assertEqual(code.co_consts, (0.0, -0.0, None))
        self.assertEqual(math.copysign(1, code.co_consts[0]), 1)
        self.assertEqual(math.copysign(1, code.co_consts[1]), -1)

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_const_fold_tuple(self):
        code = self.compile("x = (0.0, )\ny=(-0.0, )")
        self.assertEqual(code.co_consts, ((0.0,), (-0.0,), None))
        self.assertEqual(math.copysign(1, code.co_consts[0][0]), 1)
        self.assertEqual(math.copysign(1, code.co_consts[1][0]), -1)

    def test_ast_optimizer(self):
        cases = [
            ("+1", "1"),
            ("--1", "1"),
            ("~1", "-2"),
            ("not 1", "False"),
            ("not x is y", "x is not y"),
            ("not x is not y", "x is y"),
            ("not x in y", "x not in y"),
            ("~1.1", "~1.1"),
            ("+'str'", "+'str'"),
            ("1 + 2", "3"),
            ("1 + 3", "4"),
            ("'abc' + 'def'", "'abcdef'"),
            ("b'abc' + b'def'", "b'abcdef'"),
            ("b'abc' + 'def'", "b'abc' + 'def'"),
            ("b'abc' + --2", "b'abc' + 2"),
            ("--2 + 'abc'", "2 + 'abc'"),
            ("5 - 3", "2"),
            ("6 - 3", "3"),
            ("2 * 2", "4"),
            ("2 * 3", "6"),
            ("'abc' * 2", "'abcabc'"),
            ("b'abc' * 2", "b'abcabc'"),
            ("1 / 2", "0.5"),
            ("6 / 2", "3.0"),
            ("6 // 2", "3"),
            ("5 // 2", "2"),
            ("2 >> 1", "1"),
            ("6 >> 1", "3"),
            ("1 | 2", "3"),
            ("1 | 1", "1"),
            ("1 ^ 3", "2"),
            ("1 ^ 1", "0"),
            ("1 & 2", "0"),
            ("1 & 3", "1"),
            ("'abc' + 1", "'abc' + 1"),
            ("1 / 0", "1 / 0"),
            ("1 + None", "1 + None"),
            ("True + None", "True + None"),
            ("True + 1", "2"),
            ("(1, 2)", "(1, 2)"),
            ("(1, 2) * 2", "(1, 2, 1, 2)"),
            ("(1, --2, abc)", "(1, 2, abc)"),
            ("(1, 2)[0]", "1"),
            ("1[0]", "1[0]"),
            ("x[+1]", "x[1]"),
            ("(+1)[x]", "1[x]"),
            ("[x for x in [1,2,3]]", "[x for x in (1, 2, 3)]"),
            ("(x for x in [1,2,3])", "(x for x in (1, 2, 3))"),
            ("{x for x in [1,2,3]}", "{x for x in (1, 2, 3)}"),
            ("{x for x in [--1,2,3]}", "{x for x in (1, 2, 3)}"),
            ("{--1 for x in [1,2,3]}", "{1 for x in (1, 2, 3)}"),
            ("x in [1,2,3]", "x in (1, 2, 3)"),
            ("x in x in [1,2,3]", "x in x in (1, 2, 3)"),
            ("x in [1,2,3] in x", "x in [1, 2, 3] in x"),
        ]
        for inp, expected in cases:
            optimizer = AstOptimizer()
            tree = ast.parse(inp)
            # pyrefly: ignore [missing-attribute]
            optimized = to_expr(optimizer.visit(tree).body[0].value)
            self.assertEqual(expected, optimized, "Input was: " + inp)

    def test_ast_optimizer_for(self):
        optimizer = AstOptimizer()
        tree = ast.parse("for x in [1,2,3]: pass")
        # pyrefly: ignore [missing-attribute]
        optimized = optimizer.visit(tree).body[0]
        self.assertEqual(to_expr(optimized.iter), "(1, 2, 3)")

    def test_fold_nonconst_list_to_tuple_in_comparisons(self):
        optimizer = AstOptimizer()
        tree = ast.parse("[a for a in b if a.c in [e, f]]")
        optimized = optimizer.visit(tree)
        self.assertEqual(
            # pyrefly: ignore [missing-attribute]
            to_expr(optimized.body[0].value.generators[0].ifs[0].comparators[0]),
            "(e, f)",
        )

    def test_assert_statements(self):
        optimizer = AstOptimizer(optimize=True)
        non_optimizer = AstOptimizer(optimize=False)
        code = """def f(a, b): assert a == b, 'lol'"""
        tree = ast.parse(code)
        optimized = optimizer.visit(tree)
        # Function body should contain the assert
        # pyrefly: ignore [missing-attribute]
        self.assertIsInstance(optimized.body[0].body[0], ast.Assert)

        unoptimized = non_optimizer.visit(tree)
        # Function body should contain the assert
        # pyrefly: ignore [missing-attribute]
        self.assertIsInstance(unoptimized.body[0].body[0], ast.Assert)

    def test_folding_of_tuples_of_constants(self):
        bigtuple = tuple(range(10000))
        for line, elem in (
            ("a = 1,2,3", (1, 2, 3)),
            ('a = ("a","b","c")', ("a", "b", "c")),
            ("a,b,c = 1,2,3", (1, 2, 3)),
            ("a = (None, 1, None)", (None, 1, None)),
            ("a = ((1, 2), 3, 4)", ((1, 2), 3, 4)),
            ("a = " + repr(bigtuple), bigtuple),
        ):
            tree = ast.parse(line)
            # pyrefly: ignore [missing-attribute]
            self.assertIsInstance(tree.body[0].value, ast.Tuple)
            optimized = AstOptimizer(optimize=True).visit(tree)
            # pyrefly: ignore [missing-attribute]
            const = optimized.body[0].value
            self.assertIsInstance(const, ast.Constant)
            self.assertEqual(const.value, elem)

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_folding_of_lists_of_constants(self):
        for line, elem in (
            # in/not in constants with BUILD_LIST should be folded to a tuple:
            ("a in [1,2,3]", (1, 2, 3)),
            ('a not in ["a","b","c"]', ("a", "b", "c")),
            ("a in [None, 1, None]", (None, 1, None)),
        ):
            code = self.compare_graph(line)
            code.assert_both("LOAD_CONST", elem)

        for line, elem in (("a not in [(1, 2), 3, 4]", ((1, 2), 3, 4)),):
            code = self.compare_graph(line)
            code.assert_added("LOAD_CONST", elem)
            code.assert_removed("BUILD_LIST")

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_folding_of_sets_of_constants(self):
        for line, elem in (
            ("a in {1,2,3}", frozenset({1, 2, 3})),
            ('a not in {"a","b","c"}', frozenset({"a", "c", "b"})),
            ("a in {None, 1, None}", frozenset({1, None})),
            ("a in {1, 2, 3, 3, 2, 1}", frozenset({1, 2, 3})),
        ):
            code = self.compare_graph(line)
            # The LOAD_CONSTfrozenset optimization takes place in the compiler.
            code.assert_both("LOAD_CONST", elem)
        for line, elem in (("a not in {(1, 2), 3, 4}", frozenset({(1, 2), 3, 4})),):
            code = self.compare_graph(line)
            code.assert_added("LOAD_CONST", elem)
            code.assert_removed("BUILD_SET")

        # Ensure that the resulting code actually works:
        d = self.run_code(
            """
        def f(a):
            return a in {1, 2, 3}

        def g(a):
            return a not in {1, 2, 3}"""
        )
        f, g = d["f"], d["g"]

        self.assertTrue(f(3))
        self.assertTrue(not f(4))

        self.assertTrue(not g(3))
        self.assertTrue(g(4))

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_folding_of_binops_on_constants(self):
        for line, elem in (
            ("a = 2+3+4", 9),  # chained fold
            ('a = "@"*4', "@@@@"),  # check string ops
            ('a="abc" + "def"', "abcdef"),  # check string ops
            ("a = 3**4", 81),  # binary power
            ("a = 3*4", 12),  # binary multiply
            ("a = 13//4", 3),  # binary floor divide
            ("a = 14%4", 2),  # binary modulo
            ("a = 2+3", 5),  # binary add
            ("a = 13-4", 9),  # binary subtract
            # ('a = (12,13)[1]', 13),             # binary subscr
            ("a = 13 << 2", 52),  # binary lshift
            ("a = 13 >> 2", 3),  # binary rshift
            ("a = 13 & 7", 5),  # binary and
            ("a = 13 ^ 7", 10),  # binary xor
            ("a = 13 | 7", 15),  # binary or
            ("a = 2 ** -14", 6.103515625e-05),  # binary power neg rhs
        ):
            code = self.compare_graph(line)
            code.assert_added("LOAD_CONST", elem)
            code.assert_all_removed("BINARY_")

        # Verify that unfoldables are skipped
        code = self.compare_graph('a=2+"b"')
        code.assert_both("LOAD_CONST", 2)
        code.assert_both("LOAD_CONST", "b")

        # Verify that large sequences do not result from folding
        code = self.compare_graph('a="x"*10000')
        code.assert_both("LOAD_CONST", 10000)
        consts = code.opt.getConsts()
        self.assertNotIn("x" * 10000, consts)
        code = self.compare_graph("a=1<<1000")
        code.assert_both("LOAD_CONST", 1000)
        self.assertNotIn(1 << 1000, consts)
        code = self.compare_graph("a=2**1000")
        code.assert_both("LOAD_CONST", 1000)
        self.assertNotIn(2**1000, consts)

    @passIf(sys.version_info >= (3, 14), "AST optimizer does less on 3.14")
    def test_binary_subscr_on_unicode(self):
        # valid code get optimized
        code = self.compare_graph('x = "foo"[0]')
        code.assert_added("LOAD_CONST", "f")
        code.assert_removed("BINARY_SUBSCR")
        code = self.compare_graph('x = "\u0061\uffff"[1]')
        code.assert_added("LOAD_CONST", "\uffff")
        code.assert_removed("BINARY_SUBSCR")

        # With PEP 393, non-BMP char get optimized
        code = self.compare_graph('x = "\U00012345"[0]')
        code.assert_both("LOAD_CONST", "\U00012345")
        code.assert_removed("BINARY_SUBSCR")

        # invalid code doesn't get optimized
        # out of range
        code = self.compare_graph('x = "fuu"[10]')
        code.assert_both("BINARY_SUBSCR")

    def test_enum_format_str_components(self) -> None:
        test_cases = [
            ("%s", ["", FormatInfo("s")]),
            ("aa%s", ["aa", FormatInfo("s")]),
            ("%saa", ["", FormatInfo("s"), "aa"]),
            ("%%s", ["%s"]),
            ("%-s", ["", FormatInfo("s", flags=F_LJUST)]),
            ("%99s", ["", FormatInfo("s", width=99)]),
            # Unsupported format, too many digits
            ("%999s", None),
        ]

        for format, expected in test_cases:
            with self.subTest(format=format, expected=expected):
                try:
                    got = list(enum_format_str_components(format))
                except UnsupportedFormat:
                    self.assertEqual(expected, None)
                else:
                    self.assertEqual(got, expected)

    def assert_formatted_value(
        self, format_info: FormatInfo, val: ast.expr
    ) -> ast.FormattedValue:
        res = format_info.as_formatted_value(val)
        assert res is not None
        return res

    def assert_formatted_spec(
        self, format_info: FormatInfo, val: ast.expr
    ) -> ast.Constant:
        formatted_val = format_info.as_formatted_value(val)
        assert formatted_val is not None
        res = formatted_val.format_spec
        assert isinstance(res, ast.Constant)
        return res

    def test_format_info(self) -> None:
        val = ast.Constant(None)
        # unsupported spec
        self.assertEqual(FormatInfo("!").as_formatted_value(val), None)

        # conversion pass through
        self.assertEqual(
            self.assert_formatted_value(FormatInfo("s"), val).conversion, ord("s")
        )
        self.assertEqual(
            self.assert_formatted_value(FormatInfo("r"), val).conversion, ord("r")
        )
        self.assertEqual(
            self.assert_formatted_value(FormatInfo("a"), val).conversion, ord("a")
        )

        # width
        self.assertEqual(
            self.assert_formatted_spec(FormatInfo("s", width=10), val).value, ">10"
        )

        # zero-width (but no F_LJUST)
        self.assertEqual(
            self.assert_formatted_spec(FormatInfo("s", width=0), val).value, "0"
        )

        # precision
        self.assertEqual(
            self.assert_formatted_spec(FormatInfo("s", prec=10), val).value, ".10"
        )

        # width and precision
        self.assertEqual(
            self.assert_formatted_spec(FormatInfo("s", width=1, prec=2), val).value,
            ">1.2",
        )

        self.assertEqual(
            self.assert_formatted_spec(
                FormatInfo("s", flags=F_LJUST, width=10), val
            ).value,
            "10",
        )

    def test_empty_tuple_load_common_constant(self) -> None:
        # The empty tuple was added to the LOAD_COMMON_CONSTANT table in 3.16
        # (magic 3701); on earlier versions it stays a plain LOAD_CONST.
        graph = self.to_graph("x = ()")
        if sys.version_info >= (3, 16):
            self.assertInGraph(graph, "LOAD_COMMON_CONSTANT", CONSTANT_EMPTY_TUPLE)
            self.assertNotInGraph(graph, "LOAD_CONST", ())
        else:
            self.assertInGraph(graph, "LOAD_CONST", ())
            self.assertNotInGraph(graph, "LOAD_COMMON_CONSTANT", CONSTANT_EMPTY_TUPLE)

    def test_frozenset_call_optimization(self) -> None:
        # 3.16 (gh-150027) optimizes frozenset({...}) into a guarded
        # INTRINSIC_BUILD_FROZENSET build; earlier versions emit a plain call.
        graph = self.to_graph("x = frozenset({a, b})")
        if sys.version_info >= (3, 16):
            self.assertInGraph(graph, "CALL_INTRINSIC_1")
        else:
            self.assertNotInGraph(graph, "CALL_INTRINSIC_1")


class _FakeInstr:
    __slots__ = ("opname", "oparg", "ioparg")

    def __init__(self, opname: str, oparg: object) -> None:
        self.opname = opname
        self.oparg = oparg
        self.ioparg = 0


class _FakeBlock:
    __slots__ = ("insts",)

    def __init__(self, insts: list[_FakeInstr]) -> None:
        self.insts = insts


class LoadCommonConstantTest(CompilerTest):
    """Version-independent tests for the LOAD_COMMON_CONSTANT mapping.

    These exercise the optimizer logic directly so they run under any
    interpreter, without needing a 3.16 runtime.
    """

    def test_common_constant_oparg_shared_values(self) -> None:
        # The 3.15 set is unaffected by the empty-tuple flag.
        for allow in (False, True):
            self.assertEqual(
                _common_constant_oparg(None, allow_empty_tuple=allow), CONSTANT_NONE
            )
            self.assertEqual(
                _common_constant_oparg(True, allow_empty_tuple=allow), CONSTANT_TRUE
            )
            self.assertEqual(
                _common_constant_oparg(False, allow_empty_tuple=allow), CONSTANT_FALSE
            )
            self.assertEqual(
                _common_constant_oparg("", allow_empty_tuple=allow), CONSTANT_EMPTY_STR
            )
            self.assertEqual(
                _common_constant_oparg(-1, allow_empty_tuple=allow), CONSTANT_MINUS_ONE
            )

    def test_common_constant_oparg_empty_tuple_gated(self) -> None:
        # Empty tuple only maps when explicitly allowed (3.16+).
        self.assertIsNone(_common_constant_oparg(()))
        self.assertIsNone(_common_constant_oparg((), allow_empty_tuple=False))
        self.assertEqual(
            _common_constant_oparg((), allow_empty_tuple=True), CONSTANT_EMPTY_TUPLE
        )
        # Non-empty tuples are never common constants.
        self.assertIsNone(_common_constant_oparg((1,), allow_empty_tuple=True))

    def test_convert_pass_respects_flag(self) -> None:
        def make_blocks() -> list[_FakeBlock]:
            return [_FakeBlock([_FakeInstr("LOAD_CONST", ())])]

        # Default (3.15 behavior): empty tuple left as LOAD_CONST.
        blocks = make_blocks()
        convert_load_const_to_load_common_constant(blocks)
        self.assertEqual(blocks[0].insts[0].opname, "LOAD_CONST")

        # 3.16 behavior: rewritten to LOAD_COMMON_CONSTANT with the right oparg.
        blocks = make_blocks()
        convert_load_const_to_load_common_constant(blocks, allow_empty_tuple=True)
        instr = blocks[0].insts[0]
        self.assertEqual(instr.opname, "LOAD_COMMON_CONSTANT")
        self.assertEqual(instr.oparg, CONSTANT_EMPTY_TUPLE)
        self.assertEqual(instr.ioparg, CONSTANT_EMPTY_TUPLE)

    def test_const_optimizer_flag_wiring(self) -> None:
        self.assertFalse(FlowGraphConstOptimizer315._allow_empty_tuple_const)
        self.assertTrue(FlowGraphConstOptimizer316._allow_empty_tuple_const)
