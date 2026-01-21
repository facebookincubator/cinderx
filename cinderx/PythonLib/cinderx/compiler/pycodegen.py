# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict


# @lint-ignore-ide BLACK

from __future__ import annotations

try:
    # Import our dependencies eagerly in case we are imported before the static
    # source loader is installed.
    import ast
    import importlib.util
    import itertools
    import marshal
    import opcode
    import os
    import sys
    from ast import AST, ClassDef
    from builtins import compile as builtin_compile
    from contextlib import contextmanager
    from enum import IntEnum
    from types import CodeType
    from typing import Callable, cast, Generator, Protocol, Sequence, Union

    from .consts import (
        CO_ASYNC_GENERATOR,
        CO_COROUTINE,
        CO_FUTURE_ANNOTATIONS,
        CO_FUTURE_BARRY_AS_BDFL,
        CO_GENERATOR,
        CO_HAS_DOCSTRING,
        CO_METHOD,
        CO_NESTED,
        CO_VARARGS,
        CO_VARKEYWORDS,
        PyCF_COMPILE_MASK,
        PyCF_MASK,
        PyCF_MASK_OBSOLETE,
        PyCF_ONLY_AST,
        PyCF_SOURCE_IS_UTF8,
        SC_CELL,
        SC_FREE,
        SC_GLOBAL_EXPLICIT,
        SC_GLOBAL_IMPLICIT,
        SC_LOCAL,
        SC_UNKNOWN,
    )
    from .future import find_futures as future_find_futures
    from .misc import mangle

    # pyre-fixme[21]: Could not find name `INTRINSIC_1` in `cinderx.compiler.opcodes`.
    # pyre-fixme[21]: Could not find name `INTRINSIC_2` in `cinderx.compiler.opcodes`.
    from .opcodes import find_op_idx, INTRINSIC_1, INTRINSIC_2
    from .optimizer import AstOptimizer, AstOptimizer312, AstOptimizer314
    from .pyassem import (
        Block,
        FVC_ASCII,
        FVC_NONE,
        FVC_REPR,
        FVC_STR,
        FVS_HAVE_SPEC,
        Instruction,
        NEXT_LOCATION,
        NO_LOCATION,
        PyFlowGraph,
        PyFlowGraph310,
        PyFlowGraph312,
        PyFlowGraph314,
        PyFlowGraph315,
        PyFlowGraphCinder310,
        PyFlowGraphCinder312,
        ResumeOparg,
        SrcLocation,
    )
    from .symbols import (
        Annotations,
        AnnotationScope,
        BaseSymbolVisitor,
        CinderSymbolVisitor,
        ClassScope,
        FunctionScope,
        GenExprScope,
        ModuleScope,
        Scope,
        SymbolVisitor310,
        SymbolVisitor312,
        SymbolVisitor314,
        SymbolVisitor315,
        TypeParams,
        TypeParamScope,
        TypeVarDefault,
    )
    from .unparse import to_expr
    from .visitor import ASTVisitor
except Exception:
    raise

try:
    from cinder import _set_qualname as cx_set_qualname
except ImportError:

    def cx_set_qualname(code: CodeType, qualname: str | None) -> None:
        pass


IS_3_12_8: bool = sys.version_info[:3] >= (3, 12, 8)

callfunc_opcode_info = {
    # (Have *args, Have **args) : opcode
    (0, 0): "CALL_FUNCTION",
    (1, 0): "CALL_FUNCTION_VAR",
    (0, 1): "CALL_FUNCTION_KW",
    (1, 1): "CALL_FUNCTION_VAR_KW",
}

INT_MAX: int = (2**31) - 1

# enum fblocktype
WHILE_LOOP = 1
FOR_LOOP = 2
TRY_EXCEPT = 3
FINALLY_TRY = 4
FINALLY_END = 5
WITH = 6
ASYNC_WITH = 7
HANDLER_CLEANUP = 8
POP_VALUE = 9
EXCEPTION_HANDLER = 10
EXCEPTION_GROUP_HANDLER = 11
ASYNC_COMPREHENSION_GENERATOR = 12
STOP_ITERATION = 13
ASYNC_FOR_LOOP = 14

_ZERO: bytes = (0).to_bytes(4, "little")

_DEFAULT_MODNAME: str = sys.intern("<module>")


MAKE_FUNCTION_DEFAULTS: int = 0x01
MAKE_FUNCTION_KWDEFAULTS: int = 0x02
MAKE_FUNCTION_ANNOTATIONS: int = 0x04
MAKE_FUNCTION_CLOSURE: int = 0x08
MAKE_FUNCTION_ANNOTATE: int = 0x10


FuncOrLambda = Union[ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda]
CompNode = Union[ast.GeneratorExp, ast.SetComp, ast.DictComp, ast.ListComp]
if sys.version_info >= (3, 12):
    CodeGenTree = Union[
        FuncOrLambda,
        CompNode,
        ast.ClassDef,
        TypeParams,
        Annotations,
        ast.arguments,
        ast.TypeAlias,
    ]
else:
    CodeGenTree = Union[
        FuncOrLambda, CompNode, ast.ClassDef, TypeParams, Annotations, ast.arguments
    ]


# A soft limit for stack use, to avoid excessive
# memory use for large constants, etc.
#
# The value 30 is plucked out of thin air.
# Code that could use more stack than this is
# rare, so the exact value is unimportant.
STACK_USE_GUIDELINE = 30


class AwaitableKind(IntEnum):
    Default = 0
    AsyncEnter = 1
    AsyncExit = 2


class IterStackState(IntEnum):
    IterableInLocal = 0
    IterableOnStack = 1
    IteratorOnStack = 2


def make_header(mtime: int, size: int) -> bytes:
    return _ZERO + mtime.to_bytes(4, "little") + size.to_bytes(4, "little")


def compileFile(
    filename: str,
    display: int = 0,
    compiler: type[CodeGenerator] | None = None,
    modname: str = _DEFAULT_MODNAME,
) -> None:
    # compile.c uses marshal to write a long directly, with
    # calling the interface that would also generate a 1-byte code
    # to indicate the type of the value.  simplest way to get the
    # same effect is to call marshal and then skip the code.
    fileinfo = os.stat(filename)

    with open(filename) as f:
        buf = f.read()
    code = compile_code(buf, filename, "exec", compiler=compiler, modname=modname)
    with open(filename + "c", "wb") as f:
        hdr = make_header(int(fileinfo.st_mtime), fileinfo.st_size)
        f.write(importlib.util.MAGIC_NUMBER)
        f.write(hdr)
        marshal.dump(code, f)


def compile(
    source: str | bytes | ast.Module | ast.Expression | ast.Interactive,
    filename: str,
    mode: str,
    flags: int = 0,
    dont_inherit: bool | None = None,
    optimize: int = -1,
    compiler: type[CodeGenerator] | None = None,
    modname: str = _DEFAULT_MODNAME,
) -> CodeType | CodeGenerator | ast.Module | ast.Expression | ast.Interactive:
    """Replacement for builtin compile() function

    Does not yet support ast.PyCF_ALLOW_TOP_LEVEL_AWAIT flag.
    """
    if dont_inherit is not None:
        raise RuntimeError("not implemented yet")

    if flags & PyCF_ONLY_AST:
        return parse(source, filename, mode, flags & PyCF_MASK)

    result = make_compiler(source, filename, mode, flags, optimize, compiler, modname)
    assert isinstance(result, CodeGenerator)
    return result.getCode()


def compile_code(
    source: str | bytes | ast.Module | ast.Expression | ast.Interactive,
    filename: str,
    mode: str,
    flags: int = 0,
    dont_inherit: bool | None = None,
    optimize: int = -1,
    compiler: type[CodeGenerator] | None = None,
    modname: str = _DEFAULT_MODNAME,
) -> CodeType:
    """Replacement for builtin compile() function

    Does not support the PyCF_ONLY_AST flag, this always returns code objects.

    Does not yet support ast.PyCF_ALLOW_TOP_LEVEL_AWAIT flag.
    """
    if flags & PyCF_ONLY_AST:
        raise ValueError(
            "compile_code() called with unsupported flag PyCF_ONLY_AST, use compile()"
        )

    result = compile(
        source=source,
        filename=filename,
        mode=mode,
        flags=flags,
        dont_inherit=dont_inherit,
        optimize=optimize,
        compiler=compiler,
        modname=modname,
    )
    assert isinstance(result, CodeType)
    return result


def parse(
    source: str | bytes | ast.Module | ast.Expression | ast.Interactive,
    filename: str,
    mode: str,
    flags: int,
) -> CodeType:
    return builtin_compile(source, filename, mode, flags | PyCF_ONLY_AST)


def make_compiler(
    source: str | bytes | ast.Module | ast.Expression | ast.Interactive,
    filename: str,
    mode: str,
    flags: int = 0,
    optimize: int = -1,
    generator: type[CodeGenerator] | None = None,
    modname: str = _DEFAULT_MODNAME,
    ast_optimizer_enabled: bool = True,
) -> CodeGenerator:
    if mode not in ("single", "exec", "eval"):
        raise ValueError("compile() mode must be 'exec', 'eval' or 'single'")
    if flags & PyCF_ONLY_AST:
        raise ValueError("make_compiler() does not support PyCF_ONLY_AST, use parse()")

    if generator is None:
        generator = get_default_generator()

    if flags & ~(PyCF_MASK | PyCF_MASK_OBSOLETE | PyCF_COMPILE_MASK):
        raise ValueError("compile(): unrecognised flags", hex(flags))

    flags |= PyCF_SOURCE_IS_UTF8

    if isinstance(source, ast.AST):
        tree = source
    else:
        tree = parse(source, filename, mode, flags & PyCF_MASK)

    assert isinstance(tree, ast.AST)
    optimize = sys.flags.optimize if optimize == -1 else optimize

    return generator.make_code_gen(
        modname,
        tree,
        filename,
        source,
        flags,
        optimize,
        ast_optimizer_enabled=ast_optimizer_enabled,
    )


def is_const(node: ast.expr | None) -> bool:
    return isinstance(node, ast.Constant)


def all_items_const(seq: list[ast.expr | None], begin: int, end: int) -> bool:
    for item in seq[begin:end]:
        if not is_const(item):
            return False
    return True


def find_futures(flags: int, node: ast.AST) -> int:
    future_flags = flags & PyCF_MASK
    for feature in future_find_futures(node):
        if feature == "barry_as_FLUFL":
            future_flags |= CO_FUTURE_BARRY_AS_BDFL
        elif feature == "annotations":
            future_flags |= CO_FUTURE_ANNOTATIONS
    return future_flags


CONV_STR: int = ord("s")
CONV_REPR: int = ord("r")
CONV_ASCII: int = ord("a")


class PatternContext:
    def __init__(self) -> None:
        self.stores: list[str] = []
        self.allow_irrefutable: bool = False
        self.fail_pop: list[Block] = []
        self.on_top: int = 0

    def clone(self) -> PatternContext:
        pc = PatternContext()
        pc.stores = list(self.stores)
        pc.allow_irrefutable = self.allow_irrefutable
        pc.fail_pop = list(self.fail_pop)
        pc.on_top = self.on_top
        return pc


class CodeGenerator(ASTVisitor):
    """Defines basic code generator for Python bytecode

    This class is an abstract base class.  Concrete subclasses must
    define an __init__() that defines self.graph and then calls the
    __init__() defined in this class.
    """

    optimized = 0  # is namespace access optimized?
    class_name: str | None = None  # provide default for instance variable
    future_flags = 0
    flow_graph: type[PyFlowGraph] = PyFlowGraph310
    _SymbolVisitor: type[BaseSymbolVisitor] = BaseSymbolVisitor
    pattern_context: type[PatternContext] = PatternContext

    # pyre-fixme[4] This appears to be unused.
    __initialized = None

    def __init__(
        self,
        parent: CodeGenerator | None,
        node: AST,
        symbols: BaseSymbolVisitor,
        graph: PyFlowGraph,
        flags: int = 0,
        optimization_lvl: int = 0,
        future_flags: int | None = None,
        name: str | None = None,
    ) -> None:
        super().__init__()
        if parent is not None:
            assert future_flags is None, "Child codegen should inherit future flags"
            future_flags = parent.future_flags
        self.future_flags = future_flags or 0
        graph.setFlag(self.future_flags)
        self.module_gen: CodeGenerator = self if parent is None else parent.module_gen
        self._tree = node
        self.symbols = symbols
        self.graph = graph
        self.scopes: dict[ast.AST, Scope] = symbols.scopes
        self.setups: list[Entry] = []
        self._setupGraphDelegation()
        self.interactive = False
        self.scope: Scope = self.scopes[node]
        self.flags = flags
        self.optimization_lvl = optimization_lvl
        self.strip_docstrings: bool = optimization_lvl == 2
        self.did_setup_annotations = False
        self._qual_name: str | None = None
        self.parent_code_gen = parent
        self.name: str = self.get_node_name(node) if name is None else name
        self.in_conditional_block = 0

        # pyre-fixme[4] This appears to be unused.
        self.last_lineno = None

    @property
    def tree(self) -> AST:
        tree = self._tree
        if isinstance(tree, AST):
            return tree

        return tree[0]

    def _setupGraphDelegation(self) -> None:
        self.emit = self.graph.emit
        self.emitWithBlock = self.graph.emitWithBlock
        self.emit_noline = self.graph.emit_noline
        self.newBlock = self.graph.newBlock
        self.nextBlock = self.graph.nextBlock
        self.emit_jump_forward = self.graph.emit_jump_forward
        self.emit_jump_forward_noline = self.graph.emit_jump_forward_noline

    def getCode(self) -> CodeType:
        """Return a code object"""
        return self.graph.getCode()

    def set_qual_name(self, qualname: str) -> None:
        pass

    def finish_type_params(
        self, has_args: bool, num_typeparam_args: int, need_swap: bool = False
    ) -> None:
        raise NotImplementedError()

    @contextmanager
    def noEmit(self) -> Generator[None, None, None]:
        self.graph.do_not_emit_bytecode += 1
        try:
            yield
        finally:
            self.graph.do_not_emit_bytecode -= 1

    @contextmanager
    def conditional_block(self) -> Generator[None, None, None]:
        self.in_conditional_block += 1
        try:
            yield
        finally:
            self.in_conditional_block -= 1

    def mangle(self, name: str) -> str:
        if self.class_name is not None:
            return mangle(name, self.class_name)
        else:
            return name

    def check_name(self, name: str) -> int:
        return self.scope.check_name(name)

    def get_module(self) -> None:
        raise RuntimeError("should be implemented by subclasses")

    # Next five methods handle name access

    def _nameOp(self, prefix: str, name: str) -> None:
        raise NotImplementedError()

    def storeName(self, name: str) -> None:
        self._nameOp("STORE", name)

    def loadName(self, name: str) -> None:
        self._nameOp("LOAD", name)

    def delName(self, name: str) -> None:
        self._nameOp("DELETE", name)

    def _implicitNameOp(self, prefix: str, name: str) -> None:
        """Emit name ops for names generated implicitly by for loops

        The interpreter generates names that start with a period or
        dollar sign.  The symbol table ignores these names because
        they aren't present in the program text.
        """
        if self.optimized:
            self.emit(prefix + "_FAST", name)
        else:
            self.emit(prefix + "_NAME", name)

    def set_pos(self, node: AST | SrcLocation) -> None:
        self.graph.set_pos(node)

    def set_no_pos(self) -> None:
        """Mark following instructions as synthetic (no source line number)."""
        self.graph.set_pos(NO_LOCATION)

    @contextmanager
    def temp_lineno(self, lineno: int) -> Generator[None, None, None]:
        old_loc = self.graph.loc
        self.graph.set_pos(SrcLocation(lineno, lineno, -1, -1))
        try:
            yield
        finally:
            self.graph.set_pos(old_loc)

    def skip_docstring(self, body: list[ast.stmt]) -> list[ast.stmt]:
        """Given list of statements, representing body of a function, class,
        or module, skip docstring, if any.
        """
        if body and isinstance(body, list):
            expr = body[0]

            if isinstance(expr, ast.Expr):
                value = expr.value

                if isinstance(value, ast.Constant) and isinstance(value.value, str):
                    return body[1:]

        return body

    # The first few visitor methods handle nodes that generator new
    # code objects.  They use class attributes to determine what
    # specialized code generators to use.

    def visitInteractive(self, node: ast.Interactive) -> None:
        self.interactive = True
        self.visitStatements(node.body)
        self.emit("LOAD_CONST", None)
        self.emit("RETURN_VALUE")

    def visitModule(self, node: ast.Module) -> None:
        # Set current line number to the line number of first statement.
        # This way line number for SETUP_ANNOTATIONS will always
        # coincide with the line number of first "real" statement in module.
        # If body is empy, then lineno will be set later in assemble.
        if node.body:
            self.set_pos(node.body[0])

        self.setup_module_annotations(node)
        doc = self.get_docstring(node)
        if doc is not None:
            self.emit("LOAD_CONST", doc)
            self.storeName("__doc__")
        self.startModule()
        self.visitStatements(self.skip_docstring(node.body))

        # See if the was a live statement, to later set its line number as
        # module first line. If not, fall back to first line of 1.
        if not self.graph.first_inst_lineno:
            self.graph.first_inst_lineno = 1

        self.emit_module_return(node)

    def setup_module_annotations(self, node: ast.Module) -> None:
        if self.findAnn(node.body):
            self.emit("SETUP_ANNOTATIONS")
            self.did_setup_annotations = True

    def startModule(self) -> None:
        pass

    def emit_module_return(self, node: ast.Module) -> None:
        self.set_no_pos()
        self.emit("LOAD_CONST", None)
        self.emit("RETURN_VALUE")

    def visitExpression(self, node: ast.Expression) -> None:
        self.visit(node.body)
        self.emit("RETURN_VALUE")

    def visitFunctionDef(self, node: ast.FunctionDef) -> None:
        self.visitFunctionOrLambda(node)

    def visitAsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        self.visitFunctionOrLambda(node)

    def visitLambda(self, node: ast.Lambda) -> None:
        self.visitFunctionOrLambda(node)

    def visitJoinedStr(self, node: ast.JoinedStr) -> None:
        raise NotImplementedError()

    def visitFormattedValue(self, node: ast.FormattedValue) -> None:
        self.visit(node.value)

        if node.conversion == CONV_STR:
            oparg = FVC_STR
        elif node.conversion == CONV_REPR:
            oparg = FVC_REPR
        elif node.conversion == CONV_ASCII:
            oparg = FVC_ASCII
        else:
            assert node.conversion == -1, str(node.conversion)
            oparg = FVC_NONE

        if node.format_spec:
            self.visit(node.format_spec)
            oparg |= FVS_HAVE_SPEC
        self.emit("FORMAT_VALUE", oparg)

    def processBody(
        self, node: FuncOrLambda, body: list[ast.stmt], gen: CodeGenerator
    ) -> None:
        if isinstance(body, list):
            for stmt in body:
                gen.visit(stmt)
        else:
            gen.visit(body)

    def build_annotations(self, node: ast.FunctionDef | ast.AsyncFunctionDef) -> int:
        annotation_count = self.annotate_args(node)
        # Cannot annotate return type for lambda
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            returns = node.returns
            if returns:
                self.set_pos(node)
                self.emit("LOAD_CONST", "return")
                self._visitAnnotation(returns)
                annotation_count += 2

        if annotation_count > 0:
            self.set_pos(node)
            self.emit("BUILD_TUPLE", annotation_count)
            return MAKE_FUNCTION_ANNOTATIONS

        return 0

    def emit_function_decorators(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef
    ) -> None:
        raise NotImplementedError()

    def visitFunctionOrLambda(self, node: FuncOrLambda) -> None:
        if isinstance(node, ast.Lambda):
            name = sys.intern("<lambda>")
            ndecorators, first_lineno = 0, node.lineno
        else:
            name = node.name
            if node.decorator_list:
                for decorator in node.decorator_list:
                    self.visit(decorator)
                ndecorators = len(node.decorator_list)
                first_lineno = node.decorator_list[0].lineno
            else:
                ndecorators = 0
                first_lineno = node.lineno

        gen = self.generate_function(node, name, first_lineno)

        self.build_function(node, gen, first_lineno)

        if ndecorators:
            assert isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            self.emit_function_decorators(node)

        if not isinstance(node, ast.Lambda):
            self.storeName(gen.graph.name)

    def visitDefault(self, node: ast.expr) -> None:
        self.visit(node)

    def annotate_args(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef | ast.Lambda
    ) -> int:
        args = node.args
        annotation_count = 0
        for arg in args.args:
            annotation_count += self.annotate_arg(node, arg)
        for arg in args.posonlyargs:
            annotation_count += self.annotate_arg(node, arg)
        if arg := args.vararg:
            annotation_count += self.annotate_arg(node, arg)
        for arg in args.kwonlyargs:
            annotation_count += self.annotate_arg(node, arg)
        if arg := args.kwarg:
            annotation_count += self.annotate_arg(node, arg)
        return annotation_count

    def annotate_arg(self, loc: AST, arg: ast.arg) -> int:
        ann = arg.annotation
        if ann:
            self.set_pos(loc)
            self.emit("LOAD_CONST", self.mangle(arg.arg))
            self._visitAnnotation(ann)
            return 2
        return 0

    def find_immutability_flag(self, node: ClassDef) -> bool:
        return False

    def visit_decorator(self, decorator: AST, class_def: ClassDef) -> None:
        self.visit(decorator)

    def emit_decorator_call(self, decorator: AST, class_def: ClassDef) -> None:
        # Overridden by the static compiler
        self.emit_call_one_arg()

    def register_immutability(self, node: ClassDef, flag: bool) -> None:
        """
        Note: make sure this do not have side effect on the
        stack, assumes class is on the stack
        """

    def post_process_and_store_name(self, node: ClassDef) -> None:
        self.storeName(node.name)

    def walkClassBody(self, node: ClassDef, gen: CodeGenerator) -> None:
        gen.visit_list(self.skip_docstring(node.body))

    # The rest are standard visitor methods

    # The next few implement control-flow statements

    def visitIf(self, node: ast.If) -> None:
        end = self.newBlock("if_end")
        orelse = None
        if node.orelse:
            orelse = self.newBlock("if_else")

        self.compileJumpIf(node.test, orelse or end, False)
        self.visitStatements(node.body)

        if node.orelse:
            self.emit_jump_forward_noline(end)
            self.nextBlock(orelse)
            self.visitStatements(node.orelse)

        self.nextBlock(end)

    def emit_cond_jump(self, jump: str, label: Block) -> None:
        self.emit(jump, label)

    def visitWhile(self, node: ast.While) -> None:
        loop = self.newBlock("while_loop")
        body = self.newBlock("while_body")
        else_ = self.newBlock("while_else")
        after = self.newBlock("while_after")

        self.push_loop(WHILE_LOOP, loop, after)

        self.nextBlock(loop)
        self.compileJumpIf(node.test, else_, False)

        self.nextBlock(body)
        self.visitStatements(node.body)
        self.set_pos(node)
        self.compileJumpIf(node.test, body, True)

        self.pop_loop()
        self.nextBlock(else_)
        if node.orelse:
            self.visitStatements(node.orelse)

        self.nextBlock(after)

    def push_fblock(self, entry: Entry) -> None:
        self.setups.append(entry)

    def pop_fblock(self, kind: int | None = None) -> Entry:
        assert kind is None or self.setups[-1].kind == kind, (
            f"{self.setups[-1].kind} vs {kind} expected"
        )
        return self.setups.pop()

    def push_loop(self, kind: int, start: Block, end: Block) -> None:
        self.push_fblock(Entry(kind, start, end, None))

    def pop_loop(self) -> None:
        self.pop_fblock()

    def visitAsyncFor(self, node: ast.AsyncFor) -> None:
        start = self.newBlock("async_for_try")
        except_ = self.newBlock("except")
        end = self.newBlock("end")

        self.visit(node.iter)
        if IS_3_12_8:
            self.graph.emit_with_loc("GET_AITER", 0, node.iter)
        else:
            self.emit("GET_AITER")

        self.nextBlock(start)

        self.push_loop(FOR_LOOP, start, end)
        self.emit("SETUP_FINALLY", except_)
        self.emit("GET_ANEXT")
        self.emit("LOAD_CONST", None)
        self.emit_yield_from(await_=True)
        self.emit("POP_BLOCK")
        self.visit(node.target)
        self.visitStatements(node.body)
        self.set_no_pos()
        self.emitJump(start)
        self.pop_fblock(FOR_LOOP)

        self.nextBlock(except_)
        self.set_pos(node.iter)
        self.emit("END_ASYNC_FOR")
        if node.orelse:
            self.visitStatements(node.orelse)
        self.nextBlock(end)

    def visitBreak(self, node: ast.Break) -> None:
        self.emit("NOP")  # for line number
        loop = self.unwind_setup_entries(preserve_tos=False, stop_on_loop=True)
        if loop is None:
            raise self.syntax_error("'break' outside loop", node)
        self.unwind_setup_entry(loop, preserve_tos=False)

        jumpblock = loop.exit
        assert jumpblock is not None

        self.emitJump(jumpblock)
        self.nextBlock()

    def visitContinue(self, node: ast.Continue) -> None:
        self.emit("NOP")  # for line number
        loop = self.unwind_setup_entries(preserve_tos=False, stop_on_loop=True)
        if loop is None:
            raise self.syntax_error("'continue' not properly in loop", node)

        jumpblock = loop.block
        assert jumpblock is not None

        self.emitJump(jumpblock)
        self.nextBlock()

    # pyre-fixme[2] The node accepted here is called from a lot of places, but
    # not every node type has lineno and col_offset defined on it. We need to
    # narrow the type from ast.AST.
    def syntax_error(self, msg: str, node) -> SyntaxError:
        import linecache

        source_line = linecache.getline(self.graph.filename, node.lineno)
        return SyntaxError(
            msg,
            (self.graph.filename, node.lineno, node.col_offset, source_line or None),
        )

    def compileJumpIfPop(self, test: ast.expr, label: Block, is_if_true: bool) -> None:
        self.visit(test)
        self.emit_cond_jump(
            "JUMP_IF_TRUE_OR_POP" if is_if_true else "JUMP_IF_FALSE_OR_POP", label
        )

    def emit_test(self, node: ast.BoolOp, is_if_true: bool) -> None:
        end = self.newBlock()
        for child in node.values[:-1]:
            self.compileJumpIfPop(child, end, is_if_true)
            self.nextBlock()
        self.visit(node.values[-1])
        self.nextBlock(end)

    def visitBoolOp(self, node: ast.BoolOp) -> None:
        raise NotImplementedError()

    def emit_binary_subscr(self) -> None:
        raise NotImplementedError()

    def emit_bin_op(self, binop: type[ast.operator]) -> None:
        raise NotImplementedError()

    def visitBinOp(self, node: ast.BinOp) -> None:
        self.visit(node.left)
        self.visit(node.right)
        self.emit_bin_op(type(node.op))

    _cmp_opcode: dict[type[ast.cmpop], str] = {
        ast.Eq: "==",
        ast.NotEq: "!=",
        ast.Lt: "<",
        ast.LtE: "<=",
        ast.Gt: ">",
        ast.GtE: ">=",
    }

    def emit_compile_jump_if_compare(
        self, test: ast.Compare, next: Block, is_if_true: bool
    ) -> None:
        raise NotImplementedError()

    def emit_finish_jump_if(
        self, test: ast.expr, next: Block, is_if_true: bool
    ) -> None:
        raise NotImplementedError()

    def compileJumpIf(self, test: ast.expr, next: Block, is_if_true: bool) -> None:
        if isinstance(test, ast.UnaryOp):
            if isinstance(test.op, ast.Not):
                # Compile to remove not operation
                self.compileJumpIf(test.operand, next, not is_if_true)
                return
        elif isinstance(test, ast.BoolOp):
            is_or = isinstance(test.op, ast.Or)
            skip_jump = next
            if is_if_true != is_or:
                skip_jump = self.newBlock()

            for node in test.values[:-1]:
                self.compileJumpIf(node, skip_jump, is_or)

            self.compileJumpIf(test.values[-1], next, is_if_true)

            if skip_jump is not next:
                self.nextBlock(skip_jump)
            return
        elif isinstance(test, ast.IfExp):
            end = self.newBlock("end")
            orelse = self.newBlock("orelse")
            # Jump directly to orelse if test matches
            self.compileJumpIf(test.test, orelse, False)
            # Jump directly to target if test is true and body is matches
            self.compileJumpIf(test.body, next, is_if_true)
            self.emit_jump_forward_noline(end)
            # Jump directly to target if test is true and orelse matches
            self.nextBlock(orelse)
            self.compileJumpIf(test.orelse, next, is_if_true)
            self.nextBlock(end)
            return
        elif isinstance(test, ast.Compare):
            if len(test.ops) > 1:
                self.emit_compile_jump_if_compare(test, next, is_if_true)
                return

        self.emit_finish_jump_if(test, next, is_if_true)
        self.nextBlock()

    def visitIfExp(self, node: ast.IfExp) -> None:
        endblock = self.newBlock()
        elseblock = self.newBlock()
        self.compileJumpIf(node.test, elseblock, False)
        self.visit(node.body)
        self.emit_jump_forward_noline(endblock)
        self.nextBlock(elseblock)
        self.visit(node.orelse)
        self.nextBlock(endblock)

    def emitChainedCompareStep(
        self, op: ast.cmpop, value: ast.expr, cleanup: Block, always_pop: bool = False
    ) -> None:
        raise NotImplementedError()

    def defaultEmitCompare(self, op: ast.cmpop) -> None:
        if isinstance(op, ast.Is):
            self.emit("IS_OP", 0)
        elif isinstance(op, ast.IsNot):
            self.emit("IS_OP", 1)
        elif isinstance(op, ast.In):
            self.emit("CONTAINS_OP", 0)
        elif isinstance(op, ast.NotIn):
            self.emit("CONTAINS_OP", 1)
        else:
            self.emit("COMPARE_OP", self._cmp_opcode[type(op)])

    def visitCompare(self, node: ast.Compare) -> None:
        raise NotImplementedError()

    def visitDelete(self, node: ast.Delete) -> None:
        self.visit_list(node.targets)

    def conjure_arguments(self, args: list[ast.arg]) -> ast.arguments:
        return ast.arguments([], args, None, [], [], None, [])

    def emit_get_awaitable(self, kind: AwaitableKind) -> None:
        raise NotImplementedError()

    def visitGeneratorExp(self, node: ast.GeneratorExp) -> None:
        self.compile_comprehension(node, sys.intern("<genexpr>"), node.elt, None, None)

    def visitListComp(self, node: ast.ListComp) -> None:
        self.compile_comprehension(
            node, sys.intern("<listcomp>"), node.elt, None, "BUILD_LIST"
        )

    def visitSetComp(self, node: ast.SetComp) -> None:
        self.compile_comprehension(
            node, sys.intern("<setcomp>"), node.elt, None, "BUILD_SET"
        )

    def visitDictComp(self, node: ast.DictComp) -> None:
        self.compile_comprehension(
            node, sys.intern("<dictcomp>"), node.key, node.value, "BUILD_MAP"
        )

    # compile_comprehension helper functions

    def make_comprehension_codegen(self, node: CompNode, name: str) -> CodeGenerator:
        args = self.conjure_arguments([ast.arg(".0", None)])
        return self.make_func_codegen(node, args, name, node.lineno)

    def check_async_comprehension(self, node: CompNode) -> None:
        # TASK(T209725064): also add check for PyCF_ALLOW_TOP_LEVEL_AWAIT
        is_async_generator = self.symbols.scopes[node].coroutine
        is_async_function = self.scope.coroutine
        if (
            is_async_generator
            and not isinstance(node, ast.GeneratorExp)
            and not is_async_function
            and not isinstance(self.scope, GenExprScope)
        ):
            raise self.syntax_error(
                "asynchronous comprehension outside of an asynchronous function", node
            )

    def finish_comprehension(self, gen: CodeGenerator, node: CompNode) -> None:
        self.emit_closure(gen, 0)

        # precomputation of outmost iterable
        self.compile_comprehension_iter(node.generators[0])
        self.emit_call_one_arg()

        if gen.scope.coroutine and type(node) is not ast.GeneratorExp:
            self.emit_get_awaitable(AwaitableKind.Default)
            self.emit("LOAD_CONST", None)
            self.emit_yield_from(await_=True)

    def compile_comprehension_iter(self, gen: ast.comprehension) -> None:
        self.visit(gen.iter)
        if IS_3_12_8:
            self.graph.emit_with_loc(
                "GET_AITER" if gen.is_async else "GET_ITER", 0, gen.iter
            )
        else:
            self.emit("GET_AITER" if gen.is_async else "GET_ITER")

    def compile_dictcomp_element(self, elt: ast.expr, val: ast.expr) -> None:
        self.visit(elt)
        self.visit(val)

    # exception related

    def visitRaise(self, node: ast.Raise) -> None:
        n = 0
        if node.exc:
            self.visit(node.exc)
            n = n + 1
        if node.cause:
            self.visit(node.cause)
            n = n + 1
        self.emit("RAISE_VARARGS", n)
        self.nextBlock()

    def emit_except_local(self, handler: ast.ExceptHandler) -> None:
        target = handler.name
        type_ = handler.type
        if target:
            if type_:
                self.set_pos(type_)
            self.storeName(target)
        else:
            self.emit("POP_TOP")
        self.emit("POP_TOP")

    def visitTry(self, node: ast.Try) -> None:
        if node.finalbody:
            self.emit_try_finally(node)
        else:
            self.emit_try_except(node)

    def set_with_position_for_exit(
        self, node: ast.With | ast.AsyncWith, kind: int
    ) -> None:
        raise NotImplementedError()

    def emit_setup_with(self, node: ast.withitem, target: Block, async_: bool) -> None:
        raise NotImplementedError()

    def emit_with_except_cleanup(self, cleanup: Block) -> None:
        raise NotImplementedError()

    def emit_with_except_finish(self, cleanup: Block) -> None:
        raise NotImplementedError()

    def emit_before_async_with(self) -> None:
        self.emit("BEFORE_ASYNC_WITH")

    def make_with_block(self) -> Block | None:
        return self.newBlock("with_block")

    def visitWith_(
        self, node: ast.With | ast.AsyncWith, kind: int, pos: int = 0
    ) -> None:
        item = node.items[pos]

        block = self.make_with_block()
        finally_ = self.newBlock("with_finally")
        exit_ = self.newBlock("with_exit")
        cleanup = self.newBlock("cleanup")
        self.visit(item.context_expr)
        if kind == ASYNC_WITH:
            if IS_3_12_8:
                self.set_pos(item.context_expr)
            self.emit_before_async_with()
            self.emit_get_awaitable(AwaitableKind.AsyncEnter)
            self.emit("LOAD_CONST", None)
            self.emit_yield_from(await_=True)

        self.emit_setup_with(item, finally_, kind == ASYNC_WITH)

        if block is not None:
            self.nextBlock(block)
        self.push_fblock(Entry(kind, block, finally_, node, item.context_expr))
        if item.optional_vars:
            self.visit(item.optional_vars)
        else:
            self.emit("POP_TOP")

        if pos + 1 < len(node.items):
            self.visitWith_(node, kind, pos + 1)
        else:
            self.visitStatements(node.body)

        self.set_with_position_for_exit(node, kind)
        self.pop_fblock(kind)
        self.emit("POP_BLOCK")

        if IS_3_12_8:
            self.set_pos(item.context_expr)
        else:
            self.set_pos(node)
        self.emit_call_exit_with_nones()

        if kind == ASYNC_WITH:
            self.emit_get_awaitable(AwaitableKind.AsyncExit)
            self.emit("LOAD_CONST", None)
            self.emit_yield_from(await_=True)
        self.emit("POP_TOP")
        if kind == ASYNC_WITH:
            self.emitJump(exit_)
        else:
            self.emit_jump_forward(exit_)

        self.nextBlock(finally_)
        self.emit_with_except_cleanup(cleanup)

        if kind == ASYNC_WITH:
            self.emit_get_awaitable(AwaitableKind.AsyncExit)
            self.emit("LOAD_CONST", None)
            self.emit_yield_from(await_=True)

        self.emit_with_except_finish(cleanup)

        self.nextBlock(exit_)

    def visitWith(self, node: ast.With) -> None:
        self.visitWith_(node, WITH, 0)

    def visitAsyncWith(self, node: ast.AsyncWith, pos: int = 0) -> None:
        if not self.scope.coroutine:
            raise self.syntax_error("'async with' outside async function", node)
        self.visitWith_(node, ASYNC_WITH, 0)

    # misc

    def emit_print(self) -> None:
        raise NotImplementedError()

    def visitExpr(self, node: ast.Expr) -> None:
        if self.interactive:
            self.visit(node.value)
            self.emit_print()
        elif is_const(node.value):
            self.emit("NOP")
        else:
            self.visit(node.value)
            self.set_no_pos()
            self.emit("POP_TOP")

    def visitConstant(self, node: ast.Constant) -> None:
        self.emit("LOAD_CONST", node.value)

    # pyre-fixme[2] It is not clear the type of this node because ast.keyword
    # exists but does not have a name field on it.
    def visitKeyword(self, node) -> None:
        self.emit("LOAD_CONST", node.name)
        self.visit(node.expr)

    def visitGlobal(self, node: ast.Global) -> None:
        pass

    def visitNonlocal(self, node: ast.Nonlocal) -> None:
        pass

    def visitName(self, node: ast.Name) -> None:
        if isinstance(node.ctx, ast.Store):
            self.storeName(node.id)
        elif isinstance(node.ctx, ast.Del):
            self.delName(node.id)
        else:
            self.loadName(node.id)

    def visitPass(self, node: ast.Pass) -> None:
        self.emit("NOP")  # for line number

    def emit_import_name(self, name: str) -> None:
        raise NotImplementedError()

    def visitImport(self, node: ast.Import) -> None:
        level = 0
        for alias in node.names:
            name = alias.name
            asname = alias.asname
            self.emit("LOAD_CONST", level)
            self.emit("LOAD_CONST", None)
            self.emit_import_name(self.mangle(name))
            mod = name.split(".")[0]
            if asname:
                self.emitImportAs(name, asname)
            else:
                self.storeName(mod)

    def visitImportFrom(self, node: ast.ImportFrom) -> None:
        level = node.level
        fromlist = tuple(alias.name for alias in node.names)
        self.emit("LOAD_CONST", level)
        self.emit("LOAD_CONST", fromlist)
        self.emit_import_name(node.module or "")
        for alias in node.names:
            name = alias.name
            asname = alias.asname
            if name == "*":
                # pyre-fixme[16] This field does not appear to be used.
                self.namespace = 0
                self.emit_import_star()
                # There can only be one name w/ from ... import *
                assert len(node.names) == 1
                return
            else:
                self.emit("IMPORT_FROM", name)
                self.storeName(asname or name)
        self.emit("POP_TOP")

    def emit_import_star(self) -> None:
        self.emit("IMPORT_STAR")

    def emitImportAs(self, name: str, asname: str) -> None:
        elts = name.split(".")
        if len(elts) == 1:
            self.storeName(asname)
            return
        first = True
        for elt in elts[1:]:
            if not first:
                self.emit_rotate_stack(2)
                self.emit("POP_TOP")
            self.emit("IMPORT_FROM", elt)
            first = False
        self.storeName(asname)
        self.emit("POP_TOP")

    # next five implement assignments

    def visitAssign(self, node: ast.Assign) -> None:
        self.visit(node.value)
        dups = len(node.targets) - 1
        for i in range(len(node.targets)):
            elt = node.targets[i]
            if i < dups:
                self.emit_dup()
            if isinstance(elt, ast.AST):
                self.visit(elt)

    def checkAnnExpr(self, node: ast.expr) -> None:
        self.visit(node)
        self.graph.emit_with_loc("POP_TOP", 0, node)

    def checkAnnSlice(self, node: ast.Slice) -> None:
        if node.lower:
            self.checkAnnExpr(node.lower)
        if node.upper:
            self.checkAnnExpr(node.upper)
        if node.step:
            self.checkAnnExpr(node.step)

    def checkAnnSubscr(self, node: ast.expr) -> None:
        if isinstance(node, ast.Slice):
            self.checkAnnSlice(node)
        elif isinstance(node, ast.Tuple):
            # extended slice
            for v in node.elts:
                self.checkAnnSubscr(v)
        else:
            self.checkAnnExpr(node)

    def checkAnnotation(self, node: ast.AnnAssign) -> None:
        if isinstance(self.tree, (ast.Module, ast.ClassDef)):
            if self.future_flags & CO_FUTURE_ANNOTATIONS:
                with self.noEmit():
                    self._visitAnnotation(node.annotation)
            else:
                self.checkAnnExpr(node.annotation)

    def findAnn(self, stmts: list[ast.stmt]) -> bool:
        for stmt in stmts:
            if isinstance(stmt, (ast.ClassDef, ast.FunctionDef, ast.AsyncFunctionDef)):
                # Don't recurse into definitions looking for annotations
                continue
            elif isinstance(stmt, ast.AnnAssign):
                return True
            elif isinstance(stmt, (ast.stmt, ast.ExceptHandler, ast.match_case)):
                for field in stmt._fields:
                    child = getattr(stmt, field)
                    if isinstance(child, list):
                        if self.findAnn(child):
                            return True

        return False

    def _visitAnnotation(self, node: ast.expr) -> None:
        raise NotImplementedError()

    def emit_store_annotation(self, name: str, node: ast.AnnAssign) -> None:
        raise NotImplementedError()

    def emit_simple_ann_assign(self, node: ast.AnnAssign) -> None:
        if node.simple and isinstance(self.tree, (ast.Module, ast.ClassDef)):
            # If we have a simple name in a module or class, store the annotation
            assert isinstance(node.target, ast.Name)
            self.emit_store_annotation(node.target.id, node)
        else:
            # if not, still visit the annotation so we consistently catch bad ones
            with self.noEmit():
                self._visitAnnotation(node.annotation)

    def visitAnnAssign(self, node: ast.AnnAssign) -> None:
        if node.value:
            self.visit(node.value)
            self.visit(node.target)
        if isinstance(node.target, ast.Name):
            self.emit_simple_ann_assign(node)
        elif isinstance(node.target, ast.Attribute):
            if not node.value:
                self.checkAnnExpr(node.target.value)
        elif isinstance(node.target, ast.Subscript):
            if not node.value:
                target = node.target
                assert isinstance(target, ast.Subscript)
                self.checkAnnExpr(target.value)
                self.checkAnnSubscr(target.slice)
        else:
            raise SystemError(
                f"invalid node type {type(node).__name__} for annotated assignment"
            )

        if not node.simple:
            self.checkAnnotation(node)

    # pyre-fixme[2] It is not clear what type this node is because ast.AssName
    # does not exist.
    def visitAssName(self, node) -> None:
        if node.flags == "OP_ASSIGN":
            self.storeName(node.name)
        elif node.flags == "OP_DELETE":
            self.set_pos(node)
            self.delName(node.name)
        else:
            print("oops", node.flags)
            assert 0

    # pyre-fixme[2] It is not clear what type this node is because ast.AssAttr
    # does not exist.
    def visitAssAttr(self, node) -> None:
        self.visit(node.expr)
        if node.flags == "OP_ASSIGN":
            self.emit("STORE_ATTR", self.mangle(node.attrname))
        elif node.flags == "OP_DELETE":
            self.emit("DELETE_ATTR", self.mangle(node.attrname))
        else:
            print("warning: unexpected flags:", node.flags)
            print(node)
            assert 0

    # augmented assignment

    def visitAugAssign(self, node: ast.AugAssign) -> None:
        self.set_pos(node.target)
        if isinstance(node.target, ast.Attribute):
            self.emitAugAttribute(node)
        elif isinstance(node.target, ast.Name):
            self.emitAugName(node)
        else:
            self.emitAugSubscript(node)

    _augmented_opcode: dict[type[ast.operator], str] = {
        ast.Add: "INPLACE_ADD",
        ast.Sub: "INPLACE_SUBTRACT",
        ast.Mult: "INPLACE_MULTIPLY",
        ast.MatMult: "INPLACE_MATRIX_MULTIPLY",
        ast.Div: "INPLACE_TRUE_DIVIDE",
        ast.FloorDiv: "INPLACE_FLOOR_DIVIDE",
        ast.Mod: "INPLACE_MODULO",
        ast.Pow: "INPLACE_POWER",
        ast.RShift: "INPLACE_RSHIFT",
        ast.LShift: "INPLACE_LSHIFT",
        ast.BitAnd: "INPLACE_AND",
        ast.BitXor: "INPLACE_XOR",
        ast.BitOr: "INPLACE_OR",
    }

    def emitAugRHS(self, node: ast.AugAssign) -> None:
        raise NotImplementedError()

    def emitAugName(self, node: ast.AugAssign) -> None:
        target = node.target

        # pyre-fixme[16] This code assumes that the target of the AugAssign is a
        # Name as opposed to ast.Attribute or ast.Subscript.
        name = target.id

        self.loadName(name)
        self.emitAugRHS(node)
        self.set_pos(target)
        self.storeName(name)

    def emitAugAttribute(self, node: ast.AugAssign) -> None:
        raise NotImplementedError()

    def emitAugSubscript(self, node: ast.AugAssign) -> None:
        raise NotImplementedError()

    # pyre-fixme[2] It is not clear which type this node is because ast.Exec
    # does not exist.
    def visitExec(self, node) -> None:
        self.visit(node.expr)
        if node.locals is None:
            self.emit("LOAD_CONST", None)
        else:
            self.visit(node.locals)
        if node.globals is None:
            self.emit_dup()
        else:
            self.visit(node.globals)
        self.emit("EXEC_STMT")

    def compiler_subkwargs(
        self, kwargs: list[ast.keyword], begin: int, end: int
    ) -> None:
        nkwargs = end - begin
        big = (nkwargs * 2) > STACK_USE_GUIDELINE
        if nkwargs > 1 and not big:
            for i in range(begin, end):
                self.visit(kwargs[i].value)
            self.emit("LOAD_CONST", tuple(arg.arg for arg in kwargs[begin:end]))
            self.emit("BUILD_CONST_KEY_MAP", nkwargs)
            return
        if big:
            self.emit_noline("BUILD_MAP", 0)
        for i in range(begin, end):
            self.emit("LOAD_CONST", kwargs[i].arg)
            self.visit(kwargs[i].value)
            if big:
                self.emit_noline("MAP_ADD", 1)
        if not big:
            self.emit("BUILD_MAP", nkwargs)

    def _fastcall_helper(
        self,
        argcnt: int,
        node: ast.expr | None,
        args: list[ast.expr],
        kwargs: list[ast.keyword],
    ) -> None:
        # No * or ** args, faster calling sequence.
        for arg in args:
            self.visit(arg)
        if len(kwargs) > 0:
            self.visit_list(kwargs)
            self.emit("LOAD_CONST", tuple(arg.arg for arg in kwargs))
            self.emit("CALL_FUNCTION_KW", argcnt + len(args) + len(kwargs))
        else:
            self.emit("CALL_FUNCTION", argcnt + len(args))

    def emit_call_function_ex(self, nkwelts: int) -> None:
        self.emit("CALL_FUNCTION_EX", int(nkwelts > 0))

    def _call_helper(
        self,
        argcnt: int,
        node: ast.expr | None,
        args: list[ast.expr],
        kwargs: list[ast.keyword],
    ) -> None:
        starred = any(isinstance(arg, ast.Starred) for arg in args)
        mustdictunpack = any(arg.arg is None for arg in kwargs)
        manyargs = (len(args) + (len(kwargs) * 2)) > STACK_USE_GUIDELINE
        if not (starred or mustdictunpack or manyargs):
            return self._fastcall_helper(argcnt, node, args, kwargs)

        # Handle positional arguments.
        if argcnt == 0 and len(args) == 1 and starred:
            star_expr = args[0]
            assert isinstance(star_expr, ast.Starred)
            self.visit(star_expr.value)
        else:
            self._visitSequenceLoad(
                args,
                "BUILD_LIST",
                "LIST_APPEND",
                "LIST_EXTEND",
                num_pushed=argcnt,
                is_tuple=True,
            )
        nkwelts = len(kwargs)
        if nkwelts > 0:
            seen = 0
            have_dict = False
            for i, arg in enumerate(kwargs):
                if arg.arg is None:
                    if seen > 0:
                        # Unpack
                        self.compiler_subkwargs(kwargs, i - seen, i)
                        if have_dict:
                            self.emit("DICT_MERGE", 1)
                        have_dict = True
                        seen = 0
                    if not have_dict:
                        self.emit("BUILD_MAP", 0)
                        have_dict = True
                    self.visit(arg.value)
                    self.emit("DICT_MERGE", 1)
                else:
                    seen += 1
            if seen > 0:
                self.compiler_subkwargs(kwargs, nkwelts - seen, nkwelts)
                if have_dict:
                    self.emit("DICT_MERGE", 1)

        self.emit_call_function_ex(nkwelts)

    # Used in subclasses to specialise `super` calls.

    def is_super_shadowed(self) -> bool:
        if self.check_name("super") != SC_GLOBAL_IMPLICIT:
            return True
        module_scope = self.module_gen.check_name("super")
        return module_scope != SC_GLOBAL_IMPLICIT and module_scope != SC_LOCAL

    def _is_super_call(self, node: ast.expr) -> bool:
        if (
            not isinstance(node, ast.Call)
            or not isinstance(node.func, ast.Name)
            or node.func.id != "super"
            or node.keywords
        ):
            return False

        # check that 'super' only appear as implicit global:
        # it is not defined in local or modules scope
        if self.is_super_shadowed():
            return False

        if len(node.args) == 2:
            return True
        if len(node.args) == 0:
            return (
                len(self.scope.params) > 0 and self.check_name("__class__") == SC_FREE
            )

        return False

    def _emit_args_for_super(self, super_call: ast.Call, attr: str) -> tuple[str, bool]:
        if len(super_call.args) == 0:
            self.loadName("__class__")
            self.loadName(next(iter(self.scope.params)))
        else:
            for arg in super_call.args:
                self.visit(arg)
        return (self.mangle(attr), len(super_call.args) == 0)

    def _can_optimize_call(self, node: ast.Call) -> bool:
        raise NotImplementedError()

    def checkReturn(self, node: ast.Return) -> None:
        if not isinstance(self.tree, (ast.FunctionDef, ast.AsyncFunctionDef)):
            raise self.syntax_error("'return' outside function", node)
        elif self.scope.coroutine and self.scope.generator and node.value:
            raise self.syntax_error("'return' with value in async generator", node)

    def visitReturn(self, node: ast.Return) -> None:
        self.checkReturn(node)

        preserve_tos = bool(node.value and not isinstance(node.value, ast.Constant))
        if preserve_tos:
            value = node.value
            assert value and not isinstance(value, ast.Constant)

            self.visit(value)
        elif node.value:
            self.set_pos(node.value)
            self.emit("NOP")
        if not node.value or node.value.lineno != node.lineno:
            self.set_pos(node)
            self.emit("NOP")
        self.unwind_setup_entries(preserve_tos)
        if not node.value:
            self.emit("LOAD_CONST", None)
        elif not preserve_tos:
            value = node.value
            assert isinstance(value, ast.Constant)

            self.emit("LOAD_CONST", value.value)

        self.emit("RETURN_VALUE")
        self.nextBlock()

    def emit_yield(self, scope: Scope) -> None:
        raise NotImplementedError()

    def visitYield(self, node: ast.Yield) -> None:
        if not isinstance(
            self.tree,
            (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.GeneratorExp),
        ):
            raise self.syntax_error("'yield' outside function", node)
        if node.value:
            self.visit(node.value)
        else:
            self.emit("LOAD_CONST", None)
        self.emit_yield(self.scope)

    def visitYieldFrom(self, node: ast.YieldFrom) -> None:
        if not isinstance(
            self.tree,
            (ast.FunctionDef, ast.AsyncFunctionDef, ast.Lambda, ast.GeneratorExp),
        ):
            raise self.syntax_error(
                (
                    "'yield from' outside function"
                    if IS_3_12_8
                    else "'yield' outside function"
                ),
                node,
            )
        elif self.scope.coroutine:
            raise self.syntax_error("'yield from' inside async function", node)

        self.visit(node.value)
        self.emit("GET_YIELD_FROM_ITER")
        self.emit("LOAD_CONST", None)
        self.emit_yield_from()

    def visitAwait(self, node: ast.Await) -> None:
        self.visit(node.value)
        self.emit_get_awaitable(AwaitableKind.Default)
        self.emit("LOAD_CONST", None)
        self.emit_yield_from(await_=True)

    # slice and subscript stuff
    def visitSubscript(self, node: ast.Subscript, aug_flag: bool = False) -> None:
        raise NotImplementedError()

    # unary ops

    _unary_opcode: dict[type[ast.unaryop], str] = {
        ast.Invert: "UNARY_INVERT",
        ast.USub: "UNARY_NEGATIVE",
        ast.UAdd: "UNARY_POSITIVE",
        ast.Not: "UNARY_NOT",
    }

    def unaryOp(self, node: ast.UnaryOp, op: str) -> None:
        raise NotImplementedError()

    def visitUnaryOp(self, node: ast.UnaryOp) -> None:
        self.unaryOp(node, self._unary_opcode[type(node.op)])

    # pyre-fixme[2] It is not clear what this node type is, as ast.Backquote
    # does not exist.
    def visitBackquote(self, node) -> None:
        return self.unaryOp(node, "UNARY_CONVERT")

    # object constructors

    def visitEllipsis(self, node: ast.Ellipsis) -> None:
        self.emit("LOAD_CONST", Ellipsis)

    def _visitUnpack(self, node: ast.Tuple | ast.List | ast.Set) -> None:
        before = 0
        after = 0
        starred = None
        for elt in node.elts:
            if isinstance(elt, ast.Starred):
                if starred is not None:
                    raise self.syntax_error(
                        "multiple starred expressions in assignment", elt
                    )
                elif before >= 256 or len(node.elts) - before - 1 >= (1 << 31) >> 8:
                    raise self.syntax_error(
                        "too many expressions in star-unpacking assignment", elt
                    )
                starred = elt.value
            elif starred:
                after += 1
            else:
                before += 1
        if starred:
            self.emit("UNPACK_EX", after << 8 | before)
        else:
            self.emit("UNPACK_SEQUENCE", before)

    def hasStarred(self, elts: list[ast.expr]) -> bool:
        for elt in elts:
            if isinstance(elt, ast.Starred):
                return True
        return False

    def _visitSequence(
        self,
        node: ast.Tuple | ast.List | ast.Set,
        build_op: str,
        add_op: str,
        extend_op: str,
        ctx: ast.expr_context | ast.Load,
        is_tuple: bool = False,
    ) -> None:
        if isinstance(ctx, ast.Store):
            self._visitUnpack(node)
            for elt in node.elts:
                if isinstance(elt, ast.Starred):
                    self.visit(elt.value)
                else:
                    self.visit(elt)
            return
        elif isinstance(ctx, ast.Load):
            return self._visitSequenceLoad(
                node.elts, build_op, add_op, extend_op, num_pushed=0, is_tuple=is_tuple
            )
        else:
            return self.visit_list(node.elts)

    def visitStarred(self, node: ast.Starred) -> None:
        if isinstance(node.ctx, ast.Store):
            raise self.syntax_error(
                "starred assignment target must be in a list or tuple", node
            )
        else:
            raise self.syntax_error("can't use starred expression here", node)

    def visitTuple(self, node: ast.Tuple) -> None:
        self._visitSequence(
            node, "BUILD_LIST", "LIST_APPEND", "LIST_EXTEND", node.ctx, is_tuple=True
        )

    def visitList(self, node: ast.List) -> None:
        self._visitSequence(node, "BUILD_LIST", "LIST_APPEND", "LIST_EXTEND", node.ctx)

    def visitSet(self, node: ast.Set) -> None:
        self._visitSequence(node, "BUILD_SET", "SET_ADD", "SET_UPDATE", ast.Load())

    def visitSlice(self, node: ast.Slice) -> None:
        num = 2
        if node.lower:
            self.visit(node.lower)
        else:
            self.emit("LOAD_CONST", None)
        if node.upper:
            self.visit(node.upper)
        else:
            self.emit("LOAD_CONST", None)
        if node.step:
            self.visit(node.step)
            num += 1
        self.emit("BUILD_SLICE", num)

    def visitExtSlice(self, node: ast.ExtSlice) -> None:
        for d in node.dims:
            self.visit(d)
        self.emit("BUILD_TUPLE", len(node.dims))

    def emit_dup(self, count: int = 1) -> None:
        raise NotImplementedError()

    def visitNamedExpr(self, node: ast.NamedExpr) -> None:
        self.visit(node.value)
        self.emit_dup()
        self.visit(node.target)

    def _const_value(
        self, node: ast.expr | None
    ) -> None | str | bytes | bool | int | float | complex | ast.Ellipsis:
        assert isinstance(node, ast.Constant)
        return node.value

    def get_bool_const(self, node: ast.expr) -> bool | None:
        """Return True if node represent constantly true value, False if
        constantly false value, and None otherwise (non-constant)."""
        if isinstance(node, ast.Constant):
            return bool(node.value)

    def compile_subdict(self, node: ast.Dict, begin: int, end: int) -> None:
        n = end - begin
        big = n * 2 > STACK_USE_GUIDELINE
        if n > 1 and not big and all_items_const(node.keys, begin, end):
            for i in range(begin, end):
                self.visit(node.values[i])

            self.emit(
                "LOAD_CONST", tuple(self._const_value(x) for x in node.keys[begin:end])
            )
            self.emit("BUILD_CONST_KEY_MAP", n)
            return

        if big:
            self.emit("BUILD_MAP", 0)

        for i in range(begin, end):
            key = node.keys[i]
            assert key

            self.visit(key)
            self.visit(node.values[i])
            if big:
                self.emit("MAP_ADD", 1)

        if not big:
            self.emit("BUILD_MAP", n)

    def visitDict(self, node: ast.Dict) -> None:
        elements = 0
        is_unpacking = False
        have_dict = False
        n = len(node.values)

        for i, (k, v) in enumerate(zip(node.keys, node.values)):
            is_unpacking = k is None
            if is_unpacking:
                if elements:
                    self.compile_subdict(node, i - elements, i)
                    if have_dict:
                        self.emit("DICT_UPDATE", 1)
                    have_dict = True
                    elements = 0
                if not have_dict:
                    self.emit("BUILD_MAP", 0)
                    have_dict = True
                self.visit(v)
                self.emit("DICT_UPDATE", 1)
            else:
                if elements * 2 > STACK_USE_GUIDELINE:
                    self.compile_subdict(node, i - elements, i + 1)
                    if have_dict:
                        self.emit("DICT_UPDATE", 1)
                    have_dict = True
                    elements = 0
                else:
                    elements += 1

        if elements:
            self.compile_subdict(node, n - elements, n)
            if have_dict:
                self.emit("DICT_UPDATE", 1)
            have_dict = True

        if not have_dict:
            self.emit("BUILD_MAP")

    def storePatternName(self, name: str, pc: PatternContext) -> None:
        return self.storeName(name)

    def set_match_pos(self, node: AST) -> None:
        pass

    def emit_match_jump_to_end(self, end: Block) -> None:
        raise NotImplementedError()

    def emit_match_jump_to_fail_pop_unconditional(self, pc: PatternContext) -> None:
        raise NotImplementedError()

    def emit_pop_subject(self, case: ast.match_case) -> None:
        self.set_pos(case.pattern)
        self.emit("POP_TOP")

    def visitMatch(self, node: ast.Match) -> None:
        """See compiler_match_inner in compile.c"""
        pc = self.pattern_context()
        self.visit(node.subject)
        end = self.newBlock("match_end")
        assert node.cases, node.cases
        last_case = node.cases[-1]
        has_default = (
            self._wildcard_check(node.cases[-1].pattern) and len(node.cases) > 1
        )
        cases = list(node.cases)
        if has_default:
            cases.pop()
        for case in cases:
            self.set_pos(case.pattern)
            # Only copy the subject if we're *not* on the last case:
            is_last_non_default_case = case is cases[-1]
            if not is_last_non_default_case:
                self.emit_dup()
            pc.stores = []
            # Irrefutable cases must be either guarded, last, or both:
            pc.allow_irrefutable = case.guard is not None or case is last_case
            pc.fail_pop = []
            pc.on_top = 0
            self.visit(case.pattern, pc)
            assert not pc.on_top
            # It's a match! Store all of the captured names (they're on the stack).
            self.set_match_pos(case.pattern)
            for name in pc.stores:
                self.storePatternName(name, pc)
            guard = case.guard
            if guard:
                self._ensure_fail_pop(pc, 0)
                self.compileJumpIf(guard, pc.fail_pop[0], False)
            # Success! Pop the subject off, we're done with it:
            if not is_last_non_default_case:
                self.emit_pop_subject(case)
            self.visit_list(case.body)
            self.emit_match_jump_to_end(end)
            # If the pattern fails to match, we want the line number of the
            # cleanup to be associated with the failed pattern, not the last line
            # of the body
            self.set_pos(case.pattern)
            self._emit_and_reset_fail_pop(pc)

        if has_default:
            # A trailing "case _" is common, and lets us save a bit of redundant
            # pushing and popping in the loop above:
            self.set_pos(last_case.pattern)
            if len(node.cases) == 1:
                # No matches. Done with the subject:
                self.emit("POP_TOP")
            else:
                # Show line coverage for default case (it doesn't create bytecode)
                self.emit("NOP")
            if last_case.guard:
                self.compileJumpIf(last_case.guard, end, False)
            self.visit_list(last_case.body)
        self.nextBlock(end)

    def visitMatchValue(self, node: ast.MatchValue, pc: PatternContext) -> None:
        """See compiler_pattern_value in compile.c"""
        value = node.value
        if not isinstance(value, ast.Constant | ast.Attribute):
            raise self.syntax_error(
                "patterns may only match literals and attribute lookups", node
            )
        self.visit(value)
        self.emit("COMPARE_OP", "==")
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE", to_bool=True)

    def visitMatchSingleton(self, node: ast.MatchSingleton, pc: PatternContext) -> None:
        self.emit("LOAD_CONST", node.value)
        self.emit("IS_OP")
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")

    def visitMatchSequence(self, node: ast.MatchSequence, pc: PatternContext) -> None:
        """See compiler_pattern_sequence in compile.c"""
        patterns = node.patterns
        size = len(patterns)
        star = -1
        only_wildcard = True
        star_wildcard = False
        # Find a starred name, if it exists. There may be at most one:
        for i, pattern in enumerate(patterns):
            if isinstance(pattern, ast.MatchStar):
                if star >= 0:
                    raise self.syntax_error(
                        "multiple starred names in sequence pattern", pattern
                    )
                star_wildcard = self._wildcard_star_check(pattern)
                only_wildcard = only_wildcard and star_wildcard
                star = i
                continue
            only_wildcard = only_wildcard and self._wildcard_check(pattern)

        # We need to keep the subject on top during the sequence and length checks:
        pc.on_top += 1
        self.emit("MATCH_SEQUENCE")
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        if star < 0:
            # No star: len(subject) == size
            self.emit("GET_LEN")
            self.emit("LOAD_CONST", size)
            self.emit("COMPARE_OP", "==")
            self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        elif size > 1:
            # Star: len(subject) >= size - 1
            self.emit("GET_LEN")
            self.emit("LOAD_CONST", size - 1)
            self.emit("COMPARE_OP", ">=")
            self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        # Whatever comes next should consume the subject
        pc.on_top -= 1
        if only_wildcard:
            # Patterns like: [] / [_] / [_, _] / [*_] / [_, *_] / [_, _, *_] / etc.
            self.emit("POP_TOP")
        elif star_wildcard:
            self._pattern_helper_sequence_subscr(node, patterns, star, pc)
        else:
            self._pattern_helper_sequence_unpack(patterns, star, pc)

    def _pattern_helper_sequence_unpack(
        self, patterns: list[ast.pattern], star: int, pc: PatternContext
    ) -> None:
        self._pattern_unpack_helper(patterns)
        size = len(patterns)
        pc.on_top += size
        for pattern in patterns:
            pc.on_top -= 1
            self._visit_subpattern(pattern, pc)

    def _pattern_helper_sequence_subscr(
        self, node: AST, patterns: list[ast.pattern], star: int, pc: PatternContext
    ) -> None:
        """
        Like _pattern_helper_sequence_unpack, but uses BINARY_SUBSCR instead of
        UNPACK_SEQUENCE / UNPACK_EX. This is more efficient for patterns with a
        starred wildcard like [first, *_] / [first, *_, last] / [*_, last] / etc.
        """
        # We need to keep the subject around for extracting elements:
        pc.on_top += 1
        size = len(patterns)
        for i, pattern in enumerate(patterns):
            if self._wildcard_check(pattern):
                continue
            if i == star:
                assert self._wildcard_star_check(pattern)
                continue
            self.emit_dup()
            if i < star:
                self.emit("LOAD_CONST", i)
            else:
                # The subject may not support negative indexing! Compute a
                # nonnegative index:
                self.emit("GET_LEN")
                self.emit("LOAD_CONST", size - i)
                self.emit_bin_op(ast.Sub)
            self.emit_binary_subscr()
            self.set_pos(pattern)
            self._visit_subpattern(pattern, pc)
            self.set_pos(node)

        # Pop the subject, we're done with it:
        pc.on_top -= 1
        self.emit("POP_TOP")

    def _pattern_unpack_helper(self, elts: list[ast.pattern]) -> None:
        n = len(elts)
        seen_star = 0
        for i, elt in enumerate(elts):
            if isinstance(elt, ast.MatchStar) and not seen_star:
                if (i >= (1 << 8)) or (n - i - 1 >= (INT_MAX >> 8)):
                    raise self.syntax_error(
                        "too many expressions in star-unpacking sequence pattern", elt
                    )
                self.emit("UNPACK_EX", (i + ((n - i - 1) << 8)))
                seen_star = 1
            elif isinstance(elt, ast.MatchStar):
                raise self.syntax_error(
                    "multiple starred expressions in sequence pattern", elt
                )
        if not seen_star:
            self.emit("UNPACK_SEQUENCE", n)

    def _wildcard_check(self, pattern: ast.pattern) -> bool:
        return isinstance(pattern, ast.MatchAs) and not pattern.name

    def _wildcard_star_check(self, pattern: ast.pattern) -> bool:
        return isinstance(pattern, ast.MatchStar) and not pattern.name

    def _visit_subpattern(self, pattern: ast.pattern, pc: PatternContext) -> None:
        """Visit a pattern, but turn off checks for irrefutability.

        See compiler_pattern_subpattern in compile.c
        """
        allow_irrefutable = pc.allow_irrefutable
        pc.allow_irrefutable = True
        try:
            self.visit(pattern, pc)
        finally:
            pc.allow_irrefutable = allow_irrefutable

    def visitMatchMapping(self, node: ast.MatchMapping, pc: PatternContext) -> None:
        keys = node.keys
        patterns = node.patterns
        size = len(keys)
        npatterns = len(patterns)
        if size != npatterns:
            # AST validator shouldn't let this happen, but if it does,
            # just fail, don't crash out of the interpreter
            raise self.syntax_error(
                f"keys ({size}) / patterns ({npatterns}) length mismatch in mapping pattern",
                node,
            )
        # We have a double-star target if "rest" is set
        star_target = node.rest
        # We need to keep the subject on top during the mapping and length checks:
        pc.on_top += 1
        self.emit("MATCH_MAPPING")
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        if not size and not star_target:
            # If the pattern is just "{}", we're done! Pop the subject:
            pc.on_top -= 1
            self.emit("POP_TOP")
            return
        if size:
            # If the pattern has any keys in it, perform a length check:
            self.emit("GET_LEN")
            self.emit("LOAD_CONST", size)
            self.emit("COMPARE_OP", ">=")
            self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        if INT_MAX < size - 1:
            raise self.syntax_error("too many sub-patterns in mapping pattern", node)
        # Collect all of the keys into a tuple for MATCH_KEYS and
        # COPY_DICT_WITHOUT_KEYS. They can either be dotted names or literals:

        # Maintaining a set of Constant keys allows us to raise a
        # SyntaxError in the case of duplicates.
        seen = set()

        for i, key in enumerate(keys):
            if key is None:
                raise self.syntax_error(
                    "Can't use NULL keys in MatchMapping (set 'rest' parameter instead)",
                    patterns[i],
                )
            if isinstance(key, ast.Constant):
                if key.value in seen:
                    raise self.syntax_error(
                        f"mapping pattern checks duplicate key ({key.value})", node
                    )
                seen.add(key.value)
            elif not isinstance(key, ast.Attribute):
                raise self.syntax_error(
                    "mapping pattern keys may only match literals and attribute lookups",
                    node,
                )
            self.visit(key)

        self.emit_finish_match_mapping(node, pc, star_target, size)

    def emit_finish_match_mapping(
        self,
        node: ast.MatchMapping,
        pc: PatternContext,
        star_target: str | None,
        size: int,
    ) -> None:
        raise NotImplementedError()

    def emit_finish_match_class(self, node: ast.MatchClass, pc: PatternContext) -> None:
        raise NotImplementedError()

    def visitMatchClass(self, node: ast.MatchClass, pc: PatternContext) -> None:
        patterns = node.patterns
        kwd_attrs = node.kwd_attrs
        kwd_patterns = node.kwd_patterns
        nargs = len(patterns)
        nattrs = len(kwd_attrs)
        nkwd_patterns = len(kwd_patterns)
        if nattrs != nkwd_patterns:
            # AST validator shouldn't let this happen, but if it does,
            # just fail, don't crash out of the interpreter
            raise self.syntax_error(
                f"kwd_attrs ({nattrs}) / kwd_patterns ({nkwd_patterns}) length mismatch in class pattern",
                node,
            )
        if INT_MAX < nargs or INT_MAX < (nargs + nattrs - 1):
            raise self.syntax_error(
                f"too many sub-patterns in class pattern {node.cls}", node
            )
        if nattrs:
            self._validate_kwd_attrs(kwd_attrs, kwd_patterns)
        self.visit(node.cls)
        attr_names = tuple(name for name in kwd_attrs)
        self.set_pos(node)
        self.emit("LOAD_CONST", attr_names)
        self.emit("MATCH_CLASS", nargs)
        self.emit_finish_match_class(node, pc)

    def _validate_kwd_attrs(
        self, attrs: list[str], patterns: list[ast.pattern]
    ) -> None:
        # Any errors will point to the pattern rather than the arg name as the
        # parser is only supplying identifiers
        nattrs = len(attrs)
        for i, attr in enumerate(attrs):
            pattern = patterns[i]
            self.set_pos(pattern)
            self._check_forbidden_name(attr, ast.Store, pattern)
            for j in range(i + 1, nattrs):
                other = attrs[j]
                if attr == other:
                    self.set_pos(patterns[j])
                    raise self.syntax_error(
                        f"attribute name repeated in class pattern: {attr}", patterns[j]
                    )

    def visitMatchStar(self, node: ast.MatchStar, pc: PatternContext) -> None:
        self._pattern_helper_store_name(node.name, pc, node)

    def visitMatchAs(self, node: ast.MatchAs, pc: PatternContext) -> None:
        """See compiler_pattern_as in compile.c"""
        pat = node.pattern
        if pat is None:
            # An irrefutable match:
            if not pc.allow_irrefutable:
                if node.name:
                    raise SyntaxError(
                        f"name capture {node.name!r} makes remaining patterns unreachable"
                    )
                raise SyntaxError("wildcard makes remaining patterns unreachable")
            self._pattern_helper_store_name(node.name, pc, node)
            return

        # Need to make a copy for (possibly) storing later:
        pc.on_top += 1
        self.emit_dup()
        self.visit(pat, pc)
        # Success! Store it:
        pc.on_top -= 1
        self._pattern_helper_store_name(node.name, pc, node)

    def emit_rot_n(self, n: int) -> None:
        raise NotImplementedError()

    def visitMatchOr(self, node: ast.MatchOr, pc: PatternContext) -> None:
        """See compiler_pattern_or in compile.c"""
        end = self.newBlock("match_or_end")
        size = len(node.patterns)
        assert size > 1
        # We're going to be messing with pc. Keep the original info handy:
        old_pc = pc
        pc = pc.clone()
        # control is the list of names bound by the first alternative. It is used
        # for checking different name bindings in alternatives, and for correcting
        # the order in which extracted elements are placed on the stack.
        control: list[str] | None = None
        for alt in node.patterns:
            self.set_pos(alt)
            pc.stores = []
            # An irrefutable sub-pattern must be last, if it is allowed at all:
            pc.allow_irrefutable = (
                alt is node.patterns[-1]
            ) and old_pc.allow_irrefutable
            pc.fail_pop = []
            pc.on_top = 0
            self.emit_dup()
            self.visit(alt, pc)
            # Success!
            nstores = len(pc.stores)
            if alt is node.patterns[0]:
                # This is the first alternative, so save its stores as a "control"
                # for the others (they can't bind a different set of names, and
                # might need to be reordered):
                assert control is None
                control = pc.stores
            elif nstores != len(control or []):
                raise SyntaxError("alternative patterns bind different names")
            elif nstores:
                # There were captures. Check to see if we differ from control:
                icontrol = nstores
                assert control is not None
                while icontrol:
                    icontrol -= 1
                    name = control[icontrol]
                    istores = pc.stores.index(name)
                    if icontrol != istores:
                        # Reorder the names on the stack to match the order of the
                        # names n control. There's probably a better way of doing
                        # this; the current solution is potentially very
                        # inefficient when each alternative subpattern binds lots
                        # of names in different orders. It's fine for reasonable
                        # cases, though.
                        assert istores < icontrol
                        rotations = istores + 1
                        # Perform the same rotation on pc.stores:
                        rotated = pc.stores[:rotations]
                        del pc.stores[:rotations]
                        pc.stores[(icontrol - istores) : (icontrol - istores)] = rotated
                        # Do the same to the stack, using several ROT_Ns:
                        self.set_pos(alt)
                        for _ in range(rotations):
                            self.emit_rot_n(icontrol + 1)

            assert control is not None
            self.set_pos(alt)
            self.emit_jump_forward(end)
            self.nextBlock()
            self._emit_and_reset_fail_pop(pc)
        assert control is not None
        pc = old_pc
        # No match. Pop the remaining copy of the subject and fail:
        self.set_match_pos(node)
        self.emit("POP_TOP")
        self.emit_match_jump_to_fail_pop_unconditional(pc)
        self.nextBlock(end)
        nstores = len(control)
        # There's a bunch of stuff on the stack between any where the new stores
        # are and where they need to be:
        # - The other stores.
        # - A copy of the subject.
        # - Anything else that may be on top of the stack.
        # - Any previous stores we've already stashed away on the stack.
        nrots = nstores + 1 + pc.on_top + len(pc.stores)
        for i in range(nstores):
            # Rotate this capture to its proper place on the stack:
            self.emit_rot_n(nrots)
            # Update the list of previous stores with this new name, checking for
            # duplicates:
            name = control[i]
            dupe = name in pc.stores
            if dupe:
                raise self._error_duplicate_store(name)
            pc.stores.append(name)
        # Pop the copy of the subject:
        self.emit("POP_TOP")

    def _pattern_helper_store_name(
        self, name: str | None, pc: PatternContext, loc: ast.AST
    ) -> None:
        if name is None:
            self.emit("POP_TOP")
            return

        self._check_forbidden_name(name, ast.Store, loc)

        # Can't assign to the same name twice:
        duplicate = name in pc.stores
        if duplicate:
            raise self._error_duplicate_store(name)

        # Rotate this object underneath any items we need to preserve:
        self.set_match_pos(loc)
        self.emit_rot_n(pc.on_top + len(pc.stores) + 1)
        pc.stores.append(name)

    def _check_forbidden_name(
        self, name: str, ctx: type[ast.expr_context], loc: ast.AST
    ) -> None:
        if ctx is ast.Store and name == "__debug__":
            raise SyntaxError("cannot assign to __debug__", loc)
        if ctx is ast.Del and name == "__debug__":
            raise SyntaxError("cannot delete __debug__", loc)

    def _error_duplicate_store(self, name: str) -> SyntaxError:
        return SyntaxError(f"multiple assignments to name {name!r} in pattern")

    def _ensure_fail_pop(self, pc: PatternContext, n: int) -> None:
        """Resize pc.fail_pop to allow for n items to be popped on failure."""
        size = n + 1
        if size <= len(pc.fail_pop):
            return

        while len(pc.fail_pop) < size:
            pc.fail_pop.append(self.newBlock(f"match_fail_pop_{len(pc.fail_pop)}"))

    def _jump_to_fail_pop(
        self, pc: PatternContext, op: str, to_bool: bool = False
    ) -> None:
        """Use op to jump to the correct fail_pop block."""
        # Pop any items on the top of the stack, plus any objects we were going to
        # capture on success:
        pops = pc.on_top + len(pc.stores)
        self._ensure_fail_pop(pc, pops)
        if to_bool:
            self.emit_cond_jump(op, pc.fail_pop[pops])
        else:
            self.emit(op, pc.fail_pop[pops])
        self.nextBlock()

    def _emit_and_reset_fail_pop(self, pc: PatternContext) -> None:
        """Build all of the fail_pop blocks and reset fail_pop."""
        if not pc.fail_pop:
            self.nextBlock()
        while pc.fail_pop:
            self.nextBlock(pc.fail_pop.pop())
            if pc.fail_pop:
                self.emit("POP_TOP")

    def unwind_setup_entry(self, e: Entry, preserve_tos: int) -> None:
        raise NotImplementedError()

    def unwind_setup_entries(
        self, preserve_tos: bool, stop_on_loop: bool = False
    ) -> Entry | None:
        if len(self.setups) == 0:
            return None

        top = self.setups[-1]
        if stop_on_loop and top.kind in (WHILE_LOOP, FOR_LOOP, ASYNC_FOR_LOOP):
            return top

        copy = self.pop_fblock()
        self.unwind_setup_entry(copy, preserve_tos)
        loop = self.unwind_setup_entries(preserve_tos, stop_on_loop)
        self.push_fblock(copy)
        return loop

    def get_node_name(self, node: AST) -> str:
        if isinstance(node, (ast.FunctionDef, ast.ClassDef, ast.AsyncFunctionDef)):
            return node.name
        elif isinstance(node, ast.SetComp):
            return "<setcomp>"
        elif isinstance(node, ast.ListComp):
            return "<listcomp>"
        elif isinstance(node, ast.DictComp):
            return "<dictcomp>"
        elif isinstance(node, ast.GeneratorExp):
            return "<genexpr>"
        elif isinstance(node, ast.Lambda):
            return "<lambda>"
        elif isinstance(node, ast.Module):
            return "<module>"
        elif isinstance(node, ast.Expression):
            return "<string>"
        elif isinstance(node, ast.Interactive):
            return "<stdin>"
        # pyre-ignore[16]: Module `ast` has no attribute `TypeAlias`.
        elif hasattr(ast, "TypeAlias") and isinstance(node, ast.TypeAlias):
            # pyre-ignore[16]: `_ast.AST` has no attribute `name`
            return node.name.id
        # pyre-ignore[16]: Module `ast` has no attribute `TypeVar`.
        elif hasattr(ast, "TypeVar") and isinstance(node, ast.TypeVar):
            return node.name
        elif isinstance(node, TypeParams):
            return f"<params {[self.get_node_name(x) for x in node.params]}>"
        elif isinstance(node, ast.arguments):
            return "<arguments>"
        elif isinstance(node, Annotations):
            return "__annotate__"

        raise NotImplementedError("Unknown node type: " + type(node).__name__)

    def finish_function(self) -> None:
        current = self.graph.current
        assert current

        if current.returns:
            return

        if not isinstance(self.tree, ast.Lambda):
            self.set_no_pos()
            self.emit("LOAD_CONST", None)
        self.emit("RETURN_VALUE")

    def make_func_codegen(
        self,
        func: FuncOrLambda | CompNode,
        func_args: ast.arguments,
        name: str,
        first_lineno: int,
    ) -> CodeGenerator:
        filename = self.graph.filename
        symbols = self.symbols
        class_name = self.class_name

        graph = self.make_function_graph(
            func, func_args, filename, symbols.scopes, class_name, name, first_lineno
        )
        graph.emit_gen_start()
        res = self.make_child_codegen(func, graph)
        res.optimized = 1
        res.class_name = class_name
        return res

    def make_class_codegen(
        self, klass: ast.ClassDef, first_lineno: int
    ) -> CodeGenerator:
        filename = self.graph.filename
        symbols = self.symbols

        scope = symbols.scopes[klass]
        assert isinstance(scope, ClassScope)
        graph = self.flow_graph(
            klass.name,
            filename,
            scope,
            optimized=0,
            klass=True,
            docstring=self.get_docstring(klass),
            firstline=first_lineno,
        )

        res = self.make_child_codegen(klass, graph)
        res.class_name = klass.name
        return res

    def make_function_graph(
        self,
        func: FuncOrLambda | CompNode,
        func_args: ast.arguments,
        filename: str,
        scopes: dict[ast.AST, Scope],
        class_name: str | None,
        name: str,
        first_lineno: int,
    ) -> PyFlowGraph:
        args = [
            mangle(elt.arg, class_name)
            for elt in itertools.chain(func_args.posonlyargs, func_args.args)
        ]
        kwonlyargs = [mangle(elt.arg, class_name) for elt in func_args.kwonlyargs]

        starargs = []
        if va := func_args.vararg:
            starargs.append(va.arg)
        if kw := func_args.kwarg:
            starargs.append(kw.arg)

        scope = scopes[func]
        graph = self.flow_graph(
            name,
            filename,
            scope,
            flags=self.get_graph_flags(func, func_args, scope),
            args=args,
            kwonlyargs=kwonlyargs,
            starargs=starargs,
            optimized=1,
            docstring=self.get_docstring(func),
            firstline=first_lineno,
            posonlyargs=len(func_args.posonlyargs),
        )

        return graph

    def get_docstring(
        self, func: ast.Module | ast.ClassDef | FuncOrLambda | CompNode
    ) -> str | None:
        doc = None
        if (
            not isinstance(
                func,
                (ast.Lambda, ast.ListComp, ast.SetComp, ast.DictComp, ast.GeneratorExp),
            )
            and not self.strip_docstrings
        ):
            doc = get_docstring(func)
        return doc

    def get_graph_flags(
        self, func: FuncOrLambda | CompNode, func_args: ast.arguments, scope: Scope
    ) -> int:
        flags = 0

        if func_args.vararg:
            flags = flags | CO_VARARGS
        if func_args.kwarg:
            flags = flags | CO_VARKEYWORDS
        if scope.nested:
            flags = flags | CO_NESTED
        if scope.generator and not scope.coroutine:
            flags = flags | CO_GENERATOR
        if not scope.generator and scope.coroutine:
            flags = flags | CO_COROUTINE
        if scope.generator and scope.coroutine:
            flags = flags | CO_ASYNC_GENERATOR

        return flags

    @classmethod
    def make_code_gen(
        cls,
        module_name: str,
        tree: AST,
        filename: str,
        source: str | bytes | ast.Module | ast.Expression | ast.Interactive,
        flags: int,
        optimize: int,
        ast_optimizer_enabled: bool = True,
    ) -> CodeGenerator:
        future_flags = find_futures(flags, tree)
        if ast_optimizer_enabled:
            tree = cls.optimize_tree(
                optimize, tree, bool(future_flags & CO_FUTURE_ANNOTATIONS)
            )
        s = cls._SymbolVisitor(future_flags)
        s.visit(tree)

        graph = cls.flow_graph(
            module_name,
            filename,
            s.scopes[tree],
            firstline=1,
        )
        code_gen = cls(None, tree, s, graph, flags, optimize, future_flags=future_flags)
        code_gen._qual_name = module_name
        code_gen.visit(tree)
        return code_gen

    @classmethod
    def optimize_tree(
        cls,
        optimize: int,
        tree: AST,
        string_anns: bool,
    ) -> AST:
        result = AstOptimizer(optimize=optimize > 0, string_anns=string_anns).visit(
            tree
        )

        assert isinstance(result, AST)
        return result

    def visit(self, node: AST, *args: object) -> object:
        # Note down the old line number for exprs
        old_loc = None
        if isinstance(node, ast.expr):
            old_loc = self.graph.loc
            self.set_pos(node)
        elif isinstance(node, (ast.stmt, ast.pattern)):
            self.set_pos(node)

        if not args:
            ret = super().visit(node)
        else:
            ret = super().visit(node, *args)

        if old_loc is not None:
            self.graph.loc = old_loc

        return ret

    def visitStatements(self, nodes: Sequence[AST], *args: object) -> None:
        if not args:
            return self.visit_list(nodes)
        self.visit_list(nodes, *args)

    # Methods defined in subclasses ----------------------------------------------

    def emit_super_attribute(self, node: ast.Attribute) -> None:
        raise NotImplementedError()

    def visitAttribute(self, node: ast.Attribute) -> None:
        raise NotImplementedError()

    def visitCall(self, node: ast.Call) -> None:
        raise NotImplementedError()

    def visitFor(self, node: ast.For) -> None:
        raise NotImplementedError()

    def visitClassDef(self, node: ast.ClassDef) -> None:
        raise NotImplementedError()

    def emitJump(self, target: Block) -> None:
        raise NotImplementedError()

    def make_child_codegen(
        self,
        tree: CodeGenTree,
        graph: PyFlowGraph,
    ) -> CodeGenerator:
        raise NotImplementedError()

    def get_qual_prefix(self, gen: CodeHolder) -> str:
        raise NotImplementedError()

    def generate_function(
        self,
        node: FuncOrLambda,
        name: str,
        first_lineno: int,
    ) -> CodeGenerator:
        # pyre-fixme[6] This call to skip_docstring assumes that the body is a
        # list of statements, but when a lambda is passed in here it is possible
        # that it is only an expression.
        body = self.skip_docstring(node.body)
        return self.generate_function_with_body(node, name, first_lineno, body)

    def generate_function_with_body(
        self,
        node: FuncOrLambda,
        name: str,
        first_lineno: int,
        body: list[ast.stmt],
    ) -> CodeGenerator:
        raise NotImplementedError()

    def emit_kwonlydefaults(self, node: FuncOrLambda) -> bool:
        kwdefaults = []
        for kwonly, default in zip(node.args.kwonlyargs, node.args.kw_defaults):
            if default is not None:
                kwdefaults.append(self.mangle(kwonly.arg))
                self.visitDefault(default)

        if kwdefaults:
            self.emit("LOAD_CONST", tuple(kwdefaults))
            self.emit("BUILD_CONST_KEY_MAP", len(kwdefaults))
            return True

        return False

    def build_function(
        self, node: FuncOrLambda, gen: CodeGenerator, first_lineno: int | None = None
    ) -> None:
        raise NotImplementedError()

    def emit_closure(self, gen: CodeHolder, flags: int) -> None:
        prefix = ""
        # pyre-ignore[16]: Module `ast` has no attribute `TypeVar`.
        if not isinstance(gen.tree, self.unqualified_asts):
            prefix = self.get_qual_prefix(gen)

        frees = gen.scope.get_free_vars()
        if frees:
            for name in frees:
                self.emit("LOAD_CLOSURE", name)
            self.emit("BUILD_TUPLE", len(frees))
            flags |= MAKE_FUNCTION_CLOSURE

        self.emit_make_function(gen, prefix + gen.name, flags)

    def emit_make_function(
        self, gen: CodeHolder | CodeGenerator | PyFlowGraph, qualname: str, flags: int
    ) -> None:
        raise NotImplementedError()

    def emit_call_one_arg(self) -> None:
        self.graph.emit_call_one_arg()

    def emit_call_exit_with_nones(self) -> None:
        raise NotImplementedError()

    def emit_prepare_call(self) -> None:
        raise NotImplementedError()

    def emit_call(self, nargs: int) -> None:
        raise NotImplementedError()

    def emit_call_kw(self, nargs: int, kwargs: tuple[str, ...]) -> None:
        raise NotImplementedError()

    def emit_try_except(self, node: ast.Try) -> None:
        raise NotImplementedError()

    def emit_try_finally(
        self,
        node: ast.Try | None,
        try_body: Callable[[], None] | None = None,
        final_body: Callable[[], None] | None = None,
    ) -> None:
        raise NotImplementedError()

    def emit_yield_from(self, await_: bool = False) -> None:
        raise NotImplementedError()

    def emit_rotate_stack(self, count: int) -> None:
        raise NotImplementedError()

    def emit_end_for(self) -> None:
        raise NotImplementedError()

    def _visitSequenceLoad(
        self,
        elts: list[ast.expr],
        build_op: str,
        add_op: str,
        extend_op: str,
        num_pushed: int = 0,
        is_tuple: bool = False,
    ) -> None:
        raise NotImplementedError()

    def emit_load_assertion_error(self, loc: AST | SrcLocation | None = None) -> None:
        if loc:
            self.graph.emit_with_loc("LOAD_ASSERTION_ERROR", 0, loc)
        else:
            self.emit("LOAD_ASSERTION_ERROR")

    def visitAssert(self, node: ast.Assert) -> None:
        raise NotImplementedError()

    def compile_comprehension(
        self,
        node: CompNode,
        name: str,
        elt: ast.expr,
        val: ast.expr | None,
        opcode: str | None,
        oparg: object = 0,
    ) -> None:
        raise NotImplementedError()

    def emit_build_class(self, node: ast.ClassDef, class_body: CodeGenerator) -> None:
        raise NotImplementedError()


class Entry:
    kind: int
    block: Block | None
    exit: Block | None
    unwinding_datum: object

    def __init__(
        self,
        kind: int,
        block: Block | None,
        exit: Block | None,
        unwinding_datum: object,
        node: AST | None = None,
    ) -> None:
        self.kind = kind
        self.block = block
        self.exit = exit
        self.unwinding_datum = unwinding_datum
        self.node = node


class CodeGenerator310(CodeGenerator):
    flow_graph: type[PyFlowGraph] = PyFlowGraph310
    _SymbolVisitor = SymbolVisitor310
    unqualified_asts: tuple[type[ast.AST]] = (ast.ClassDef,)

    def __init__(
        self,
        parent: CodeGenerator | None,
        node: AST,
        symbols: BaseSymbolVisitor,
        graph: PyFlowGraph,
        flags: int = 0,
        optimization_lvl: int = 0,
        future_flags: int | None = None,
        name: str | None = None,
    ) -> None:
        super().__init__(
            parent, node, symbols, graph, flags, optimization_lvl, future_flags, name
        )

    def make_child_codegen(
        self,
        tree: CodeGenTree,
        graph: PyFlowGraph,
    ) -> CodeGenerator310:
        return type(self)(
            self,
            tree,
            self.symbols,
            graph,
            flags=self.flags,
            optimization_lvl=self.optimization_lvl,
        )

    # Names and attribute access --------------------------------------------------

    def _nameOp(self, prefix: str, name: str) -> None:
        # TASK(T130490253): The JIT suppression should happen in the jit, not the compiler.
        if (
            prefix == "LOAD"
            and name == "super"
            and isinstance(self.scope, FunctionScope)
        ):
            scope = self.check_name(name)
            if scope in (SC_GLOBAL_EXPLICIT, SC_GLOBAL_IMPLICIT):
                self.scope.suppress_jit = True

        name = self.mangle(name)
        scope = self.check_name(name)
        if scope == SC_LOCAL:
            if not self.optimized:
                self.emit(prefix + "_NAME", name)
            else:
                self.emit(prefix + "_FAST", name)
        elif scope == SC_GLOBAL_EXPLICIT:
            self.emit(prefix + "_GLOBAL", name)
        elif scope == SC_GLOBAL_IMPLICIT:
            if not self.optimized:
                self.emit(prefix + "_NAME", name)
            else:
                self.emit(prefix + "_GLOBAL", name)
        elif scope == SC_FREE or scope == SC_CELL:
            if isinstance(self.scope, ClassScope) and prefix == "LOAD":
                self.emit(prefix + "_CLASSDEREF", name)
            else:
                self.emit(prefix + "_DEREF", name)
        else:
            raise RuntimeError("unsupported scope for var %s: %d" % (name, scope))

    def visitAssert(self, node: ast.Assert) -> None:
        if not self.optimization_lvl:
            end = self.newBlock()
            self.compileJumpIf(node.test, end, True)

            self.emit_load_assertion_error()
            if node.msg:
                self.visit(node.msg)
                self.emit_call_one_arg()
                self.emit("RAISE_VARARGS", 1)
            else:
                self.emit("RAISE_VARARGS", 1)
            self.nextBlock(end)

    def emit_import_name(self, name: str) -> None:
        self.emit("IMPORT_NAME", name)

    def visitAttribute(self, node: ast.Attribute) -> None:
        self.visit(node.value)

        if isinstance(node.ctx, ast.Store):
            end_lineno = node.end_lineno
            assert end_lineno

            with self.temp_lineno(end_lineno):
                self.emit("STORE_ATTR", self.mangle(node.attr))
        elif isinstance(node.ctx, ast.Del):
            self.emit("DELETE_ATTR", self.mangle(node.attr))
        else:
            end_lineno = node.end_lineno
            assert end_lineno

            with self.temp_lineno(end_lineno):
                self.emit("LOAD_ATTR", self.mangle(node.attr))

    # Stack manipulation --------------------------------------------------

    def emit_rotate_stack(self, count: int) -> None:
        if count == 2:
            self.emit("ROT_TWO")
        elif count == 3:
            self.emit("ROT_THREE")
        elif count == 4:
            self.emit("ROT_FOUR")
        else:
            raise ValueError("Expected rotate of 2, 3, or 4")

    def emit_dup(self, count: int = 1) -> None:
        if count == 1:
            self.emit("DUP_TOP")
        elif count == 2:
            self.emit("DUP_TOP_TWO")
        else:
            raise ValueError(f"Unsupported dup count {count}")

    def _visitAnnotation(self, node: ast.expr) -> None:
        if self.future_flags & CO_FUTURE_ANNOTATIONS:
            self.emit("LOAD_CONST", to_expr(node))
        else:
            self.visit(node)

    def emit_store_annotation(self, name: str, node: ast.AnnAssign) -> None:
        assert self.did_setup_annotations

        self._visitAnnotation(node.annotation)
        self.emit("LOAD_NAME", "__annotations__")
        mangled = self.mangle(name)
        self.emit("LOAD_CONST", mangled)
        self.emit("STORE_SUBSCR")

    # Loops --------------------------------------------------

    def visitFor(self, node: ast.For) -> None:
        start = self.newBlock("for_start")
        body = self.newBlock("for_body")
        cleanup = self.newBlock("for_cleanup")
        end = self.newBlock("for_end")

        self.push_loop(FOR_LOOP, start, end)
        self.visit(node.iter)
        self.emit("GET_ITER")

        self.nextBlock(start)
        self.emit("FOR_ITER", cleanup)
        self.nextBlock(body)
        self.visit(node.target)
        self.visitStatements(node.body)
        self.set_no_pos()
        self.emitJump(start)
        self.nextBlock(cleanup)
        self.pop_loop()

        if node.orelse:
            self.visitStatements(node.orelse)
        self.nextBlock(end)

    def emit_end_for(self) -> None:
        pass

    # Class and function definitions --------------------------------------------------

    def generate_function_with_body(
        self,
        node: FuncOrLambda,
        name: str,
        first_lineno: int,
        body: list[ast.stmt],
    ) -> CodeGenerator:
        gen = cast(
            CodeGenerator310,
            self.make_func_codegen(node, node.args, name, first_lineno),
        )

        self.processBody(node, body, gen)

        gen.finish_function()

        return gen

    def build_function(
        self, node: FuncOrLambda, gen: CodeGenerator, first_lineno: int | None = None
    ) -> None:
        flags = 0
        if node.args.defaults:
            for default in node.args.defaults:
                self.visitDefault(default)
                flags |= MAKE_FUNCTION_DEFAULTS
            self.emit("BUILD_TUPLE", len(node.args.defaults))

        if self.emit_kwonlydefaults(node):
            flags |= MAKE_FUNCTION_KWDEFAULTS

        if not isinstance(node, ast.Lambda):
            flags |= self.build_annotations(node)

        self.emit_closure(gen, flags)

    def emit_function_decorators(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef
    ) -> None:
        for _dec in node.decorator_list:
            self.emit_call_one_arg()

    def visitClassDef(self, node: ast.ClassDef) -> None:
        first_lineno = None
        immutability_flag = self.find_immutability_flag(node)
        for decorator in node.decorator_list:
            if first_lineno is None:
                first_lineno = decorator.lineno
            self.visit_decorator(decorator, node)

        first_lineno = node.lineno if first_lineno is None else first_lineno
        gen = self.make_class_codegen(node, first_lineno)
        gen.emit("LOAD_NAME", "__name__")
        gen.storeName("__module__")
        gen.emit("LOAD_CONST", gen.get_qual_prefix(gen) + gen.name)
        gen.storeName("__qualname__")
        if gen.findAnn(node.body):
            gen.did_setup_annotations = True
            gen.emit("SETUP_ANNOTATIONS")

        doc = gen.get_docstring(node)
        if doc is not None:
            gen.set_pos(node.body[0])
            gen.emit("LOAD_CONST", doc)
            gen.storeName("__doc__")

        self.walkClassBody(node, gen)

        gen.set_no_pos()

        scope = gen.scope
        assert isinstance(scope, ClassScope)

        if scope.needs_class_closure:
            gen.emit("LOAD_CLOSURE", "__class__")
            gen.emit_dup()
            gen.emit("STORE_NAME", "__classcell__")
        else:
            gen.emit("LOAD_CONST", None)
        gen.emit("RETURN_VALUE")

        self.emit_build_class(node, gen)

        for d in reversed(node.decorator_list):
            self.emit_decorator_call(d, node)

        self.register_immutability(node, immutability_flag)
        self.post_process_and_store_name(node)

    def emit_build_class(self, node: ast.ClassDef, class_body: CodeGenerator) -> None:
        self.emit("LOAD_BUILD_CLASS")
        self.emit_closure(class_body, 0)
        self.emit("LOAD_CONST", node.name)

        self._call_helper(2, None, node.bases, node.keywords)

    def get_qual_prefix(self, gen: CodeHolder) -> str:
        prefix = ""
        if gen.scope.global_scope:
            return prefix
        # Construct qualname prefix
        parent = gen.scope.parent
        while not isinstance(parent, ModuleScope):
            # Only real functions use "<locals>", nested scopes like
            # comprehensions don't.
            if parent.is_function_scope:
                prefix = parent.name + ".<locals>." + prefix
            else:
                prefix = parent.name + "." + prefix
            if parent.global_scope:
                break
            parent = parent.parent
        return prefix

    def emit_make_function(
        self, gen: CodeHolder | CodeGenerator | PyFlowGraph, qualname: str, flags: int
    ) -> None:
        if isinstance(gen, CodeGenerator):
            gen.set_qual_name(qualname)
        self.emit("LOAD_CONST", gen)
        self.emit("LOAD_CONST", qualname)  # py3 qualname
        self.emit("MAKE_FUNCTION", flags)

    # Comprehensions --------------------------------------------------

    def emit_get_awaitable(self, kind: AwaitableKind) -> None:
        self.emit("GET_AWAITABLE")

    def compile_comprehension(
        self,
        node: CompNode,
        name: str,
        elt: ast.expr,
        val: ast.expr | None,
        opcode: str | None,
        oparg: object = 0,
    ) -> None:
        self.check_async_comprehension(node)

        gen = cast(CodeGenerator310, self.make_comprehension_codegen(node, name))
        gen.set_pos(node)

        if opcode:
            gen.emit(opcode, oparg)

        gen.compile_comprehension_generator(node, 0, 0, elt, val, type(node), True)

        if not isinstance(node, ast.GeneratorExp):
            gen.emit("RETURN_VALUE")

        gen.finish_function()

        self.finish_comprehension(gen, node)

    def compile_comprehension_generator(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[CompNode],
        outermost_gen_is_param: bool,
    ) -> None:
        if comp.generators[gen_index].is_async:
            self._compile_async_comprehension(
                comp, gen_index, depth, elt, val, type, outermost_gen_is_param
            )
        else:
            self._compile_sync_comprehension(
                comp, gen_index, depth, elt, val, type, outermost_gen_is_param
            )

    def _compile_async_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[CompNode],
        outermost_gen_is_param: bool,
    ) -> None:
        start = self.newBlock("start")
        except_ = self.newBlock("except")
        if_cleanup = self.newBlock("if_cleanup")

        gen = comp.generators[gen_index]
        if gen_index == 0 and outermost_gen_is_param:
            self.loadName(".0")
        else:
            self.visit(gen.iter)
            self.emit("GET_AITER")

        self.nextBlock(start)
        self.emit("SETUP_FINALLY", except_)
        self.emit("GET_ANEXT")
        self.emit("LOAD_CONST", None)
        self.emit_yield_from(await_=True)
        self.emit("POP_BLOCK")
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        depth += 1
        gen_index += 1
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, False
            )
        elif type is ast.GeneratorExp:
            self.visit(elt)
            self.emit_yield(self.scopes[comp])
            self.emit("POP_TOP")
        elif type is ast.ListComp:
            self.visit(elt)
            self.emit("LIST_APPEND", depth + 1)
        elif type is ast.SetComp:
            self.visit(elt)
            self.emit("SET_ADD", depth + 1)
        elif type is ast.DictComp:
            assert val is not None
            self.compile_dictcomp_element(elt, val)
            self.emit("MAP_ADD", depth + 1)
        else:
            raise NotImplementedError("unknown comprehension type")

        self.nextBlock(if_cleanup)
        self.emitJump(start)

        self.nextBlock(except_)
        self.emit("END_ASYNC_FOR")

    def _compile_sync_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[CompNode],
        outermost_gen_is_param: bool,
    ) -> None:
        start = self.newBlock("start")
        skip = self.newBlock("skip")
        if_cleanup = self.newBlock("if_cleanup")
        anchor = self.newBlock("anchor")

        gen = comp.generators[gen_index]
        if gen_index == 0 and outermost_gen_is_param:
            self.loadName(".0")
        else:
            if isinstance(gen.iter, (ast.Tuple, ast.List)):
                elts = gen.iter.elts
                if len(elts) == 1 and not isinstance(elts[0], ast.Starred):
                    self.visit(elts[0])
                    start = None
            if start:
                self.compile_comprehension_iter(gen)

        if start:
            depth += 1
            self.nextBlock(start)
            self.emit("FOR_ITER", anchor)
            self.nextBlock()
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        gen_index += 1
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, False
            )
        else:
            if type is ast.GeneratorExp:
                self.visit(elt)
                self.emit_yield(self.scopes[comp])
                self.emit("POP_TOP")
            elif type is ast.ListComp:
                self.visit(elt)
                self.emit("LIST_APPEND", depth + 1)
            elif type is ast.SetComp:
                self.visit(elt)
                self.emit("SET_ADD", depth + 1)
            elif type is ast.DictComp:
                assert val is not None
                self.compile_dictcomp_element(elt, val)
                self.emit("MAP_ADD", depth + 1)
            else:
                raise NotImplementedError("unknown comprehension type")

            self.nextBlock(skip)
        self.nextBlock(if_cleanup)
        if start:
            self.emitJump(start)
            self.nextBlock(anchor)
            self.emit_end_for()

    # Function calls --------------------------------------------------

    def emit_prepare_call(self) -> None:
        pass

    def emit_call(self, nargs: int) -> None:
        self.emit("CALL_FUNCTION", nargs)

    def emit_call_kw(self, nargs: int, kwargs: tuple[str, ...]) -> None:
        self.emit("LOAD_CONST", kwargs)
        self.emit("CALL_FUNCTION_KW", nargs + len(kwargs))

    def emit_call_exit_with_nones(self) -> None:
        self.emit("LOAD_CONST", None)
        self.emit_dup()
        self.emit_dup()
        self.emit("CALL_FUNCTION", 3)

    def _fastcall_helper(
        self,
        argcnt: int,
        node: ast.expr | None,
        args: list[ast.expr],
        kwargs: list[ast.keyword],
    ) -> None:
        # No * or ** args, faster calling sequence.
        for arg in args:
            self.visit(arg)
        if len(kwargs) > 0:
            self.visit_list(kwargs)
            self.emit("LOAD_CONST", tuple(arg.arg for arg in kwargs))
            self.emit("CALL_FUNCTION_KW", argcnt + len(args) + len(kwargs))
            return
        self.emit("CALL_FUNCTION", argcnt + len(args))

    def _can_optimize_call(self, node: ast.Call) -> bool:
        return (
            isinstance(node.func, ast.Attribute)
            and isinstance(node.func.ctx, ast.Load)
            and not node.keywords
            and not any(isinstance(arg, ast.Starred) for arg in node.args)
            and len(node.args) < STACK_USE_GUIDELINE
        )

    def visitCall(self, node: ast.Call) -> None:
        if not self._can_optimize_call(node):
            self.visit(node.func)
            self._call_helper(0, node, node.args, node.keywords)
            return

        attr = node.func
        assert isinstance(attr, ast.Attribute)
        self.visit(attr.value)
        with self.temp_lineno(attr.end_lineno or -1):
            self.emit("LOAD_METHOD", self.mangle(attr.attr))
            for arg in node.args:
                self.visit(arg)
            nargs = len(node.args)
            self.emit("CALL_METHOD", nargs)

    # Exceptions and with --------------------------------------------------

    def emit_try_except(self, node: ast.Try) -> None:
        body = self.newBlock("try_body")
        except_ = self.newBlock("try_handlers")
        orElse = self.newBlock("try_else")
        end = self.newBlock("try_end")

        self.emit("SETUP_FINALLY", except_)
        self.nextBlock(body)

        self.push_fblock(Entry(TRY_EXCEPT, body, None, None))
        self.visitStatements(node.body)
        self.pop_fblock(TRY_EXCEPT)
        self.set_no_pos()
        self.emit("POP_BLOCK")
        self.emit_jump_forward(orElse)
        self.nextBlock(except_)
        self.push_fblock(Entry(EXCEPTION_HANDLER, None, None, None))

        last = len(node.handlers) - 1
        for i in range(len(node.handlers)):
            handler = node.handlers[i]
            expr = handler.type
            target = handler.name
            body = handler.body
            self.set_pos(handler)
            except_ = self.newBlock(f"try_except_{i}")
            if expr:
                self.emit_dup()
                self.visit(expr)
                self.emit("JUMP_IF_NOT_EXC_MATCH", except_)
                self.nextBlock()
            elif i < last:
                raise self.syntax_error("default 'except:' must be last", handler)
            else:
                self.set_pos(handler)
            self.emit("POP_TOP")
            if target:
                cleanup_end = self.newBlock(f"try_cleanup_end{i}")
                cleanup_body = self.newBlock(f"try_cleanup_body{i}")

                self.storeName(target)
                self.emit("POP_TOP")

                self.emit("SETUP_FINALLY", cleanup_end)
                self.nextBlock(cleanup_body)
                self.push_fblock(
                    Entry(HANDLER_CLEANUP, cleanup_body, cleanup_end, target)
                )
                self.visit_list(body)
                self.pop_fblock(HANDLER_CLEANUP)
                self.set_no_pos()
                self.emit("POP_BLOCK")
                self.emit("POP_EXCEPT")

                self.emit("LOAD_CONST", None)
                self.storeName(target)
                self.delName(target)
                self.emit_jump_forward(end)

                self.nextBlock(cleanup_end)
                self.set_no_pos()
                self.emit("LOAD_CONST", None)
                self.storeName(target)
                self.delName(target)

                self.emit("RERAISE", 1)

            else:
                cleanup_body = self.newBlock(f"try_cleanup_body{i}")
                self.emit("POP_TOP")
                self.emit("POP_TOP")
                self.nextBlock(cleanup_body)
                self.push_fblock(Entry(HANDLER_CLEANUP, cleanup_body, None, None))
                self.visit_list(body)
                self.pop_fblock(HANDLER_CLEANUP)
                self.set_no_pos()
                self.emit("POP_EXCEPT")
                self.emit_jump_forward(end)
            self.nextBlock(except_)

        self.pop_fblock(EXCEPTION_HANDLER)
        self.set_no_pos()
        self.emit("RERAISE", 0)
        self.nextBlock(orElse)
        self.visitStatements(node.orelse)
        self.nextBlock(end)

    def emit_try_finally(
        self,
        node: ast.Try | None,
        try_body: Callable[[], None] | None = None,
        final_body: Callable[[], None] | None = None,
    ) -> None:
        """
        The overall idea is:
           SETUP_FINALLY end
           try-body
           POP_BLOCK
           finally-body
           JUMP exit
        end:
           finally-body
        exit:
        """
        body = self.newBlock("try_finally_body")
        end = self.newBlock("try_finally_end")
        exit_ = self.newBlock("try_finally_exit")

        if final_body is None:
            assert node
            final_body = lambda: self.visitStatements(node.finalbody)

        # try block
        self.emit("SETUP_FINALLY", end)

        self.nextBlock(body)
        self.push_fblock(Entry(FINALLY_TRY, body, end, final_body))
        if try_body is not None:
            try_body()
        else:
            assert node
            if node.handlers:
                self.emit_try_except(node)
            else:
                self.visitStatements(node.body)

        self.emit_noline("POP_BLOCK")
        self.pop_fblock(FINALLY_TRY)
        final_body()
        self.emit_jump_forward_noline(exit_)

        # finally block
        self.nextBlock(end)
        self.push_fblock(Entry(FINALLY_END, end, None, None))
        final_body()
        self.pop_fblock(FINALLY_END)
        self.emit("RERAISE", 0)

        self.nextBlock(exit_)

    def emit_with_except_cleanup(self, cleanup: Block) -> None:
        self.emit("WITH_EXCEPT_START")

    def emit_with_except_finish(self, cleanup: Block) -> None:
        except_ = self.newBlock()
        self.emit("POP_JUMP_IF_TRUE", except_)
        self.nextBlock()
        self.emit("RERAISE", 1)

        self.nextBlock(except_)
        self.emit("POP_TOP")
        self.emit("POP_TOP")
        self.emit("POP_TOP")
        self.emit("POP_EXCEPT")
        self.emit("POP_TOP")

    def set_with_position_for_exit(
        self, node: ast.With | ast.AsyncWith, kind: int
    ) -> None:
        if kind == WITH:
            self.set_no_pos()

    def emit_setup_with(self, node: ast.withitem, target: Block, async_: bool) -> None:
        self.emit("SETUP_ASYNC_WITH" if async_ else "SETUP_WITH", target)

    # Operators and augmented assignment --------------------------------------------------

    def emitAugRHS(self, node: ast.AugAssign) -> None:
        with self.temp_lineno(node.lineno):
            self.visit(node.value)
            self.emit(self._augmented_opcode[type(node.op)])

    def emitAugAttribute(self, node: ast.AugAssign) -> None:
        target = node.target
        assert isinstance(target, ast.Attribute)
        self.visit(target.value)
        self.emit_dup()
        with self.temp_lineno(node.target.end_lineno or -1):
            self.emit("LOAD_ATTR", self.mangle(target.attr))
        self.emitAugRHS(node)
        self.graph.set_pos(
            SrcLocation(
                node.target.end_lineno or -1, node.target.end_lineno or -1, -1, -1
            )
        )
        self.emit_rotate_stack(2)
        self.emit("STORE_ATTR", self.mangle(target.attr))

    def unaryOp(self, node: ast.UnaryOp, op: str) -> None:
        self.visit(node.operand)
        self.emit(op)

    _binary_opcode: dict[type[ast.operator], str] = {
        ast.Add: "BINARY_ADD",
        ast.Sub: "BINARY_SUBTRACT",
        ast.Mult: "BINARY_MULTIPLY",
        ast.MatMult: "BINARY_MATRIX_MULTIPLY",
        ast.Div: "BINARY_TRUE_DIVIDE",
        ast.FloorDiv: "BINARY_FLOOR_DIVIDE",
        ast.Mod: "BINARY_MODULO",
        ast.Pow: "BINARY_POWER",
        ast.LShift: "BINARY_LSHIFT",
        ast.RShift: "BINARY_RSHIFT",
        ast.BitOr: "BINARY_OR",
        ast.BitXor: "BINARY_XOR",
        ast.BitAnd: "BINARY_AND",
    }

    def emit_binary_subscr(self) -> None:
        self.emit("BINARY_SUBSCR")

    def emit_bin_op(self, binop: type[ast.operator]) -> None:
        op = self._binary_opcode[binop]
        self.emit(op)

    # Misc --------------------------------------------------

    def emitJump(self, target: Block) -> None:
        self.emit("JUMP_ABSOLUTE", target)

    def emit_jump_forward(self, target: Block) -> None:
        self.emit("JUMP_FORWARD", target)

    def emit_jump_forward_noline(self, target: Block) -> None:
        self.emit_noline("JUMP_FORWARD", target)

    def emit_print(self) -> None:
        self.emit("PRINT_EXPR")

    def emit_yield(self, scope: Scope) -> None:
        self.emit("YIELD_VALUE")

    def emit_yield_from(self, await_: bool = False) -> None:
        self.emit("YIELD_FROM")

    def _visitSequenceLoad(
        self,
        elts: list[ast.expr],
        build_op: str,
        add_op: str,
        extend_op: str,
        num_pushed: int = 0,
        is_tuple: bool = False,
    ) -> None:
        if len(elts) > 2 and all(isinstance(elt, ast.Constant) for elt in elts):
            elts_tuple = tuple(
                elt.value for elt in elts if isinstance(elt, ast.Constant)
            )
            if is_tuple:
                self.emit("LOAD_CONST", elts_tuple)
            else:
                if add_op == "SET_ADD":
                    elts_tuple = frozenset(elts_tuple)
                self.emit(build_op, num_pushed)
                self.emit("LOAD_CONST", elts_tuple)
                self.emit(extend_op, 1)
            return

        big = (len(elts) + num_pushed) > STACK_USE_GUIDELINE
        starred_load = self.hasStarred(elts)
        if not starred_load and not big:
            for elt in elts:
                self.visit(elt)
            collection_size = num_pushed + len(elts)
            self.emit("BUILD_TUPLE" if is_tuple else build_op, collection_size)
            return

        sequence_built = False
        if big:
            self.emit(build_op, num_pushed)
            sequence_built = True
        for on_stack, elt in enumerate(elts):
            if isinstance(elt, ast.Starred):
                if not sequence_built:
                    self.emit(build_op, on_stack + num_pushed)
                    sequence_built = True
                self.visit(elt.value)
                self.emit(extend_op, 1)
            else:
                self.visit(elt)
                if sequence_built:
                    self.emit(add_op, 1)

        if is_tuple:
            self.emit("LIST_TO_TUPLE")

    def unwind_setup_entry(self, e: Entry, preserve_tos: int) -> None:
        if e.kind in (
            WHILE_LOOP,
            EXCEPTION_HANDLER,
            ASYNC_COMPREHENSION_GENERATOR,
            STOP_ITERATION,
        ):
            return

        elif e.kind == FOR_LOOP or e.kind == ASYNC_FOR_LOOP:
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit("POP_TOP")

        elif e.kind == TRY_EXCEPT:
            self.emit("POP_BLOCK")

        elif e.kind == FINALLY_TRY:
            self.emit("POP_BLOCK")
            if preserve_tos:
                self.push_fblock(Entry(POP_VALUE, None, None, None))
            assert callable(e.unwinding_datum)
            e.unwinding_datum()
            if preserve_tos:
                self.pop_fblock(POP_VALUE)
            self.set_no_pos()

        elif e.kind == FINALLY_END:
            if preserve_tos:
                self.emit_rotate_stack(4)
            self.emit("POP_TOP")
            self.emit("POP_TOP")
            self.emit("POP_TOP")
            if preserve_tos:
                self.emit_rotate_stack(4)
            self.emit("POP_EXCEPT")

        elif e.kind in (WITH, ASYNC_WITH):
            assert isinstance(e.unwinding_datum, AST)
            self.set_pos(e.unwinding_datum)
            self.emit("POP_BLOCK")
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit_call_exit_with_nones()
            if e.kind == ASYNC_WITH:
                self.emit_get_awaitable(AwaitableKind.AsyncExit)
                self.emit("LOAD_CONST", None)
                self.emit_yield_from(await_=True)
            self.emit("POP_TOP")
            self.set_no_pos()

        elif e.kind == HANDLER_CLEANUP:
            datum = e.unwinding_datum
            if datum is not None:
                self.emit("POP_BLOCK")
            if preserve_tos:
                self.emit_rotate_stack(4)
            self.emit("POP_EXCEPT")
            if datum is not None:
                self.emit("LOAD_CONST", None)
                assert isinstance(datum, str)
                self.storeName(datum)
                self.delName(datum)

        elif e.kind == POP_VALUE:
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit("POP_TOP")

        else:
            raise Exception(f"Unexpected kind {e.kind}")

    def visitJoinedStr(self, node: ast.JoinedStr) -> None:
        if len(node.values) > STACK_USE_GUIDELINE:
            self.emit("LOAD_CONST", "")
            self.emit("LOAD_METHOD", "join")
            self.emit("BUILD_LIST")
            for value in node.values:
                self.visit(value)
                self.emit("LIST_APPEND", 1)
            self.emit("CALL_METHOD", 1)
        else:
            for value in node.values:
                self.visit(value)
            if len(node.values) != 1:
                self.emit("BUILD_STRING", len(node.values))

    def visitBoolOp(self, node: ast.BoolOp) -> None:
        self.emit_test(node, type(node.op) is ast.Or)

    def emit_rot_n(self, n: int) -> None:
        self.emit("ROT_N", n)

    def emitAugSubscript(self, node: ast.AugAssign) -> None:
        assert isinstance(node.target, ast.Subscript)
        self.visitSubscript(node.target, True)
        self.emitAugRHS(node)
        self.emit_rotate_stack(3)
        self.emit("STORE_SUBSCR")

    def visitSubscript(self, node: ast.Subscript, aug_flag: bool = False) -> None:
        self.visit(node.value)
        self.visit(node.slice)
        if isinstance(node.ctx, ast.Load):
            self.emit_binary_subscr()
        elif isinstance(node.ctx, ast.Store):
            if aug_flag:
                self.emit_dup(2)
                self.emit_binary_subscr()
            else:
                self.emit("STORE_SUBSCR")
        elif isinstance(node.ctx, ast.Del):
            self.emit("DELETE_SUBSCR")
        else:
            assert 0

    def emitChainedCompareStep(
        self, op: ast.cmpop, value: ast.expr, cleanup: Block, always_pop: bool = False
    ) -> None:
        self.visit(value)
        self.emit_dup()
        self.emit_rotate_stack(3)
        self.defaultEmitCompare(op)
        self.emit(
            "POP_JUMP_IF_FALSE" if always_pop else "JUMP_IF_FALSE_OR_POP", cleanup
        )
        self.nextBlock(label="compare_or_cleanup")

    def visitCompare(self, node: ast.Compare) -> None:
        self.visit(node.left)
        cleanup = self.newBlock("cleanup")
        for op, code in zip(node.ops[:-1], node.comparators[:-1]):
            self.emitChainedCompareStep(op, code, cleanup)
        # now do the last comparison
        if node.ops:
            op = node.ops[-1]
            code = node.comparators[-1]
            self.visit(code)
            self.defaultEmitCompare(op)
        if len(node.ops) > 1:
            end = self.newBlock("end")
            self.emit_jump_forward(end)
            self.nextBlock(cleanup)
            self.emit_rotate_stack(2)
            self.emit("POP_TOP")
            self.nextBlock(end)

    def emit_compile_jump_if_compare(
        self, test: ast.Compare, next: Block, is_if_true: bool
    ) -> None:
        cleanup = self.newBlock()
        self.visit(test.left)
        for op, comparator in zip(test.ops[:-1], test.comparators[:-1]):
            self.emitChainedCompareStep(op, comparator, cleanup, always_pop=True)
        self.visit(test.comparators[-1])
        self.defaultEmitCompare(test.ops[-1])
        self.emit("POP_JUMP_IF_TRUE" if is_if_true else "POP_JUMP_IF_FALSE", next)
        self.nextBlock()
        end = self.newBlock()
        self.emit_jump_forward_noline(end)
        self.nextBlock(cleanup)
        self.emit("POP_TOP")
        if not is_if_true:
            self.emit_jump_forward_noline(next)
        self.nextBlock(end)

    def emit_finish_jump_if(
        self, test: ast.expr, next: Block, is_if_true: bool
    ) -> None:
        self.visit(test)
        self.emit("POP_JUMP_IF_TRUE" if is_if_true else "POP_JUMP_IF_FALSE", next)

    def emit_import_star(self) -> None:
        self.emit("IMPORT_STAR")

    def emit_match_jump_to_end(self, end: Block) -> None:
        self.emit_jump_forward(end)

    def emit_match_jump_to_fail_pop_unconditional(self, pc: PatternContext) -> None:
        self._jump_to_fail_pop(pc, "JUMP_FORWARD")

    def emit_finish_match_class(self, node: ast.MatchClass, pc: PatternContext) -> None:
        # TOS is now a tuple of (nargs + nattrs) attributes. Preserve it:
        pc.on_top += 1
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        patterns = node.patterns
        kwd_patterns = node.kwd_patterns
        nargs = len(patterns)
        nattrs = len(node.kwd_attrs)
        for i in range(nargs + nattrs):
            if i < nargs:
                pattern = patterns[i]
            else:
                pattern = kwd_patterns[i - nargs]
            if self._wildcard_check(pattern):
                continue
            self.emit_dup()
            self.emit("LOAD_CONST", i)
            self.emit_binary_subscr()
            self._visit_subpattern(pattern, pc)
        # Success! Pop the tuple of attributes:
        pc.on_top -= 1
        self.emit("POP_TOP")

    def emit_finish_match_mapping(
        self,
        node: ast.MatchMapping,
        pc: PatternContext,
        star_target: str | None,
        size: int,
    ) -> None:
        self.emit("BUILD_TUPLE", size)
        self.emit("MATCH_KEYS")
        # There's now a tuple of keys and a tuple of values on top of the subject:
        pc.on_top += 2
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        # So far so good. Use that tuple of values on the stack to match
        # sub-patterns against:
        for i, pattern in enumerate(node.patterns):
            if self._wildcard_check(pattern):
                continue
            self.emit_dup()
            self.emit("LOAD_CONST", i)
            self.emit_binary_subscr()
            self._visit_subpattern(pattern, pc)

        # If we get this far, it's a match! We're done with the tuple of values,
        # and whatever happens next should consume the tuple of keys underneath it:
        pc.on_top -= 2
        self.emit("POP_TOP")
        if star_target:
            # If we have a starred name, bind a dict of remaining items to it:
            self.emit("COPY_DICT_WITHOUT_KEYS")
            self._pattern_helper_store_name(star_target, pc, node)
        else:
            # Otherwise, we don't care about this tuple of keys anymore:
            self.emit("POP_TOP")
        # Pop the subject:
        pc.on_top -= 1
        self.emit("POP_TOP")


class CodeGenerator312(CodeGenerator):
    flow_graph: type[PyFlowGraph] = PyFlowGraph312
    _SymbolVisitor = SymbolVisitor312
    # pyre-fixme[8]: Attribute has type `Tuple[Type[AST]]`; used as
    #  `Tuple[Type[ClassDef], Type[TypeVar]]`.
    unqualified_asts: tuple[type[ast.AST]] = (
        ast.ClassDef,
        # pyre-ignore[16]: No such attribute
        getattr(ast, "TypeVar", None),
    )

    def __init__(
        self,
        parent: CodeGenerator | None,
        node: AST,
        symbols: BaseSymbolVisitor,
        graph: PyFlowGraph,
        flags: int = 0,
        optimization_lvl: int = 0,
        future_flags: int | None = None,
        name: str | None = None,
    ) -> None:
        super().__init__(
            parent, node, symbols, graph, flags, optimization_lvl, future_flags, name
        )
        self.fast_hidden: set[str] = set()
        # Tracks symbols whose scopes are overridden when inlining comprehensions
        self.temp_symbols: dict[str, int] | None = None
        self.inlined_comp_depth = 0
        if parent is None:
            self.set_pos(SrcLocation(0, 1, 0, 0))
        self.emit_prologue()

    def emit_prologue(self) -> None:
        self.emit_resume(ResumeOparg.ScopeEntry)

    def check_name(self, name: str) -> int:
        if (temp_symbols := self.temp_symbols) is not None:
            if result := temp_symbols.get(name):
                return result

        return super().check_name(name)

    def emit_import_name(self, name: str) -> None:
        # If we're not on meta-Python we don't EAGER_IMPORT_NAME so only
        # emit it when it's available.
        if "EAGER_IMPORT_NAME" not in opcode.opmap or (
            isinstance(self.scope, ModuleScope) and not self.setups
        ):
            self.emit("IMPORT_NAME", name)
        else:
            self.emit("EAGER_IMPORT_NAME", name)

    @classmethod
    def optimize_tree(
        cls,
        optimize: int,
        tree: AST,
        string_anns: bool,
    ) -> AST:
        result = AstOptimizer312(optimize=optimize > 0, string_anns=string_anns).visit(
            tree
        )

        assert isinstance(result, AST)
        return result

    def emit_resume(self, oparg: ResumeOparg) -> None:
        self.emit("RESUME", int(oparg))

    def emit_get_awaitable(self, kind: AwaitableKind) -> None:
        self.emit("GET_AWAITABLE", int(kind))

    def visitAssert(self, node: ast.Assert) -> None:
        if not self.optimization_lvl:
            end = self.newBlock()
            self.compileJumpIf(node.test, end, True)

            self.emit_load_assertion_error(node)
            if node.msg:
                self.visit(node.msg)
                self.set_pos(node)
                self.emit_call_one_arg()
                self.graph.emit_with_loc("RAISE_VARARGS", 1, node.test)
            else:
                self.set_pos(node.test)
                self.emit("RAISE_VARARGS", 1)
            self.nextBlock(end)

    def visitBoolOp(self, node: ast.BoolOp) -> None:
        if type(node.op) is ast.And:
            jumpi = "POP_JUMP_IF_FALSE"
        else:
            jumpi = "POP_JUMP_IF_TRUE"
        end = self.newBlock()
        for value in node.values[:-1]:
            self.visit(value)
            self.emit("COPY", 1)
            self.emit_cond_jump(jumpi, end)
            self.nextBlock(label="cond")
            self.emit("POP_TOP")
        self.visit(node.values[-1])
        self.nextBlock(end)

    def is_import_originated(self, e: ast.expr) -> bool:
        # Check whether the global scope has an import named
        # e, if it is a Name object. For not traversing all the
        # scope stack every time this function is called, it will
        # only check the global scope to determine whether something
        # is imported or not.
        if not isinstance(e, ast.Name):
            return False

        scope = self.scope
        while scope.parent is not None:
            scope = scope.parent
        if self.temp_symbols is not None and e.id in self.temp_symbols:
            return False
        return scope.is_import(e.id)

    def _can_optimize_call(self, node: ast.Call) -> bool:
        if not (
            isinstance(node.func, ast.Attribute)
            and isinstance(node.func.ctx, ast.Load)
            and not any(isinstance(arg, ast.Starred) for arg in node.args)
            and not any(arg.arg is None for arg in node.keywords)
            and len(node.args) + len(node.keywords) < STACK_USE_GUIDELINE
        ):
            return False

        assert isinstance(node.func, ast.Attribute)
        return not self.is_import_originated(node.func.value)

    def emit_super_call(self, attr: ast.Attribute) -> None:
        call = attr.value
        assert isinstance(call, ast.Call)
        self.visit(call.func)
        self.set_pos(call)
        load_arg, is_zero = self._emit_args_for_super(call, attr.attr)
        op = "LOAD_ZERO_SUPER_METHOD" if is_zero else "LOAD_SUPER_METHOD"
        loc = self.compute_start_location_to_match_attr(attr, attr)
        self.graph.emit_with_loc("LOAD_SUPER_ATTR", (op, load_arg, is_zero), attr)
        self.graph.emit_with_loc("NOP", 0, loc)

    def is_super_shadowed(self) -> bool:
        return (
            super().is_super_shadowed()
            or "super" in self.module_gen.scope.uses
            or "super" in self.module_gen.scope.defs
        )

    def visitCall(self, node: ast.Call) -> None:
        if not self._can_optimize_call(node):
            # We cannot optimize this call
            return self.emit_noopt_call(node)

        attr = node.func
        assert isinstance(attr, ast.Attribute)
        loc = self.compute_start_location_to_match_attr(node.func, attr)

        if self._is_super_call(attr.value):
            self.emit_super_call(attr)
        else:
            self.visit(attr.value)
            loc = self.compute_start_location_to_match_attr(node.func, attr)
            self.graph.emit_with_loc("LOAD_ATTR", (self.mangle(attr.attr), 1), loc)

        for arg in node.args:
            self.visit(arg)

        self.finish_opt_call(node, attr)

    def emit_noopt_call(self, node: ast.Call) -> None:
        self.graph.emit_with_loc("PUSH_NULL", 0, node.func)
        self.visit(node.func)
        self._call_helper(0, node, node.args, node.keywords)

    def finish_opt_call(self, node: ast.Call, attr: ast.Attribute) -> None:
        loc = self.compute_start_location_to_match_attr(node.func, attr)
        if node.keywords:
            for arg in node.keywords:
                self.visit(arg)
            self.set_pos(loc)
            self.emit("KW_NAMES", tuple(arg.arg for arg in node.keywords))

        loc = self.compute_start_location_to_match_attr(node, attr)
        self.graph.emit_with_loc("CALL", len(node.args) + len(node.keywords), loc)

    def visitJoinedStr(self, node: ast.JoinedStr) -> None:
        if len(node.values) > STACK_USE_GUIDELINE:
            self.emit("LOAD_CONST", "")
            self.emit("LOAD_ATTR", ("join", 1))
            self.emit("BUILD_LIST")
            for value in node.values:
                self.visit(value)
                self.emit("LIST_APPEND", 1)
            self.emit("CALL", 1)
        else:
            for value in node.values:
                self.visit(value)
            if len(node.values) != 1:
                self.emit("BUILD_STRING", len(node.values))

    def emitChainedCompareStep(
        self, op: ast.cmpop, value: ast.expr, cleanup: Block, always_pop: bool = False
    ) -> None:
        self.visit(value)
        self.emit("SWAP", 2)
        self.emit("COPY", 2)
        self.defaultEmitCompare(op)

    def visitCompare(self, node: ast.Compare) -> None:
        self.visit(node.left)
        if len(node.comparators) == 1:
            self.visit(node.comparators[0])
            self.defaultEmitCompare(node.ops[0])
            return

        cleanup = self.newBlock("cleanup")
        for i in range(len(node.comparators) - 1):
            self.set_pos(node)
            self.emitChainedCompareStep(
                node.ops[i], node.comparators[i], cleanup, False
            )
            self.emit("COPY", 1)
            self.emit_cond_jump("POP_JUMP_IF_FALSE", cleanup)
            self.nextBlock(label="compare_or_cleanup")
            self.emit("POP_TOP")

        self.visit(node.comparators[-1])
        self.defaultEmitCompare(node.ops[-1])
        end = self.newBlock("end")
        self.emit_jump_forward(end)

        self.nextBlock(cleanup)
        self.emit("SWAP", 2)
        self.emit("POP_TOP")
        self.nextBlock(end)

    def emit_compile_jump_if_compare(
        self, test: ast.Compare, next: Block, is_if_true: bool
    ) -> None:
        cleanup = self.newBlock()
        self.visit(test.left)
        for op, comparator in zip(test.ops[:-1], test.comparators[:-1]):
            self.set_pos(test)
            self.emitChainedCompareStep(op, comparator, cleanup, always_pop=True)
            self.emit_cond_jump("POP_JUMP_IF_FALSE", cleanup)
            self.nextBlock(label="compare_or_cleanup")
        self.visit(test.comparators[-1])
        self.defaultEmitCompare(test.ops[-1])
        self.emit_cond_jump(
            "POP_JUMP_IF_TRUE" if is_if_true else "POP_JUMP_IF_FALSE", next
        )
        self.nextBlock()
        end = self.newBlock()
        self.emit_jump_forward_noline(end)
        self.nextBlock(cleanup)
        self.emit("POP_TOP")
        if not is_if_true:
            self.emit_jump_forward_noline(next)
        self.nextBlock(end)

    def _visitAnnotation(self, node: ast.expr) -> None:
        if self.future_flags & CO_FUTURE_ANNOTATIONS:
            self.set_pos(node)
            self.emit("LOAD_CONST", to_expr(node))
        elif isinstance(node, ast.Starred):
            self.visit(node.value)
            self.emit("UNPACK_SEQUENCE", 1)
        else:
            self.visit(node)

    def emit_store_annotation(self, name: str, node: ast.AnnAssign) -> None:
        assert self.did_setup_annotations

        self._visitAnnotation(node.annotation)
        self.set_pos(node)
        self.emit("LOAD_NAME", "__annotations__")
        mangled = self.mangle(name)
        self.emit("LOAD_CONST", mangled)
        self.emit("STORE_SUBSCR")

    def emit_finish_jump_if(
        self, test: ast.expr, next: Block, is_if_true: bool
    ) -> None:
        self.visit(test)
        self.set_pos(test)
        self.emit_cond_jump(
            "POP_JUMP_IF_TRUE" if is_if_true else "POP_JUMP_IF_FALSE", next
        )

    def visitFor(self, node: ast.For) -> None:
        start = self.newBlock("for_start")
        body = self.newBlock("for_body")
        cleanup = self.newBlock("for_cleanup")
        end = self.newBlock("for_end")

        self.push_loop(FOR_LOOP, start, end)
        self.visit(node.iter)
        if IS_3_12_8:
            self.set_pos(node.iter)
        self.emit("GET_ITER")

        self.nextBlock(start)
        self.emit("FOR_ITER", cleanup)
        if IS_3_12_8:
            self.graph.emit_with_loc("NOP", 0, node.target)
        self.nextBlock(body)
        self.visit(node.target)
        self.visitStatements(node.body)
        self.set_no_pos()
        self.emitJump(start)
        self.nextBlock(cleanup)
        self.emit_end_for()
        self.pop_loop()

        if node.orelse:
            self.visitStatements(node.orelse)
        self.nextBlock(end)

    def emit_slice_components(self, node: ast.Slice) -> None:
        assert node.step is None
        if node.lower:
            self.visit(node.lower)
        else:
            self.graph.emit_with_loc("LOAD_CONST", None, node)
        if node.upper:
            self.visit(node.upper)
        else:
            self.graph.emit_with_loc("LOAD_CONST", None, node)

    def emitAugSubscript(self, node: ast.AugAssign) -> None:
        subs = node.target
        assert isinstance(subs, ast.Subscript)
        self.visit(subs.value)

        if is_simple_slice := (
            isinstance(subs.slice, ast.Slice)
            and self.should_apply_two_element_slice_optimization(subs.slice)
        ):
            assert isinstance(subs.slice, ast.Slice)
            self.emit_slice_components(subs.slice)
            self.emit("COPY", 3)
            self.emit("COPY", 3)
            self.emit("COPY", 3)
            self.emit("BINARY_SLICE")
        else:
            self.visit(subs.slice)
            self.emit("COPY", 2)
            self.emit("COPY", 2)
            self.emit_binary_subscr()

        self.emitAugRHS(node)

        if is_simple_slice:
            self.emit("SWAP", 4)
            self.emit("SWAP", 3)
            self.emit("SWAP", 2)
            self.emit("STORE_SLICE")
        else:
            self.emit("SWAP", 3)
            self.emit("SWAP", 2)
            self.emit("STORE_SUBSCR")

    def should_apply_two_element_slice_optimization(self, node: ast.Slice) -> bool:
        return node.step is None

    def visitSubscript(self, node: ast.Subscript, aug_flag: bool = False) -> None:
        assert not aug_flag  # not used in the 3.12 compiler
        self.visit(node.value)
        if (
            isinstance(node.slice, ast.Slice)
            and self.should_apply_two_element_slice_optimization(node.slice)
            and not isinstance(node.ctx, ast.Del)
        ):
            self.emit_slice_components(node.slice)
            if isinstance(node.ctx, ast.Load):
                self.emit("BINARY_SLICE")
            else:
                self.emit("STORE_SLICE")
            return

        self.visit(node.slice)

        if isinstance(node.ctx, ast.Load):
            self.emit_binary_subscr()
        elif isinstance(node.ctx, ast.Store):
            self.emit("STORE_SUBSCR")
        elif isinstance(node.ctx, ast.Del):
            self.emit("DELETE_SUBSCR")
        else:
            assert 0

    def emit_call_intrinsic_1(self, oparg: str) -> None:
        # pyre-fixme[16]: Module `opcodes` has no attribute `INTRINSIC_1`.
        self.emit("CALL_INTRINSIC_1", INTRINSIC_1.index(oparg))

    def emit_call_intrinsic_2(self, oparg: str) -> None:
        # pyre-fixme[16]: Module `opcodes` has no attribute `INTRINSIC_2`.
        self.emit("CALL_INTRINSIC_2", INTRINSIC_2.index(oparg))

    def emit_import_star(self) -> None:
        self.emit_call_intrinsic_1("INTRINSIC_IMPORT_STAR")
        self.emit("POP_TOP")

    def emit_yield(self, scope: Scope) -> None:
        if scope.generator and scope.coroutine:
            self.emit_call_intrinsic_1("INTRINSIC_ASYNC_GEN_WRAP")
        self.emit("YIELD_VALUE")
        self.emit_resume(ResumeOparg.Yield)

    _binary_opargs: dict[type[ast.operator], int] = {
        ast.Add: find_op_idx("NB_ADD"),
        ast.Sub: find_op_idx("NB_SUBTRACT"),
        ast.Mult: find_op_idx("NB_MULTIPLY"),
        ast.MatMult: find_op_idx("NB_MATRIX_MULTIPLY"),
        ast.Div: find_op_idx("NB_TRUE_DIVIDE"),
        ast.FloorDiv: find_op_idx("NB_FLOOR_DIVIDE"),
        ast.Mod: find_op_idx("NB_REMAINDER"),
        ast.Pow: find_op_idx("NB_POWER"),
        ast.LShift: find_op_idx("NB_LSHIFT"),
        ast.RShift: find_op_idx("NB_RSHIFT"),
        ast.BitOr: find_op_idx("NB_OR"),
        ast.BitXor: find_op_idx("NB_XOR"),
        ast.BitAnd: find_op_idx("NB_AND"),
    }

    def emit_binary_subscr(self) -> None:
        self.emit("BINARY_SUBSCR")

    def emit_bin_op(self, binop: type[ast.operator]) -> None:
        op = self._binary_opargs[binop]
        self.emit("BINARY_OP", op)

    # unary ops

    def unaryOp(self, node: ast.UnaryOp, op: str) -> None:
        self.visit(node.operand)
        if op == "UNARY_POSITIVE":
            self.emit_call_intrinsic_1("INTRINSIC_UNARY_POSITIVE")
        else:
            self.emit(op)

    _augmented_opargs: dict[type[ast.AST], int] = {
        ast.Add: find_op_idx("NB_INPLACE_ADD"),
        ast.Sub: find_op_idx("NB_INPLACE_SUBTRACT"),
        ast.Mult: find_op_idx("NB_INPLACE_MULTIPLY"),
        ast.MatMult: find_op_idx("NB_INPLACE_MATRIX_MULTIPLY"),
        ast.Div: find_op_idx("NB_INPLACE_TRUE_DIVIDE"),
        ast.FloorDiv: find_op_idx("NB_INPLACE_FLOOR_DIVIDE"),
        ast.Mod: find_op_idx("NB_INPLACE_REMAINDER"),
        ast.Pow: find_op_idx("NB_INPLACE_POWER"),
        ast.RShift: find_op_idx("NB_INPLACE_RSHIFT"),
        ast.LShift: find_op_idx("NB_INPLACE_LSHIFT"),
        ast.BitOr: find_op_idx("NB_INPLACE_OR"),
        ast.BitXor: find_op_idx("NB_INPLACE_XOR"),
        ast.BitAnd: find_op_idx("NB_INPLACE_AND"),
    }

    def emitAugRHS(self, node: ast.AugAssign) -> None:
        self.visit(node.value)
        op = self._augmented_opargs[type(node.op)]
        assert op != -1, node.op
        self.graph.emit_with_loc("BINARY_OP", op, node)

    def emitAugAttribute(self, node: ast.AugAssign) -> None:
        target = node.target
        assert isinstance(target, ast.Attribute)
        loc = self.compute_start_location_to_match_attr(target, target)
        self.visit(target.value)
        self.emit("COPY", 1)
        self.graph.emit_with_loc("LOAD_ATTR", self.mangle(target.attr), loc)
        self.emitAugRHS(node)
        self.set_pos(loc)
        self.emit_rotate_stack(2)
        self.emit("STORE_ATTR", self.mangle(target.attr))

    def emit_dup(self, count: int = 1) -> None:
        for _i in range(count):
            self.emit("COPY", count)

    def emit_rotate_stack(self, count: int) -> None:
        # 2 for 2, 3,2 for 3, 4,3,2 for 4.
        for i in range(count, 1, -1):
            self.emit("SWAP", i)

    def emit_print(self) -> None:
        self.emit_call_intrinsic_1("INTRINSIC_PRINT")
        self.set_no_pos()
        self.emit("POP_TOP")

    def emit_yield_value_for_yield_from(self) -> None:
        self.emit("YIELD_VALUE")

    def emit_yield_from(self, await_: bool = False) -> None:
        send = self.newBlock("send")
        fail = self.newBlock("fail")
        exit = self.newBlock("exit")
        post_send = self.newBlock("post send")

        self.nextBlock(send)
        self.emit("SEND", exit)
        self.nextBlock(post_send)

        # Setup try/except to handle StopIteration
        self.emit("SETUP_FINALLY", fail)
        self.emit_yield_value_for_yield_from()
        self.emit_noline("POP_BLOCK")
        self.emit_resume(ResumeOparg.Await if await_ else ResumeOparg.YieldFrom)
        self.emit("JUMP_NO_INTERRUPT", send)

        self.nextBlock(fail)
        self.emit("CLEANUP_THROW")

        self.nextBlock(exit)
        self.emit("END_SEND")

    def set_with_position_for_exit(
        self, node: ast.With | ast.AsyncWith, kind: int
    ) -> None:
        if kind == WITH:
            self.set_no_pos()
        else:
            self.set_pos(node)

    def emit_before_with(self) -> None:
        self.emit("BEFORE_WITH")

    def emit_setup_with(self, node: ast.withitem, target: Block, async_: bool) -> None:
        if not async_:
            if IS_3_12_8:
                self.set_pos(node.context_expr)
            self.emit_before_with()
        self.emit("SETUP_WITH", target)

    def emit_end_for(self) -> None:
        if IS_3_12_8:
            self.emit_noline("END_FOR")
        else:
            self.emit("END_FOR")

    def emit_with_except_cleanup(self, cleanup: Block) -> None:
        self.emit("SETUP_CLEANUP", cleanup)
        self.emit("PUSH_EXC_INFO")
        self.emit("WITH_EXCEPT_START")

    def emit_with_except_finish(self, cleanup: Block) -> None:
        assert cleanup is not None
        suppress = self.newBlock("suppress")
        self.emit_cond_jump("POP_JUMP_IF_TRUE", suppress)
        self.nextBlock()
        self.emit("RERAISE", 2)

        self.nextBlock(suppress)
        self.emit("POP_TOP")
        self.emit("POP_BLOCK")
        self.emit("POP_EXCEPT")
        self.emit("POP_TOP")
        self.emit("POP_TOP")

        exit = self.newBlock("exit")
        assert exit is not None
        self.emit_jump_forward(exit)

        self.nextBlock(cleanup)
        self.set_no_pos()
        self.emit("COPY", 3)
        self.emit("POP_EXCEPT")
        self.emit("RERAISE", 1)

        self.nextBlock(exit)

    def emit_rot_n(self, n: int) -> None:
        while 1 < n:
            self.emit("SWAP", n)
            n -= 1

    def _fastcall_helper(
        self,
        argcnt: int,
        node: ast.expr | None,
        args: list[ast.expr],
        kwargs: list[ast.keyword],
    ) -> None:
        # No * or ** args, faster calling sequence.
        for arg in args:
            self.visit(arg)
        if len(kwargs) > 0:
            self.visit_list(kwargs)
            self.emit("KW_NAMES", tuple(arg.arg for arg in kwargs))

        self.emit("CALL", argcnt + len(args) + len(kwargs))

    def emitJump(self, target: Block) -> None:
        self.emit_jump_forward(target)

    def emit_jump_forward(self, target: Block) -> None:
        self.emit("JUMP", target)

    def emit_jump_forward_noline(self, target: Block) -> None:
        self.emit_noline("JUMP", target)

    def _visitSequenceLoad(
        self,
        elts: list[ast.expr],
        build_op: str,
        add_op: str,
        extend_op: str,
        num_pushed: int = 0,
        is_tuple: bool = False,
    ) -> None:
        if len(elts) > 2 and all(isinstance(elt, ast.Constant) for elt in elts):
            elts_tuple = tuple(
                elt.value for elt in elts if isinstance(elt, ast.Constant)
            )
            if is_tuple and not num_pushed:
                self.emit("LOAD_CONST", elts_tuple)
            else:
                if add_op == "SET_ADD":
                    elts_tuple = frozenset(elts_tuple)
                self.emit(build_op, num_pushed)
                self.emit("LOAD_CONST", elts_tuple)
                self.emit(extend_op, 1)
                if is_tuple:
                    self.emit_call_intrinsic_1("INTRINSIC_LIST_TO_TUPLE")
            return

        return self._visitSequenceLoadNoOpt(
            elts, build_op, add_op, extend_op, num_pushed, is_tuple
        )

    def _visitSequenceLoadNoOpt(
        self,
        elts: list[ast.expr],
        build_op: str,
        add_op: str,
        extend_op: str,
        num_pushed: int = 0,
        is_tuple: bool = False,
    ) -> None:
        big = (len(elts) + num_pushed) > STACK_USE_GUIDELINE
        starred_load = self.hasStarred(elts)
        if not starred_load and not big:
            for elt in elts:
                self.visit(elt)
            collection_size = num_pushed + len(elts)
            self.emit("BUILD_TUPLE" if is_tuple else build_op, collection_size)
            return

        sequence_built = False
        if big:
            self.emit(build_op, num_pushed)
            sequence_built = True
        for on_stack, elt in enumerate(elts):
            if isinstance(elt, ast.Starred):
                if not sequence_built:
                    self.emit(build_op, on_stack + num_pushed)
                    sequence_built = True
                self.visit(elt.value)
                self.emit(extend_op, 1)
            else:
                self.visit(elt)
                if sequence_built:
                    self.emit(add_op, 1)

        if is_tuple:
            self.emit_call_intrinsic_1("INTRINSIC_LIST_TO_TUPLE")

    def make_child_codegen(
        self,
        tree: CodeGenTree,
        graph: PyFlowGraph,
        name: str | None = None,
    ) -> CodeGenerator312:
        graph.set_pos(SrcLocation(graph.firstline, graph.firstline, 0, 0))
        return type(self)(
            self,
            tree,
            self.symbols,
            graph,
            flags=self.flags,
            optimization_lvl=self.optimization_lvl,
            name=name,
        )

    def wrap_in_stopiteration_handler(self) -> None:
        handler = self.newBlock("handler")
        handler.is_exc_handler = True

        # compile.c handles this with a function that is used nowhere else
        # (instr_sequence_insert_instruction), so it seems worth just accessing
        # the block internal insts list rather than bubbling up an accessor
        # through several layers of containers.
        # TODO: handler.bid is unassigned at this point.
        setup = Instruction("SETUP_CLEANUP", handler, target=handler)
        for i, inst in enumerate(self.graph.entry.insts):
            if inst.opname == "RESUME":
                self.graph.entry.insts.insert(i, setup)
                break
        else:
            self.graph.entry.insts.insert(0, setup)

        self.emit_noline("LOAD_CONST", None)
        self.emit_noline("RETURN_VALUE")

        self.nextBlock(handler)
        self.set_no_pos()
        self.emit_call_intrinsic_1("INTRINSIC_STOPITERATION_ERROR")
        self.emit("RERAISE", 1)

    def generate_function_with_body(
        self, node: FuncOrLambda, name: str, first_lineno: int, body: list[ast.stmt]
    ) -> CodeGenerator:
        gen = cast(
            CodeGenerator312,
            self.make_func_codegen(node, node.args, name, first_lineno),
        )

        start = gen.newBlock("start")
        gen.nextBlock(start)

        scope = gen.scope
        add_stopiteration_handler = (
            scope.coroutine or scope.generator
        ) and not isinstance(node, ast.Lambda)
        if add_stopiteration_handler:
            gen.setups.append(Entry(STOP_ITERATION, start, None, None))

        self.processBody(node, body, gen)

        if add_stopiteration_handler:
            gen.wrap_in_stopiteration_handler()
            gen.pop_fblock(STOP_ITERATION)
        elif isinstance(node, ast.Lambda) and not scope.generator:
            if IS_3_12_8:
                gen.set_pos(node.body)
            else:
                gen.set_pos(SrcLocation(node.lineno, node.lineno or 0, 0, 0))
            gen.emit("RETURN_VALUE")
            gen.nextBlock()
            gen.finish_function()
        else:
            if isinstance(node, ast.Lambda):
                gen.set_pos(node.body)
            gen.finish_function()

        return gen

    def prepare_type_params(self) -> None:
        self.emit("PUSH_NULL")

    def finish_type_params(
        self, has_args: bool, num_typeparam_args: int, need_swap: bool = False
    ) -> None:
        if need_swap:
            self.emit("SWAP", num_typeparam_args + 1)
        self.emit("CALL", num_typeparam_args)

    def build_function(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef | ast.Lambda,
        gen: CodeGenerator,
        first_lineno: int | None = 0,
    ) -> None:
        flags = 0
        type_params = getattr(node, "type_params", None)
        if type_params:
            self.prepare_type_params()

        if node.args.defaults:
            for default in node.args.defaults:
                self.visitDefault(default)
                flags |= MAKE_FUNCTION_DEFAULTS
            self.emit("BUILD_TUPLE", len(node.args.defaults))

        if self.emit_kwonlydefaults(node):
            flags |= MAKE_FUNCTION_KWDEFAULTS

        outer_gen: CodeGenerator312 = self
        args = []
        num_typeparam_args = 0
        if type_params:
            assert isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
            args = []
            # The code object produced by CPython is a little wonky, if we have
            # no defaults, or just defaults, then `co_varnames` has .defaults.
            # If we have any kwdefaults then it'll have both .defaults and .kwdefaults
            # even though ".kwdefaults" isn't used and there's only a single arg.
            # This results in disassembly looking weird in that we have a
            # LOAD_FAST .defaults when we only have a kwdefaults. We therefore populate
            # args with both here, and then we'll reset "varnames"

            varnames = []
            varnames.append(".defaults")
            if node.args.defaults:
                args.append(".defaults")
                num_typeparam_args += 1
            if flags & MAKE_FUNCTION_KWDEFAULTS:
                args.append(".kwdefaults")
                varnames.append(".kwdefaults")
                num_typeparam_args += 1

            if num_typeparam_args == 2:
                self.emit("SWAP", 2)
            gen_param_scope = self.symbols.scopes[TypeParams(node)]
            graph = self.flow_graph(
                f"<generic parameters of {node.name}>",
                self.graph.filename,
                gen_param_scope,
                flags=CO_NESTED if gen_param_scope.nested else 0,
                args=varnames,
                kwonlyargs=(),
                starargs=(),
                optimized=1,
                docstring=None,
                firstline=first_lineno or node.lineno,
                posonlyargs=0,
            )
            graph.args = args

            outer_gen = self.make_child_codegen(
                TypeParams(node), graph, name=graph.name
            )
            outer_gen.class_name = self.class_name
            outer_gen.optimized = 1
            outer_gen.compile_type_params(type_params)

            outer_gen.set_pos(node)
            if node.args.defaults or (flags & MAKE_FUNCTION_KWDEFAULTS):
                outer_gen.emit("LOAD_FAST", 0)
            if node.args.defaults and (flags & MAKE_FUNCTION_KWDEFAULTS):
                outer_gen.emit("LOAD_FAST", 1)

        if not isinstance(node, ast.Lambda):
            flags |= outer_gen.build_annotations(node)

        outer_gen.set_pos(node)
        outer_gen.emit_closure(gen, flags)

        if type_params:
            outer_gen.emit("SWAP", 2)
            outer_gen.emit_call_intrinsic_2("INTRINSIC_SET_FUNCTION_TYPE_PARAMS")
            outer_gen.emit("RETURN_VALUE")
            self.emit_closure(outer_gen, 0)
            self.finish_type_params(len(args) > 0, num_typeparam_args, True)

    def emit_function_decorators(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef
    ) -> None:
        # Processed in reversed order as that's how they're called
        for dec in reversed(node.decorator_list):
            self.set_pos(dec)
            self.emit_call_one_arg()
        self.set_pos(node)

    def compile_type_param_bound_or_default(
        self,
        bound: ast.expr,
        name: str,
        # pyre-ignore[11]: Annotation `ast.ParamSpec` is not defined as a type.
        key: ast.TypeVar | ast.TypeVarTuple | ast.ParamSpec | TypeVarDefault,
        allow_starred: bool,
    ) -> None:
        typevar_scope = self.symbols.scopes[key]
        graph = self.flow_graph(
            name,
            self.graph.filename,
            typevar_scope,
            flags=CO_NESTED if typevar_scope.nested else 0,
            args=(),
            kwonlyargs=(),
            starargs=(),
            optimized=1,
            docstring=None,
            firstline=bound.lineno,
            posonlyargs=0,
            suppress_default_const=True,
        )
        # pyre-fixme[6]: For 1st argument expected `Union[Annotations, ClassDef,
        #  TypeParams, AsyncFunctionDef, DictComp, FunctionDef, GeneratorExp, Lambda,
        #  ListComp, SetComp, TypeAlias, arguments]` but got `Union[TypeVarDefault,
        #  ParamSpec, TypeVar, TypeVarTuple]`.
        outer_gen = self.make_child_codegen(key, graph, name=graph.name)
        outer_gen.class_name = self.class_name
        outer_gen.optimized = 1
        outer_gen.visit(bound)
        outer_gen.set_pos(key)
        outer_gen.emit("RETURN_VALUE")
        # This is dumb but will get optimized away. CPython will call
        # optimize_and_assemble which will do this adding the extra const
        # and then remove the LOAD_CONST/RETURN_VALUE as it's dead code.
        outer_gen.emit("LOAD_CONST", None)
        outer_gen.emit("RETURN_VALUE")

        self.emit_closure(outer_gen, 0)

    def compile_type_params(
        self,
        # pyre-ignore[11]: Annotation is not defined as a valid type
        type_params: list[ast.TypeVar | ast.TypeVarTuple | ast.ParamSpec],
    ) -> None:
        for param in type_params:
            # pyre-ignore[16]: Undefined attribute [16]: Module `ast` has no attribute `TypeVar`.
            if isinstance(param, ast.TypeVar):
                self.set_pos(param)
                self.emit("LOAD_CONST", param.name)
                if param.bound:
                    self.compile_type_param_bound_or_default(
                        param.bound, param.name, param, False
                    )
                    self.emit_call_intrinsic_2(
                        "INTRINSIC_TYPEVAR_WITH_CONSTRAINTS"
                        if isinstance(param.bound, ast.Tuple)
                        else "INTRINSIC_TYPEVAR_WITH_BOUND"
                    )
                else:
                    self.emit_call_intrinsic_1("INTRINSIC_TYPEVAR")

                if default_value := getattr(param, "default_value", None):
                    self.compile_type_param_bound_or_default(
                        default_value, param.name, TypeVarDefault(param), False
                    )
                    self.emit_call_intrinsic_2("INTRINSIC_SET_TYPEPARAM_DEFAULT")
                self.emit("COPY", 1)
                self.storeName(param.name)
            # pyre-ignore[16]: Module `ast` has no attribute `TypeVarTuple`.
            elif isinstance(param, ast.TypeVarTuple):
                self.set_pos(param)
                self.emit("LOAD_CONST", param.name)
                self.emit_call_intrinsic_1("INTRINSIC_TYPEVARTUPLE")
                if default_value := getattr(param, "default_value", None):
                    self.compile_type_param_bound_or_default(
                        default_value, param.name, param, True
                    )
                    self.emit_call_intrinsic_2("INTRINSIC_SET_TYPEPARAM_DEFAULT")
                self.emit("COPY", 1)
                self.storeName(param.name)
            # pyre-ignore[16]: Module `ast` has no attribute `ParamSpec`.
            elif isinstance(param, ast.ParamSpec):
                self.set_pos(param)
                self.emit("LOAD_CONST", param.name)
                self.emit_call_intrinsic_1("INTRINSIC_PARAMSPEC")
                if default_value := getattr(param, "default_value", None):
                    self.compile_type_param_bound_or_default(
                        default_value, param.name, param, False
                    )
                    self.emit_call_intrinsic_2("INTRINSIC_SET_TYPEPARAM_DEFAULT")
                self.emit("COPY", 1)
                self.storeName(param.name)
            else:
                raise NotImplementedError(
                    f"unsupported node in type params: {type(param).__name__}"
                )

        self.set_pos(type_params[0])
        self.emit("BUILD_TUPLE", len(type_params))

    def compile_body(self, gen: CodeGenerator, node: ast.ClassDef) -> None:
        if gen.findAnn(node.body):
            gen.did_setup_annotations = True
            gen.emit("SETUP_ANNOTATIONS")

        doc = gen.get_docstring(node)
        if doc is not None:
            gen.set_pos(node.body[0])
            gen.emit("LOAD_CONST", doc)
            gen.storeName("__doc__")

        self.walkClassBody(node, gen)

    def emit_init_class_attrs(
        self, gen: CodeGenerator, node: ast.ClassDef, first_lineno: int
    ) -> None:
        gen.emit("LOAD_NAME", "__name__")
        gen.storeName("__module__")

        gen.emit("LOAD_CONST", gen.get_qual_prefix(gen) + gen.name)
        gen.storeName("__qualname__")

    def compile_class_body(
        self, node: ast.ClassDef, first_lineno: int
    ) -> CodeGenerator:
        gen = self.make_class_codegen(node, first_lineno)

        scope = gen.scope
        assert isinstance(scope, ClassScope)

        self.emit_init_class_attrs(gen, node, first_lineno)

        # pyre-ignore[16]: no attribute type_params
        if node.type_params:
            gen.loadName(".type_params")
            gen.emit("STORE_NAME", "__type_params__")

        if scope.needs_classdict:
            gen.emit("LOAD_LOCALS")
            gen.emit("STORE_DEREF", "__classdict__")

        self.compile_body(gen, node)

        gen.set_no_pos()

        if scope.needs_classdict:
            gen.emit("LOAD_CLOSURE", "__classdict__")
            gen.emit("STORE_NAME", "__classdictcell__")

        if scope.needs_class_closure:
            gen.emit("LOAD_CLOSURE", "__class__")
            gen.emit("COPY", 1)
            gen.emit("STORE_NAME", "__classcell__")
        else:
            gen.emit("LOAD_CONST", None)

        gen.emit("RETURN_VALUE")
        return gen

    # Function calls --------------------------------------------------

    def emit_prepare_call(self) -> None:
        self.emit("PUSH_NULL")

    def emit_call(self, nargs: int) -> None:
        self.emit("CALL", nargs)

    def emit_call_kw(self, nargs: int, kwargs: tuple[str, ...]) -> None:
        self.emit("KW_NAMES", kwargs)
        self.emit("CALL", nargs + len(kwargs))

    def emit_call_exit_with_nones(self) -> None:
        self.emit("LOAD_CONST", None)
        self.emit("LOAD_CONST", None)
        self.emit("LOAD_CONST", None)
        self.emit("CALL", 2)

    def get_generic_base(self, node: ClassDef) -> ast.Name:
        return ast.Name(
            ".generic_base",
            lineno=node.lineno,
            end_lineno=node.end_lineno,
            col_offset=node.col_offset,
            end_col_offset=node.end_col_offset,
            ctx=ast.Load(),
        )

    def emit_load_build_class(self) -> None:
        self.emit("PUSH_NULL")
        self.emit("LOAD_BUILD_CLASS")

    def emit_build_class(
        self,
        node: ast.ClassDef,
        class_body: CodeGenerator,
        outer_scope: CodeGenerator | None = None,
    ) -> None:
        self.emit_load_build_class()
        self.emit_closure(class_body, 0)
        self.emit("LOAD_CONST", node.name)

        # pyre-ignore[16]: no attribute type_params
        if node.type_params:
            self.loadName(".type_params")
            self.emit_call_intrinsic_1("INTRINSIC_SUBSCRIPT_GENERIC")
            self.emit("STORE_FAST", ".generic_base")

            self._call_helper(
                2,
                None,
                node.bases + [self.get_generic_base(node)],
                node.keywords,
            )

            self.emit("RETURN_VALUE")

            assert outer_scope is not None, "type params are only in 3.12"
            outer_scope.emit_closure(self, 0)
            outer_scope.finish_type_params(False, 0)
        else:
            self._call_helper(2, None, node.bases, node.keywords)

    def visitClassDef(self, node: ast.ClassDef) -> None:
        first_lineno = None
        immutability_flag = self.find_immutability_flag(node)
        for decorator in node.decorator_list:
            if first_lineno is None:
                first_lineno = decorator.lineno
            self.visit_decorator(decorator, node)

        first_lineno = node.lineno if first_lineno is None else first_lineno

        outer_gen: CodeGenerator312 = self
        # pyre-ignore[16]: no attribute type_params
        if node.type_params:
            self.prepare_type_params()
            gen_param_scope = self.symbols.scopes[TypeParams(node)]
            graph = self.flow_graph(
                f"<generic parameters of {node.name}>",
                self.graph.filename,
                gen_param_scope,
                flags=CO_NESTED if gen_param_scope.nested else 0,
                args=(),
                kwonlyargs=(),
                starargs=(),
                optimized=1,
                docstring=None,
                firstline=first_lineno,
                posonlyargs=0,
            )
            graph.args = ()

            outer_gen = self.make_child_codegen(
                TypeParams(node), graph, name=graph.name
            )
            outer_gen.class_name = node.name
            outer_gen.optimized = 1
            # pyre-fixme[6]: For 1st argument expected `List[Union[ParamSpec,
            #  TypeVar, TypeVarTuple]]` but got `List[type_param]`.
            outer_gen.compile_type_params(node.type_params)

            outer_gen.set_pos(node)
            outer_gen.emit("STORE_DEREF", ".type_params")

        class_gen = outer_gen.compile_class_body(node, first_lineno)

        outer_gen.emit_build_class(node, class_gen, self)

        for d in reversed(node.decorator_list):
            self.set_pos(d)
            self.emit_decorator_call(d, node)

        self.register_immutability(node, immutability_flag)
        self.set_pos(node)
        self.post_process_and_store_name(node)

    # pyre-ignore[11]: Annotation `ast.TypeAlias` is not defined as a type.
    def make_type_alias_code_gen(self, node: ast.TypeAlias) -> CodeGenerator312:
        filename = self.graph.filename
        symbols = self.symbols

        scope = symbols.scopes[node]
        graph = self.flow_graph(
            node.name.id,
            filename,
            scope,
            optimized=1,
            firstline=node.lineno,
            flags=CO_NESTED if scope.nested else 0,
        )

        res = self.make_child_codegen(node, graph)
        res.class_name = self.class_name
        res.optimized = 1
        return res

    def emit_type_alias_set_func_defaults(self, code_gen: CodeGenerator) -> None:
        pass

    def visitTypeAlias(self, node: ast.TypeAlias) -> None:
        outer_gen: CodeGenerator312 = self
        if node.type_params:
            self.prepare_type_params()
            generic_param_scope = self.symbols.scopes[TypeParams(node)]
            graph = self.flow_graph(
                f"<generic parameters of {node.name.id}>",
                self.graph.filename,
                generic_param_scope,
                flags=CO_NESTED if generic_param_scope.nested else 0,
                args=(),
                kwonlyargs=(),
                starargs=(),
                optimized=1,
                docstring=None,
                firstline=node.lineno,
                posonlyargs=0,
            )
            graph.args = ()

            outer_gen = self.make_child_codegen(
                TypeParams(node), graph, name=graph.name
            )
            outer_gen.class_name = self.class_name
            outer_gen.optimized = 1
            outer_gen.set_pos(node)
            outer_gen.emit("LOAD_CONST", node.name.id)
            # pyre-fixme[6]: For 1st argument expected `List[Union[ParamSpec,
            #  TypeVar, TypeVarTuple]]` but got `List[type_param]`.
            outer_gen.compile_type_params(node.type_params)
        else:
            outer_gen.emit("LOAD_CONST", node.name.id)
            outer_gen.emit("LOAD_CONST", None)

        code_gen = outer_gen.make_type_alias_code_gen(node)
        code_gen.visit(node.value)
        code_gen.graph.emit_with_loc("RETURN_VALUE", 0, loc=node)

        outer_gen.set_pos(node)
        outer_gen.emit_closure(code_gen, 0)
        self.emit_type_alias_set_func_defaults(outer_gen)
        outer_gen.emit("BUILD_TUPLE", 3)
        outer_gen.emit_call_intrinsic_1("INTRINSIC_TYPEALIAS")

        if node.type_params:
            outer_gen.emit("RETURN_VALUE")
            self.emit_closure(outer_gen, 0)
            self.finish_type_params(False, 0)

        self.storeName(node.name.id)

    def get_qual_prefix(self, gen: CodeHolder) -> str:
        prefix = ""
        if gen.scope.global_scope:
            return prefix
        # Construct qualname prefix
        parent = gen.scope.parent
        while not isinstance(parent, ModuleScope):
            if not isinstance(parent, TypeParamScope):
                # Only real functions use "<locals>", nested scopes like
                # comprehensions don't.
                if parent.is_function_scope:
                    prefix = parent.name + ".<locals>." + prefix
                else:
                    prefix = parent.name + "." + prefix
                if parent.global_scope:
                    break
            parent = parent.parent
        return prefix

    def emit_make_function(
        self, gen: CodeHolder | CodeGenerator | PyFlowGraph, qualname: str, flags: int
    ) -> None:
        if isinstance(gen, CodeGenerator):
            gen.set_qual_name(qualname)
        elif isinstance(gen, PyFlowGraph312):
            gen.qualname = qualname
        self.emit("LOAD_CONST", gen)
        self.emit("MAKE_FUNCTION", flags)

    def _nameOp(self, prefix: str, name: str) -> None:
        # TASK(T130490253): The JIT suppression should happen in the jit, not the compiler.
        if (
            prefix == "LOAD"
            and name == "super"
            and isinstance(self.scope, FunctionScope)
        ):
            scope = self.check_name(name)
            if scope in (SC_GLOBAL_EXPLICIT, SC_GLOBAL_IMPLICIT):
                self.scope.suppress_jit = True

        name = self.mangle(name)
        scope = self.check_name(name)
        if scope == SC_LOCAL:
            if not self.optimized and name not in self.fast_hidden:
                self.emit(prefix + "_NAME", name)
            else:
                self.emit(prefix + "_FAST", name)
        elif scope == SC_GLOBAL_EXPLICIT:
            self.emit(prefix + "_GLOBAL", name)
        elif scope == SC_GLOBAL_IMPLICIT:
            if self.scope.can_see_class_scope:
                self.emit("LOAD_DEREF", "__classdict__")
                self.emit(prefix + "_FROM_DICT_OR_GLOBALS", name)
            elif isinstance(self.scope, ClassScope):
                if self.inlined_comp_depth:
                    self.emit(prefix + "_GLOBAL", name)
                else:
                    self.emit(prefix + "_NAME", name)
            elif not self.optimized:
                self.emit(prefix + "_NAME", name)
            else:
                self.emit(prefix + "_GLOBAL", name)
        elif scope == SC_FREE or scope == SC_CELL:
            if isinstance(self.scope, ClassScope) and prefix == "LOAD":
                self.emit("LOAD_LOCALS")
                self.emit("LOAD_FROM_DICT_OR_DEREF", name)
            elif self.scope.can_see_class_scope and prefix == "LOAD":
                self.emit("LOAD_DEREF", "__classdict__")
                self.emit("LOAD_FROM_DICT_OR_DEREF", name)
            else:
                self.emit(prefix + "_DEREF", name)
        elif scope == SC_UNKNOWN:
            self.emit(prefix + "_NAME", name)
        else:
            raise RuntimeError(f"unsupported scope for var {name}: {scope}")

    def compute_start_location_to_match_attr(
        self, base: ast.expr, attr: ast.Attribute
    ) -> AST | SrcLocation:
        if base.lineno != attr.end_lineno:
            lineno = attr.end_lineno
            end_lineno = base.end_lineno
            col_offset = base.col_offset
            end_col_offset = base.end_col_offset
            assert (
                end_lineno is not None
                and lineno is not None
                and end_col_offset is not None
            )

            attr_len = len(attr.attr)
            if attr.end_col_offset is not None and attr_len <= attr.end_col_offset:
                col_offset = attr.end_col_offset - attr_len
            else:
                # GH-94694: Somebody's compiling weird ASTs. Just drop the columns:
                col_offset = -1
                end_col_offset = -1

            # Make sure the end position still follows the start position, even for
            # weird ASTs:
            end_lineno = max(lineno, end_lineno)
            if lineno == end_lineno:
                end_col_offset = max(col_offset, end_col_offset)

            return SrcLocation(
                lineno or -1, end_lineno, col_offset, end_col_offset or 0
            )

        return base

    def emit_super_attribute(self, node: ast.Attribute) -> None:
        call = node.value
        assert isinstance(call, ast.Call)
        self.graph.emit_with_loc("LOAD_GLOBAL", "super", call.func)
        self.set_pos(call)
        load_arg, is_zero = self._emit_args_for_super(call, node.attr)
        op = "LOAD_ZERO_SUPER_ATTR" if is_zero else "LOAD_SUPER_ATTR"
        self.graph.emit_with_loc("LOAD_SUPER_ATTR", (op, load_arg, is_zero), node)

    def visitAttribute(self, node: ast.Attribute) -> None:
        if (
            isinstance(node.ctx, ast.Load)
            and self._is_super_call(node.value)
            and node.attr != "__class__"
        ):
            self.emit_super_attribute(node)
            return

        self.visit(node.value)
        if isinstance(node.ctx, ast.Store):
            op = "STORE_ATTR"
        elif isinstance(node.ctx, ast.Del):
            op = "DELETE_ATTR"
        else:
            op = "LOAD_ATTR"

        loc = self.compute_start_location_to_match_attr(node, node)
        self.graph.emit_with_loc(op, self.mangle(node.attr), loc)

    # Exceptions --------------------------------------------------

    # pyre-ignore[11]: Annotation `ast.TryStar` is not defined as a type.
    def visitTryStar(self, node: ast.TryStar) -> None:
        if node.finalbody:
            # pyre-fixme[6]: For 1st argument expected `Optional[Try]` but got
            #  `TryStar`.
            self.emit_try_finally(node, star=True)
        else:
            self.emit_try_star_except(node)

    def emit_pop_except_and_reraise(self) -> None:
        self.emit_noline("COPY", 3)
        self.emit_noline("POP_EXCEPT")
        self.emit_noline("RERAISE", 1)

    def emit_try_finally(
        self,
        node: ast.Try | None,
        try_body: Callable[[], None] | None = None,
        final_body: Callable[[], None] | None = None,
        star: bool = False,
    ) -> None:
        assert (try_body is not None and final_body is not None) or node is not None, (
            "should be called with nodes or body"
        )
        body = self.make_try_body_block()
        end = self.newBlock("try_finally_end")
        exit_ = self.newBlock("try_finally_exit")
        cleanup = self.newBlock("cleanup")

        if final_body is None:

            def final_body() -> None:
                assert node
                return self.visitStatements(node.finalbody)

        # try block
        self.emit("SETUP_FINALLY", end)

        if body is not None:
            self.nextBlock(body)
        self.push_fblock(Entry(FINALLY_TRY, body, end, final_body))
        if node is None:
            assert try_body
            try_body()
        elif node.handlers:
            if star:
                # pyre-fixme[6]: For 1st argument expected `TryStar` but got `Try`.
                self.emit_try_star_except(node)
            else:
                self.emit_try_except(node)
        else:
            self.visitStatements(node.body)

        self.emit_noline("POP_BLOCK")
        self.pop_fblock(FINALLY_TRY)
        final_body()

        self.emit_jump_forward_noline(exit_)

        # finally block
        self.nextBlock(end)
        self.set_no_pos()
        self.emit("SETUP_CLEANUP", cleanup)
        self.emit("PUSH_EXC_INFO")
        self.push_fblock(Entry(FINALLY_END, end, None, None))
        final_body()
        self.pop_fblock(FINALLY_END)
        self.emit_noline("RERAISE", 0)

        self.nextBlock(cleanup)
        self.emit_pop_except_and_reraise()

        self.nextBlock(exit_)

    def emit_jump_eh_end(self, target: Block) -> None:
        self.emit_jump_forward(target)

    def make_try_body_block(self) -> Block | None:
        return self.newBlock("try_body")

    def emit_try_except(self, node: ast.Try) -> None:
        body = self.make_try_body_block()
        except_ = self.newBlock("try_except")
        end = self.newBlock("try_end")
        cleanup = self.newBlock("try_cleanup")

        self.emit("SETUP_FINALLY", except_)

        if body is not None:
            self.nextBlock(body)
        self.push_fblock(Entry(TRY_EXCEPT, body, None, None))
        self.visitStatements(node.body)
        self.pop_fblock(TRY_EXCEPT)
        self.emit_noline("POP_BLOCK")
        self.visitStatements(node.orelse)
        self.set_no_pos()
        self.emit_jump_eh_end(end)

        self.nextBlock(except_)
        self.emit("SETUP_CLEANUP", cleanup)
        self.emit("PUSH_EXC_INFO")

        # Runtime will push a block here, so we need to account for that
        self.push_fblock(Entry(EXCEPTION_HANDLER, None, None, None))

        last = len(node.handlers) - 1
        for i in range(len(node.handlers)):
            handler = node.handlers[i]
            target = handler.name
            self.set_pos(handler)
            except_ = self.newBlock(f"try_except_{i}")
            if handler.type:
                self.visit(handler.type)
                self.emit("CHECK_EXC_MATCH")
                self.emit("POP_JUMP_IF_FALSE", except_)
                self.nextBlock()
            elif i < last:
                raise self.syntax_error("default 'except:' must be last", handler)

            if target:
                cleanup_end = self.newBlock(f"try_cleanup_end_{i}")
                cleanup_body = self.newBlock(f"try_cleanup_body_{i}")

                self.storeName(target)

                self.emit("SETUP_CLEANUP", cleanup_end)

                self.nextBlock(cleanup_body)
                self.push_fblock(Entry(HANDLER_CLEANUP, cleanup_body, None, target))
                self.visitStatements(handler.body)
                self.pop_fblock(HANDLER_CLEANUP)
                self.set_no_pos()
                self.emit("POP_BLOCK")
                self.emit("POP_BLOCK")
                self.emit("POP_EXCEPT")
                self.emit("LOAD_CONST", None)
                self.storeName(target)
                self.delName(target)
                self.emit_jump_eh_end(end)

                # except:
                self.nextBlock(cleanup_end)
                self.set_no_pos()
                self.emit("LOAD_CONST", None)
                self.storeName(target)
                self.delName(target)

                self.emit("RERAISE", 1)
            else:
                cleanup_body = self.newBlock(f"try_cleanup_body_{i}")
                self.emit("POP_TOP")
                self.nextBlock(cleanup_body)
                self.push_fblock(Entry(HANDLER_CLEANUP, cleanup_body, None, None))
                self.visitStatements(handler.body)
                self.pop_fblock(HANDLER_CLEANUP)
                self.set_no_pos()
                self.emit("POP_BLOCK")
                self.emit("POP_EXCEPT")
                self.emit_jump_eh_end(end)

            self.nextBlock(except_)

        self.pop_fblock(EXCEPTION_HANDLER)
        self.set_no_pos()
        self.emit("RERAISE", 0)
        self.nextBlock(cleanup)
        self.pop_except_and_reraise()
        self.nextBlock(end)

    def emit_try_star_except(self, node: ast.TryStar) -> None:
        body = self.newBlock("try*_body")
        except_ = self.newBlock("try*_except")
        orelse = self.newBlock("try*_orelse")
        end = self.newBlock("try*_end")
        cleanup = self.newBlock("try*_cleanup")
        reraise_star = self.newBlock("try*_reraise_star")

        self.emit("SETUP_FINALLY", except_)

        self.nextBlock(body)
        self.push_fblock(Entry(TRY_EXCEPT, body, None, None))
        self.visitStatements(node.body)
        self.pop_fblock(TRY_EXCEPT)
        self.set_no_pos()
        self.emit("POP_BLOCK")
        self.emit_jump_forward_noline(orelse)

        self.nextBlock(except_)
        self.emit("SETUP_CLEANUP", cleanup)
        self.emit("PUSH_EXC_INFO")

        # Runtime will push a block here, so we need to account for that
        self.push_fblock(Entry(EXCEPTION_GROUP_HANDLER, None, None, "except_handler"))

        n = len(node.handlers)
        for i in range(n):
            handler = node.handlers[i]
            self.set_pos(handler)
            next_except = self.newBlock(f"next_except_{i}")
            except_ = next_except
            except_with_error = self.newBlock(f"except_with_error_{i}")
            no_match = self.newBlock(f"no_match_{i}")
            if i == 0:
                # create empty list for exceptions raised/reraise in the except* blocks
                self.emit("BUILD_LIST", 0)
                self.emit("COPY", 2)

            if handler.type:
                self.visit(handler.type)
                self.emit("CHECK_EG_MATCH")
                self.emit("COPY", 1)
                self.emit("POP_JUMP_IF_NONE", no_match)

            # We need a new block here (cpython creates it in a later pass)
            self.nextBlock(label="try*_insert_block")

            cleanup_end = self.newBlock("try*_cleanup_end")
            cleanup_body = self.newBlock("try*_cleanup_body")

            if handler.name:
                self.storeName(handler.name)
            else:
                self.emit("POP_TOP")

            # second try:
            self.emit("SETUP_CLEANUP", cleanup_end)

            self.nextBlock(cleanup_body)
            self.push_fblock(Entry(HANDLER_CLEANUP, cleanup_body, None, handler.name))

            # second body
            self.visitStatements(handler.body)
            self.pop_fblock(HANDLER_CLEANUP)
            self.set_no_pos()
            self.emit("POP_BLOCK")
            if handler.name:
                self.emit("LOAD_CONST", None)
                # pyre-fixme[6]: For 1st argument expected `str` but got
                #  `Optional[str]`.
                self.storeName(handler.name)
                # pyre-fixme[6]: For 1st argument expected `str` but got
                #  `Optional[str]`.
                self.delName(handler.name)
            self.emit_jump_forward(except_)

            # except:
            self.nextBlock(cleanup_end)
            if handler.name:
                self.emit("LOAD_CONST", None)
                # pyre-fixme[6]: For 1st argument expected `str` but got
                #  `Optional[str]`.
                self.storeName(handler.name)
                # pyre-fixme[6]: For 1st argument expected `str` but got
                #  `Optional[str]`.
                self.delName(handler.name)

            # add exception raised to the res list
            self.emit("LIST_APPEND", 3)
            self.emit("POP_TOP")
            self.emit_jump_forward(except_with_error)

            self.nextBlock(except_)
            self.emit("NOP")
            self.emit_jump_forward(except_with_error)

            self.nextBlock(no_match)
            self.set_pos(handler)
            self.emit("POP_TOP")

            self.nextBlock(except_with_error)

            if i == n - 1:
                # Add exc to the list (if not None it's the unhandled part of the EG)
                self.set_no_pos()
                self.emit("LIST_APPEND", 1)
                self.emit_jump_forward(reraise_star)

        # end handler loop

        self.pop_fblock(EXCEPTION_GROUP_HANDLER)
        reraise = self.newBlock("try*_reraise")

        self.nextBlock(reraise_star)
        self.emit_call_intrinsic_2("INTRINSIC_PREP_RERAISE_STAR")
        self.emit("COPY", 1)
        self.emit("POP_JUMP_IF_NOT_NONE", reraise)

        # We need a new block here (cpython creates it in a later pass)
        self.nextBlock(label="try*_insert_block")

        # Nothing to reraise
        self.emit_noline("POP_TOP")
        self.emit_noline("POP_BLOCK")
        self.emit_noline("POP_EXCEPT")
        self.emit_jump_forward_noline(end)

        self.nextBlock(reraise)
        self.emit_noline("POP_BLOCK")
        self.emit_noline("SWAP", 2)
        self.emit_noline("POP_EXCEPT")
        self.emit_noline("RERAISE", 0)

        self.nextBlock(cleanup)
        self.pop_except_and_reraise()

        self.nextBlock(orelse)
        self.visitStatements(node.orelse)

        self.nextBlock(end)

    def pop_except_and_reraise(self) -> None:
        self.emit("COPY", 3)
        self.emit("POP_EXCEPT")
        self.emit("RERAISE", 1)

    def emit_unwind_with_preserve_tos(self) -> None:
        self.emit_rotate_stack(2)

    def unwind_setup_entry(self, e: Entry, preserve_tos: int) -> None:
        if e.kind in (
            WHILE_LOOP,
            EXCEPTION_HANDLER,
            ASYNC_COMPREHENSION_GENERATOR,
            STOP_ITERATION,
        ):
            return

        elif e.kind == FOR_LOOP or e.kind == ASYNC_FOR_LOOP:
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit("POP_TOP")

        elif e.kind == TRY_EXCEPT:
            self.emit("POP_BLOCK")

        elif e.kind == FINALLY_TRY:
            self.emit("POP_BLOCK")
            if preserve_tos:
                self.push_fblock(Entry(POP_VALUE, None, None, None))
            assert callable(e.unwinding_datum)
            e.unwinding_datum()
            if preserve_tos:
                self.pop_fblock(POP_VALUE)
            self.set_no_pos()

        elif e.kind == FINALLY_END:
            if preserve_tos:
                self.emit("SWAP", 2)
            self.emit("POP_TOP")
            if preserve_tos:
                self.emit("SWAP", 2)
            self.emit("POP_BLOCK")
            self.emit("POP_EXCEPT")

        elif e.kind in (WITH, ASYNC_WITH):
            if IS_3_12_8:
                assert e.node is not None
                self.set_pos(e.node)
            else:
                assert isinstance(e.unwinding_datum, AST)
                self.set_pos(e.unwinding_datum)
            self.emit("POP_BLOCK")
            if preserve_tos:
                self.emit_unwind_with_preserve_tos()
            self.emit_call_exit_with_nones()
            if e.kind == ASYNC_WITH:
                self.emit_get_awaitable(AwaitableKind.AsyncExit)
                self.emit("LOAD_CONST", None)
                self.emit_yield_from(await_=True)
            self.emit("POP_TOP")
            self.set_no_pos()

        elif e.kind == HANDLER_CLEANUP:
            datum = e.unwinding_datum
            if datum is not None:
                self.emit("POP_BLOCK")
            if preserve_tos:
                self.emit("SWAP", 2)
            self.emit("POP_BLOCK")
            self.emit("POP_EXCEPT")
            if datum is not None:
                self.emit("LOAD_CONST", None)
                assert isinstance(datum, str)
                self.storeName(datum)
                self.delName(datum)

        elif e.kind == POP_VALUE:
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit("POP_TOP")

        else:
            raise Exception(f"Unexpected kind {e.kind}")

    def compile_comprehension(
        self,
        node: CompNode,
        name: str,
        elt: ast.expr,
        val: ast.expr | None,
        opcode: str | None,
        oparg: object = 0,
    ) -> None:
        self.check_async_comprehension(node)

        # fetch the scope that corresponds to comprehension
        scope = self.scopes[node]
        assert isinstance(scope, GenExprScope)

        outermost = node.generators[0]
        inlined_state: InlinedComprehensionState | None = None
        if scope.inlined:
            # for inlined comprehension process with current generator
            gen = self
            gen.compile_comprehension_iter(outermost)
            inlined_state = self.push_inlined_comprehension_state(scope)
        else:
            gen = cast(CodeGenerator312, self.make_comprehension_codegen(node, name))
        gen.set_pos(node)
        start = gen.newBlock("start")
        gen.setups.append(Entry(STOP_ITERATION, start, None, None))
        if opcode:
            gen.emit(opcode, oparg)
            if scope.inlined:
                gen.emit("SWAP", 2)

        assert isinstance(gen, CodeGenerator312)
        gen.compile_comprehension_generator(
            node,
            0,
            0,
            elt,
            val,
            type(node),
            IterStackState.IterableOnStack
            if scope.inlined
            else IterStackState.IterableInLocal,
        )
        if inlined_state is not None:
            self.pop_fblock(STOP_ITERATION)
            self.pop_inlined_comprehension_state(scope, inlined_state)
            return

        if not isinstance(node, ast.GeneratorExp):
            gen.emit("RETURN_VALUE")

        gen.wrap_in_stopiteration_handler()
        gen.pop_fblock(STOP_ITERATION)

        self.finish_comprehension(gen, node)

    def compile_comprehension_generator(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        if comp.generators[gen_index].is_async:
            self._compile_async_comprehension(
                comp, gen_index, depth, elt, val, type, iter_on_stack
            )
        else:
            self._compile_sync_comprehension(
                comp, gen_index, depth, elt, val, type, iter_on_stack
            )

    def _compile_async_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        start = self.newBlock("start")
        except_ = self.newBlock("except")
        if_cleanup = self.newBlock("if_cleanup")

        gen = comp.generators[gen_index]
        if iter_on_stack == IterStackState.IterableInLocal:
            if gen_index == 0:
                self.loadName(".0")
            else:
                self.visit(gen.iter)
                self.set_pos(comp)
                if IS_3_12_8:
                    self.graph.emit_with_loc("GET_AITER", 0, gen.iter)
                else:
                    self.emit("GET_AITER")

        self.nextBlock(start)
        self.emit("SETUP_FINALLY", except_)
        self.emit("GET_ANEXT")
        self.emit("LOAD_CONST", None)
        self.emit_yield_from(await_=True)
        self.emit("POP_BLOCK")
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        depth += 1
        gen_index += 1
        elt_loc = elt
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, IterStackState.IterableInLocal
            )
        elif type is ast.GeneratorExp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit_yield(self.scopes[comp])
            self.emit("POP_TOP")
        elif type is ast.ListComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("LIST_APPEND", depth + 1)
        elif type is ast.SetComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("SET_ADD", depth + 1)
        elif type is ast.DictComp:
            assert val is not None
            self.compile_dictcomp_element(elt, val)
            elt_loc = SrcLocation(
                elt.lineno,
                val.end_lineno or 0,
                elt.col_offset,
                val.end_col_offset or 0,
            )
            self.set_pos(elt_loc)
            self.emit("MAP_ADD", depth + 1)
        else:
            raise NotImplementedError("unknown comprehension type")

        self.nextBlock(if_cleanup)
        self.set_pos(elt_loc)
        self.emitJump(start)

        self.nextBlock(except_)
        self.set_pos(comp)
        self.emit("END_ASYNC_FOR")

    def _compile_sync_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        start = self.newBlock("start")
        skip = self.newBlock("skip")
        if_cleanup = self.newBlock("if_cleanup")
        anchor = self.newBlock("anchor")

        gen = comp.generators[gen_index]
        if not iter_on_stack:
            if gen_index == 0:
                self.loadName(".0")
            else:
                if isinstance(gen.iter, (ast.Tuple, ast.List)):
                    elts = gen.iter.elts
                    if len(elts) == 1 and not isinstance(elts[0], ast.Starred):
                        self.visit(elts[0])
                        start = None
                if start:
                    self.set_pos(comp)
                    self.compile_comprehension_iter(gen)

        if start:
            depth += 1
            self.nextBlock(start)
            if IS_3_12_8:
                self.graph.set_pos(gen.iter)
                self.graph.emit_with_loc("FOR_ITER", anchor, gen.iter)
            else:
                self.emit("FOR_ITER", anchor)
            self.nextBlock()
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        gen_index += 1
        elt_loc = elt
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, IterStackState.IterableInLocal
            )
        else:
            if type is ast.GeneratorExp:
                self.set_pos(elt)
                self.visit(elt)
                self.emit_yield(self.scopes[comp])
                self.emit("POP_TOP")
            elif type is ast.ListComp:
                self.visit(elt)
                self.set_pos(elt)
                self.emit("LIST_APPEND", depth + 1)
            elif type is ast.SetComp:
                self.set_pos(elt)
                self.visit(elt)
                self.emit("SET_ADD", depth + 1)
            elif type is ast.DictComp:
                assert elt is not None and val is not None
                self.compile_dictcomp_element(elt, val)
                elt_loc = SrcLocation(
                    elt.lineno,
                    val.end_lineno or 0,
                    elt.col_offset,
                    val.end_col_offset or 0,
                )
                self.set_pos(elt_loc)
                self.emit("MAP_ADD", depth + 1)
            else:
                raise NotImplementedError("unknown comprehension type")

            self.nextBlock(skip)
        self.nextBlock(if_cleanup)
        if start:
            self.set_pos(elt_loc)
            self.emitJump(start)
            self.nextBlock(anchor)
            self.set_pos(comp)
            self.emit_end_for()

    def push_inlined_comprehension_state(
        self, scope: Scope
    ) -> InlinedComprehensionState:
        end = self.newBlock("end")
        cleanup = self.newBlock("cleanup")
        temp_symbols: dict[str, int] | None = None
        fast_hidden: set[str] = set()
        pushed_locals: list[str] = []
        self.inlined_comp_depth += 1

        # iterate over names bound in the comprehension and ensure we isolate
        # them from the outer scope as needed
        for name in scope.symbols:
            # If a name has different scope inside than outside the comprehension,
            # we need to temporarily handle it with the right scope while
            # compiling the comprehension. If it's free in the comprehension
            # scope, no special handling; it should be handled the same as the
            # enclosing scope. (If it's free in outer scope and cell in inner
            # scope, we can't treat it as both cell and free in the same function,
            # but treating it as free throughout is fine; it's *_DEREF
            # either way.)
            compsc = scope.check_name(name)
            outsc = self.check_name(name)
            # When a comprehension is inlined into the global scope CPython loses the
            # DEF_IMPORTED flag. It'll see compsc != outsc and will replace it in the
            # top-level symbols dict w/ the version from the list comp which doesn't have
            # the flag set. We simulate this by checking if we're in the module scope and
            # always putting a value in temp_symbols, we then disable the is_import_originated
            # check for these names (this is really ugly, and CPython probably doesn't mean
            # to lose the DEF_IMPORTED flag in these cases).
            if (
                isinstance(self.scope, ModuleScope) and outsc == SC_GLOBAL_IMPLICIT
            ) or (
                compsc != outsc
                and compsc != SC_FREE
                and not (compsc == SC_CELL and outsc == SC_FREE)
            ):
                if temp_symbols is None:
                    temp_symbols = {}
                temp_symbols[name] = compsc

            # locals handling for names bound in comprehension
            if (
                name in scope.defs
                and name not in scope.nonlocals
                and name not in scope.params
            ) or isinstance(self.scope, ClassScope):
                self.fast_hidden.add(name)
                fast_hidden.add(name)
            elif name in scope.params or name not in scope.defs:
                # ignore .0
                continue

            self.emit("LOAD_FAST_AND_CLEAR", name)
            if name in scope.cells:
                name_idx = (
                    # Matching PyFlowGraph._convert_DEREF offset for free var index
                    (self.graph.freevars.get_index(name) + len(self.graph.cellvars))
                    if name in self.scope.frees
                    else self.graph.cellvars.get_index(name)
                )
                self.emit("MAKE_CELL", name_idx)

            pushed_locals.append(name)

        if pushed_locals:
            self.emit("SWAP", len(pushed_locals) + 1)
            self.emit("SETUP_FINALLY", cleanup)

        prev_temp_symbols = self.temp_symbols
        self.temp_symbols = temp_symbols
        return InlinedComprehensionState(
            pushed_locals, fast_hidden, prev_temp_symbols, end, cleanup
        )

    def pop_inlined_comprehension_state(
        self, scope: Scope, inlined_state: InlinedComprehensionState
    ) -> None:
        self.inlined_comp_depth -= 1
        self.temp_symbols = inlined_state.prev_temp_symbols
        if inlined_state.pushed_locals:
            self.emit_noline("POP_BLOCK")
            self.emit_jump_forward_noline(inlined_state.end)

            self.nextBlock(inlined_state.cleanup)

            # discard incomplete comprehension result (beneath exc on stack)
            self.emit_noline("SWAP", 2)
            self.emit_noline("POP_TOP")
            self.restore_inlined_comprehension_locals(inlined_state)

            self.emit_noline("RERAISE")

            self.nextBlock(inlined_state.end)
            self.restore_inlined_comprehension_locals(inlined_state)

        self.fast_hidden -= inlined_state.fast_hidden

    def restore_inlined_comprehension_locals(
        self, inlined_state: InlinedComprehensionState
    ) -> None:
        self.emit("SWAP", len(inlined_state.pushed_locals) + 1)
        for local in reversed(inlined_state.pushed_locals):
            self.emit("STORE_FAST_MAYBE_NULL", local)

    def set_match_pos(self, node: AST) -> None:
        self.set_pos(node)

    def emit_match_jump_to_end(self, end: Block) -> None:
        self.emit_jump_forward_noline(end)

    def emit_match_jump_to_fail_pop_unconditional(self, pc: PatternContext) -> None:
        self._jump_to_fail_pop(pc, "JUMP")

    def emit_finish_match_class(self, node: ast.MatchClass, pc: PatternContext) -> None:
        self.emit("COPY", 1)
        self.emit("LOAD_CONST", None)
        self.emit("IS_OP", 1)

        # TOS is now a tuple of (nargs + nattrs) attributes. Preserve it:
        pc.on_top += 1
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")

        patterns = node.patterns
        kwd_patterns = node.kwd_patterns
        nargs = len(patterns)
        nattrs = len(node.kwd_attrs)

        self.emit("UNPACK_SEQUENCE", nargs + nattrs)
        pc.on_top += nargs + nattrs - 1

        for i in range(nargs + nattrs):
            pc.on_top -= 1
            if i < nargs:
                pattern = patterns[i]
            else:
                pattern = kwd_patterns[i - nargs]
            if self._wildcard_check(pattern):
                self.graph.emit_with_loc("POP_TOP", 0, node)
                continue

            self._visit_subpattern(pattern, pc)

    def emit_finish_match_mapping(
        self,
        node: ast.MatchMapping,
        pc: PatternContext,
        star_target: str | None,
        size: int,
    ) -> None:
        self.emit("BUILD_TUPLE", size)
        self.emit("MATCH_KEYS")
        # There's now a tuple of keys and a tuple of values on top of the subject:
        pc.on_top += 2
        self.emit("COPY", 1)
        self.emit("LOAD_CONST", None)
        self.emit("IS_OP", 1)
        self._jump_to_fail_pop(pc, "POP_JUMP_IF_FALSE")
        # So far so good. Use that tuple of values on the stack to match
        # sub-patterns against:
        self.emit("UNPACK_SEQUENCE", size)
        pc.on_top += size - 1
        for pattern in node.patterns:
            pc.on_top -= 1
            self._visit_subpattern(pattern, pc)

        # If we get this far, it's a match! We're done with the tuple of values,
        # and whatever happens next should consume the tuple of keys underneath it:
        self.set_match_pos(node)
        pc.on_top -= 2
        if star_target:
            # If we have a starred name, bind a dict of remaining items to it (this may
            # seem a bit inefficient, but keys is rarely big enough to actually impact
            # runtime):
            # rest = dict(TOS1)
            # for key in TOS:
            #     del rest[key]
            self.emit("BUILD_MAP", 0)
            self.emit("SWAP", 3)
            self.emit("DICT_UPDATE", 2)
            self.emit("UNPACK_SEQUENCE", size)
            while size:
                self.emit("COPY", 1 + size)
                self.emit("SWAP", 2)
                self.emit("DELETE_SUBSCR")
                size -= 1
            self._pattern_helper_store_name(star_target, pc, node)
        else:
            # Otherwise, we don't care about this tuple of keys anymore:
            self.emit("POP_TOP")
            self.emit("POP_TOP")


class InlinedComprehensionState:
    def __init__(
        self,
        pushed_locals: list[str],
        fast_hidden: set[str],
        prev_temp_symbols: dict[str, int] | None,
        end: Block,
        cleanup: Block,
    ) -> None:
        self.pushed_locals = pushed_locals
        self.fast_hidden = fast_hidden
        self.prev_temp_symbols = prev_temp_symbols
        self.end = end
        self.cleanup = cleanup


class CinderCodeGenBase(CodeGenerator):
    """Specialize some methods for cinder.

    Code generator equivalent to `Python/compile.c` in Cinder.
    The base `CodeGenerator` is equivalent to upstream `Python/compile.c`.

    Code in this class should be common to all python versions.
    Inherit from this class and a concrete code generator, e.g.

    class CinderCodeGenerator310(CinderCodeGenBase, CodeGenerator310):
        ...

    Note that CinderCodeGenBase needs to come first in the list of base
    classes.
    """

    def set_qual_name(self, qualname: str) -> None:
        self._qual_name = qualname

    def getCode(self) -> CodeType:
        code = super().getCode()
        cx_set_qualname(code, self._qual_name)
        return code

    def visitAttribute(self, node: ast.Attribute) -> None:
        if isinstance(node.ctx, ast.Load) and self._is_super_call(node.value):
            self.cx_super_attribute(node)
        else:
            super().visitAttribute(node)

    def visitCall(self, node: ast.Call) -> None:
        if self._can_optimize_call(node) and self._is_super_call(
            cast(ast.Attribute, node.func).value
        ):
            self.cx_super_call(node)
        else:
            super().visitCall(node)

    # subclasses should implement these

    def cx_super_attribute(self, node: ast.Attribute) -> None:
        raise NotImplementedError()

    def cx_super_call(self, node: ast.Call) -> None:
        raise NotImplementedError()


class CodeGenerator314(CodeGenerator312):
    flow_graph = PyFlowGraph314
    _SymbolVisitor = SymbolVisitor314

    def __init__(
        self,
        parent: CodeGenerator | None,
        node: AST,
        symbols: BaseSymbolVisitor,
        graph: PyFlowGraph,
        flags: int = 0,
        optimization_lvl: int = 0,
        future_flags: int | None = None,
        name: str | None = None,
    ) -> None:
        super().__init__(
            parent, node, symbols, graph, flags, optimization_lvl, future_flags, name
        )
        self.static_attributes: set[str] = set()
        self.deferred_annotations: list[ast.AnnAssign] = []
        self.conditional_annotation_indices: list[int] = []
        self.next_conditional_annotation_index = 0

    @classmethod
    def optimize_tree(
        cls,
        optimize: int,
        tree: AST,
        string_anns: bool,
    ) -> AST:
        result = AstOptimizer314(optimize=optimize > 0, string_anns=string_anns).visit(
            tree
        )

        assert isinstance(result, AST)
        return result

    def make_with_block(self) -> Block | None:
        return None

    def get_graph_flags(
        self, func: FuncOrLambda | CompNode, func_args: ast.arguments, scope: Scope
    ) -> int:
        assert isinstance(scope, FunctionScope)
        flags = super().get_graph_flags(func, func_args, scope)

        if scope.has_docstring:
            flags |= CO_HAS_DOCSTRING

        assert isinstance(scope, FunctionScope)
        if scope.is_method:
            flags |= CO_METHOD

        return flags

    def get_docstring(
        self, func: ast.Module | ast.ClassDef | FuncOrLambda | CompNode
    ) -> str | None:
        doc = super().get_docstring(func)
        if doc is None:
            return None

        # Difference from inspect.cleandoc():
        # Do not remove leading and trailing blank lines to keep lineno.

        doc = doc.expandtabs()
        # Find minimum indentation of any non-blank lines after first line

        lines = doc.split("\n")
        margin = 1 << 31
        for line in lines[1:]:
            for i, c in enumerate(line):
                if c == "\n":
                    # empty line
                    break
                elif c != " ":
                    margin = min(margin, i)
                    break

        if margin == 1 << 31:
            margin = 0
        # remove leading white space of 1st line
        lines[0] = lines[0].lstrip(" ")
        if margin != 0:
            for i, line in enumerate(lines):
                if i != 0:
                    # remove margin of subsequent lines
                    lines[i] = line[margin:]
        doc = "\n".join(lines)

        return doc

    def prepare_type_params(self) -> None:
        pass

    def finish_type_params(
        self, has_args: bool, num_typeparam_args: int, need_swap: bool = False
    ) -> None:
        if has_args:
            self.emit("SWAP", num_typeparam_args + 1)
            self.emit("CALL", num_typeparam_args - 1)
        else:
            self.emit("PUSH_NULL")
            self.emit("CALL", 0)

    # pyre-fixme[14]: `compile_type_param_bound_or_default` overrides method defined
    #  in `CodeGenerator312` inconsistently.
    def compile_type_param_bound_or_default(
        self, bound: ast.expr, name: str, key: ast.expr, allow_starred: bool
    ) -> None:
        typevar_scope = self.symbols.scopes[key]
        # emit defaults
        self.graph.emit_with_loc("LOAD_CONST", (1,), bound)
        # pyre-ignore[6]: Expected Annotations, arguments, not expr
        outer_gen = self.setup_annotations(bound, key, typevar_scope)
        outer_gen.class_name = self.class_name
        if allow_starred and isinstance(bound, ast.Starred):
            outer_gen.visit(bound.value)
            outer_gen.emit("UNPACK_SEQUENCE", 1)
        else:
            outer_gen.visit(bound)
        outer_gen.emit("RETURN_VALUE")

        loc = self.graph.loc
        self.set_pos(bound)
        self.emit_closure(outer_gen, 0)
        self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_DEFAULTS)
        self.set_pos(loc)

    def make_type_alias_code_gen(self, node: ast.TypeAlias) -> CodeGenerator312:
        # emit default param values
        self.graph.emit_with_loc("LOAD_CONST", (1,), node)
        symbols = self.symbols

        scope = symbols.scopes[node]
        # pyre-fixme[6]: For 2nd argument expected `Union[Annotations, arguments]`
        #  but got `TypeAlias`.
        res = self.setup_annotations(node, node, scope)
        res.class_name = self.class_name
        return res

    def emit_type_alias_set_func_defaults(self, code_gen: CodeGenerator) -> None:
        code_gen.emit("SET_FUNCTION_ATTRIBUTE", 1)

    def _fastcall_helper(
        self,
        argcnt: int,
        node: ast.expr | None,
        args: list[ast.expr],
        kwargs: list[ast.keyword],
    ) -> None:
        # No * or ** args, faster calling sequence.
        for arg in args:
            self.visit(arg)
        if kwargs:
            self.visit_list(kwargs)
            self.emit("LOAD_CONST", tuple(x.arg for x in kwargs))
            self.graph.emit("CALL_KW", argcnt + len(args) + len(kwargs))
        else:
            self.emit("CALL", argcnt + len(args) + len(kwargs))

    def emit_call_kw(self, nargs: int, kwargs: tuple[str, ...]) -> None:
        if kwargs:
            self.emit("LOAD_CONST", kwargs)
            self.graph.emit("CALL_KW", nargs + len(kwargs))
        else:
            self.emit("CALL", nargs)

    def compiler_subkwargs(
        self, kwargs: list[ast.keyword], begin: int, end: int
    ) -> None:
        nkwargs = end - begin
        big = (nkwargs * 2) > STACK_USE_GUIDELINE
        if big:
            self.emit_noline("BUILD_MAP", 0)
        for i in range(begin, end):
            self.emit("LOAD_CONST", kwargs[i].arg)
            self.visit(kwargs[i].value)
            if big:
                self.emit_noline("MAP_ADD", 1)
        if not big:
            self.emit("BUILD_MAP", nkwargs)

    SUPPORTED_FUNCTION_CALL_OPS: tuple[str, ...] = ("all", "any", "tuple")

    def maybe_optimize_function_call(self, node: ast.Call) -> bool:
        end = self.newBlock("end")
        if not (
            isinstance(node.func, ast.Name)
            and len(node.args) == 1
            and not node.keywords
            and isinstance(node.args[0], ast.GeneratorExp)
            and node.func.id in self.SUPPORTED_FUNCTION_CALL_OPS
        ):
            return False

        skip_optimization = self.newBlock("skip_optimization")
        attr = node.func
        assert isinstance(attr, ast.Name)
        if attr.id == "all":
            const = all
            continue_jump_opcode = "POP_JUMP_IF_TRUE"
            initial_res = True
        elif attr.id == "any":
            const = any
            continue_jump_opcode = "POP_JUMP_IF_FALSE"
            initial_res = False
        else:
            if attr.id == "list":
                const = list
            elif attr.id == "set":
                const = set
            else:
                assert attr.id == "tuple"
                const = tuple
            continue_jump_opcode = None
            initial_res = False

        loc = self.graph.loc
        self.visit(node.func)
        self.set_pos(node.func)
        self.emit("COPY", 1)
        self.emit("LOAD_COMMON_CONSTANT", const)
        self.emit("IS_OP", 0)
        self.emit("POP_JUMP_IF_FALSE", skip_optimization)
        self.nextBlock()
        self.emit("POP_TOP")

        if const is tuple or const is list:
            self.emit("BUILD_LIST", 0)
        elif const is set:
            self.emit("BUILD_SET", 0)
        self.visit(node.args[0])
        loop = self.newBlock("loop")
        cleanup = self.newBlock("cleanup")

        self.prepare_opt_function_loop(loop, cleanup)
        if const is tuple or const is list:
            self.emit_opt_function_loop_append()
            self.emit("JUMP", loop)
        elif const is set:
            self.emit("SET_ADD", 3)
            self.emit("JUMP", loop)
        else:
            self.emit("TO_BOOL")
            self.emit(continue_jump_opcode, loop)
        self.nextBlock()

        self.emit_noline("POP_ITER")
        if const not in (tuple, list, set):
            self.emit("LOAD_CONST", not initial_res)
        self.emit("JUMP", end)

        self.nextBlock(cleanup)
        self.emit("END_FOR")
        self.emit("POP_ITER")
        if const is tuple:
            self.emit_call_intrinsic_1("INTRINSIC_LIST_TO_TUPLE")
        elif const is list or const is set:
            # already the right type
            pass
        else:
            self.emit("LOAD_CONST", initial_res)
        self.emit("JUMP", end)
        self.nextBlock(skip_optimization)

        self.set_pos(loc)
        self.graph.emit_with_loc("PUSH_NULL", 0, node.func)
        self._call_helper(0, node, node.args, node.keywords)
        self.nextBlock(end)
        return True

    def prepare_opt_function_loop(self, loop: Block, cleanup: Block) -> None:
        self.nextBlock(loop)
        self.emit("FOR_ITER", cleanup)

    def emit_opt_function_loop_append(self) -> None:
        self.emit("LIST_APPEND", 2)

    def visitCall(self, node: ast.Call) -> None:
        if not self.maybe_optimize_function_call(node):
            super().visitCall(node)

    def emit_match_jump_to_end(self, end: Block) -> None:
        self.emit_noline("JUMP", end)

    def emit_noopt_call(self, node: ast.Call) -> None:
        self.visit(node.func)
        self.graph.emit_with_loc("PUSH_NULL", 0, node.func)
        self._call_helper(0, node, node.args, node.keywords)

    def finish_opt_call(self, node: ast.Call, attr: ast.Attribute) -> None:
        loc = self.compute_start_location_to_match_attr(node, attr)
        if node.keywords:
            for kw in node.keywords:
                self.visit(kw.value)
            self.graph.emit_with_loc(
                "LOAD_CONST",
                tuple(x.arg for x in node.keywords),
                self.compute_start_location_to_match_attr(node.func, attr),
            )
            self.graph.emit_with_loc(
                "CALL_KW", len(node.args) + len(node.keywords), loc
            )
        else:
            self.graph.emit_with_loc("CALL", len(node.args), loc)

    def compile_subdict(self, node: ast.Dict, begin: int, end: int) -> None:
        n = end - begin
        big = n * 2 > STACK_USE_GUIDELINE
        if big:
            self.emit("BUILD_MAP", 0)

        for i in range(begin, end):
            key = node.keys[i]
            assert key

            self.visit(key)
            self.visit(node.values[i])
            if big:
                self.emit("MAP_ADD", 1)

        if not big:
            self.emit("BUILD_MAP", n)

    def emit_jump_eh_end(self, target: Block) -> None:
        self.emit("JUMP_NO_INTERRUPT", target)

    def make_try_body_block(self) -> Block | None:
        return None

    def emit_binary_subscr(self) -> None:
        self.emit("BINARY_OP", find_op_idx("NB_SUBSCR"))

    def emit_load_assertion_error(self, loc: AST | SrcLocation | None = None) -> None:
        if loc:
            self.graph.emit_with_loc("LOAD_COMMON_CONSTANT", AssertionError, loc)
        else:
            self.emit("LOAD_COMMON_CONSTANT", AssertionError)

    def emit_super_attribute(self, node: ast.Attribute) -> None:
        super().emit_super_attribute(node)
        loc = self.compute_start_location_to_match_attr(node, node)
        self.graph.emit_with_loc("NOP", 0, loc)

    def visitJoinedStr(self, node: ast.JoinedStr) -> None:
        if len(node.values) > STACK_USE_GUIDELINE:
            self.emit("LOAD_CONST", "")
            self.emit("LOAD_ATTR", ("join", 1))
            self.emit("BUILD_LIST")
            for value in node.values:
                self.visit(value)
                self.emit("LIST_APPEND", 1)
            self.emit("CALL", 1)
        else:
            for value in node.values:
                self.visit(value)
            if len(node.values) > 1:
                self.emit("BUILD_STRING", len(node.values))
            elif len(node.values) == 0:
                self.emit("LOAD_CONST", "")

    def visitAsyncFor(self, node: ast.AsyncFor) -> None:
        with self.conditional_block():
            start = self.newBlock("async_for_try")
            except_ = self.newBlock("except")
            end = self.newBlock("end")
            send = self.newBlock("send")

            self.visit(node.iter)
            self.graph.emit_with_loc("GET_AITER", 0, node.iter)

            self.nextBlock(start)

            self.push_async_for_loop(start, end)
            self.emit("SETUP_FINALLY", except_)
            self.emit("GET_ANEXT")
            self.emit("LOAD_CONST", None)
            self.nextBlock(send)
            self.emit_yield_from(await_=True)
            self.emit("POP_BLOCK")
            self.emit("NOT_TAKEN")
            self.visit(node.target)
            self.visitStatements(node.body)
            self.set_no_pos()
            self.emitJump(start)
            self.pop_async_for_loop()

            self.nextBlock(except_)
            self.set_pos(node.iter)
            self.emit("END_ASYNC_FOR", send)
            if node.orelse:
                self.visitStatements(node.orelse)
            self.nextBlock(end)

    def push_async_for_loop(self, start: Block, end: Block) -> None:
        self.push_loop(FOR_LOOP, start, end)

    def pop_async_for_loop(self) -> None:
        self.pop_fblock(FOR_LOOP)

    def push_inlined_comprehension_state(
        self, scope: Scope
    ) -> InlinedComprehensionState:
        end = self.newBlock("end")
        cleanup = self.newBlock("cleanup")
        temp_symbols: dict[str, int] | None = None
        fast_hidden: set[str] = set()
        pushed_locals: list[str] = []
        self.inlined_comp_depth += 1

        # iterate over names bound in the comprehension and ensure we isolate
        # them from the outer scope as needed
        for name in scope.symbols:
            compsc = scope.check_name(name)
            outsc = self.check_name(name)

            # When a comprehension is inlined into the global scope CPython loses the
            # DEF_IMPORTED flag. It'll see compsc != outsc and will replace it in the
            # top-level symbols dict w/ the version from the list comp which doesn't have
            # the flag set. We simulate this by checking if we're in the module scope and
            # always putting a value in temp_symbols, we then disable the is_import_originated
            # check for these names (this is really ugly, and CPython probably doesn't mean
            # to lose the DEF_IMPORTED flag in these cases).
            if (
                isinstance(self.scope, ModuleScope) and outsc == SC_GLOBAL_IMPLICIT
            ) or (
                compsc != outsc
                and compsc != SC_FREE
                and not (compsc == SC_CELL and outsc == SC_FREE)
            ):
                if temp_symbols is None:
                    temp_symbols = {}
                temp_symbols[name] = compsc

            if name in scope.params:
                continue
            if name in scope.defs and name not in scope.nonlocals:
                # local names bound in comprehension must be isolated from
                # outer scope; push existing value (which may be NULL if
                # not defined) on stack
                self.emit("LOAD_FAST_AND_CLEAR", name)
                if compsc == SC_CELL:
                    name_idx = (
                        # Matching PyFlowGraph._convert_DEREF offset for free var index
                        (self.graph.freevars.get_index(name) + len(self.graph.cellvars))
                        if name in self.scope.frees
                        else self.graph.cellvars.get_index(name)
                    )
                    self.emit("MAKE_CELL", name_idx)
                pushed_locals.append(name)
                self.fast_hidden.add(name)
                fast_hidden.add(name)

        if pushed_locals:
            self.emit("SWAP", len(pushed_locals) + 1)
            self.emit("SETUP_FINALLY", cleanup)

        prev_temp_symbols = self.temp_symbols
        self.temp_symbols = temp_symbols
        return InlinedComprehensionState(
            pushed_locals, fast_hidden, prev_temp_symbols, end, cleanup
        )

    def _compile_async_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        start = self.newBlock("start")
        except_ = self.newBlock("except")
        if_cleanup = self.newBlock("if_cleanup")
        send = self.newBlock("send")

        gen = comp.generators[gen_index]
        if not iter_on_stack:
            if gen_index == 0:
                self.loadName(".0")
            else:
                self.visit(gen.iter)
                self.set_pos(comp)
                if IS_3_12_8:
                    self.graph.emit_with_loc("GET_AITER", 0, gen.iter)
                else:
                    self.emit("GET_AITER")

        self.nextBlock(start)
        self.emit("SETUP_FINALLY", except_)
        self.emit("GET_ANEXT")
        self.emit("LOAD_CONST", None)
        self.nextBlock(send)
        self.emit_yield_from(await_=True)
        self.emit("POP_BLOCK")
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        depth += 1
        gen_index += 1
        elt_loc = elt
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, IterStackState.IterableInLocal
            )
        elif type is ast.GeneratorExp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit_yield(self.scopes[comp])
            self.emit("POP_TOP")
        elif type is ast.ListComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("LIST_APPEND", depth + 1)
        elif type is ast.SetComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("SET_ADD", depth + 1)
        elif type is ast.DictComp:
            assert val is not None
            self.compile_dictcomp_element(elt, val)
            elt_loc = SrcLocation(
                elt.lineno,
                val.end_lineno or 0,
                elt.col_offset,
                val.end_col_offset or 0,
            )
            self.set_pos(elt_loc)
            self.emit("MAP_ADD", depth + 1)
        else:
            raise NotImplementedError("unknown comprehension type")

        self.nextBlock(if_cleanup)
        self.set_pos(elt_loc)
        self.emitJump(start)

        self.nextBlock(except_)
        self.set_pos(comp)
        self.emit("END_ASYNC_FOR", send)

    def visitBreak(self, node: ast.Break) -> None:
        self.emit("NOP")  # for line number
        loop = self.unwind_setup_entries(preserve_tos=False, stop_on_loop=True)
        if loop is None:
            raise self.syntax_error("'break' outside loop", node)
        self.unwind_setup_entry(loop, preserve_tos=False)

        jumpblock = loop.exit
        if jumpblock is None:
            loop.exit = jumpblock = self.newBlock("break")

        self.emitJump(jumpblock)
        self.nextBlock()

    def visitFor(self, node: ast.For) -> None:
        with self.conditional_block():
            start = self.newBlock("for_start")
            body = self.newBlock("for_body")
            cleanup = self.newBlock("for_cleanup")

            setup = Entry(FOR_LOOP, start, None, None)
            self.push_fblock(setup)
            self.visit(node.iter)
            self.set_pos(node.iter)
            self.emit("GET_ITER")

            self.nextBlock(start)
            self.emit("FOR_ITER", cleanup)
            self.nextBlock(body)
            self.graph.emit_with_loc("NOP", 0, node.target)
            self.visit(node.target)
            self.visitStatements(node.body)
            self.set_no_pos()
            self.emitJump(start)
            self.nextBlock(cleanup)
            self.emit_end_for()
            self.pop_loop()

            if node.orelse:
                self.visitStatements(node.orelse)
            if setup.exit is not None:
                self.nextBlock(setup.exit)

    def visitIf(self, node: ast.If) -> None:
        with self.conditional_block():
            super().visitIf(node)

    def visitMatch(self, node: ast.Match) -> None:
        with self.conditional_block():
            super().visitMatch(node)

    def visitTry(self, node: ast.Try) -> None:
        with self.conditional_block():
            super().visitTry(node)

    # pyre-fixme[14]: `visitTryStar` overrides method defined in `CodeGenerator312`
    #  inconsistently.
    def visitTryStar(self, node: ast.Try) -> None:
        with self.conditional_block():
            # pyre-fixme[6]: For 1st argument expected `TryStar` but got `Try`.
            super().visitTryStar(node)

    def visitWith(self, node: ast.With) -> None:
        with self.conditional_block():
            super().visitWith(node)

    def visitAsyncWith(self, node: ast.AsyncWith, pos: int = 0) -> None:
        with self.conditional_block():
            super().visitAsyncWith(node, pos)

    def visitFormattedValue(self, node: ast.FormattedValue) -> None:
        self.visit(node.value)

        if node.conversion != -1:
            if node.conversion == CONV_STR:
                oparg = FVC_STR
            elif node.conversion == CONV_REPR:
                oparg = FVC_REPR
            elif node.conversion == CONV_ASCII:
                oparg = FVC_ASCII
            else:
                raise AssertionError("unknown format conversion:", node.conversion)
            self.emit("CONVERT_VALUE", oparg)

        if node.format_spec:
            self.visit(node.format_spec)
            self.emit("FORMAT_WITH_SPEC")
        else:
            self.emit("FORMAT_SIMPLE")

    def emit_unwind_with_preserve_tos(self) -> None:
        self.emit("SWAP", 3)
        self.emit("SWAP", 2)

    def emit_before_async_with(self) -> None:
        self.emit("COPY", 1)
        self.emit("LOAD_SPECIAL", "__aexit__")
        self.emit("SWAP", 2)
        self.emit("SWAP", 3)
        self.emit("LOAD_SPECIAL", "__aenter__")
        self.emit("CALL", 0)

    def emit_before_with(self) -> None:
        self.emit("COPY", 1)
        self.emit("LOAD_SPECIAL", "__exit__")
        self.emit("SWAP", 2)
        self.emit("SWAP", 3)
        self.emit("LOAD_SPECIAL", "__enter__")
        self.emit("CALL", 0)

    def emit_kwonlydefaults(self, node: FuncOrLambda) -> bool:
        default_count = 0
        for kwonly, default in zip(node.args.kwonlyargs, node.args.kw_defaults):
            if default is not None:
                default_count += 1
                self.emit("LOAD_CONST", self.mangle(kwonly.arg))
                self.visitDefault(default)

        if default_count:
            self.emit("BUILD_MAP", default_count)
            return True

        return False

    def emit_make_function(
        self, gen: CodeHolder | CodeGenerator | PyFlowGraph, qualname: str, flags: int
    ) -> None:
        if isinstance(gen, CodeGenerator):
            gen.set_qual_name(qualname)
        self.emit("LOAD_CONST", gen)
        self.emit("MAKE_FUNCTION")
        if flags & MAKE_FUNCTION_CLOSURE:
            self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_CLOSURE)
        if flags & MAKE_FUNCTION_ANNOTATIONS:
            self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_ANNOTATIONS)
        if flags & MAKE_FUNCTION_ANNOTATE:
            self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_ANNOTATE)
        if flags & MAKE_FUNCTION_KWDEFAULTS:
            self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_KWDEFAULTS)
        if flags & MAKE_FUNCTION_DEFAULTS:
            self.emit("SET_FUNCTION_ATTRIBUTE", MAKE_FUNCTION_DEFAULTS)

    def finish_function(self) -> None:
        current = self.graph.current
        assert current

        if not isinstance(self.tree, ast.Lambda) or not self.scope.generator:
            self.set_no_pos()
            self.emit("LOAD_CONST", None)
        self.emit("RETURN_VALUE")

    def emit_cond_jump(self, jump: str, label: Block) -> None:
        self.emit("TO_BOOL")
        self.emit(jump, label)

    def visitBoolOp(self, node: ast.BoolOp) -> None:
        if type(node.op) is ast.And:
            jumpi = "JUMP_IF_FALSE"
        else:
            jumpi = "JUMP_IF_TRUE"
        end = self.newBlock()
        for value in node.values[:-1]:
            self.visit(value)
            self.emit(jumpi, end)
            self.nextBlock(label="cond")
            self.emit("POP_TOP")
        self.visit(node.values[-1])
        self.nextBlock(end)

    def unaryOp(self, node: ast.UnaryOp, op: str) -> None:
        self.visit(node.operand)
        if op == "UNARY_POSITIVE":
            self.emit_call_intrinsic_1("INTRINSIC_UNARY_POSITIVE")
        elif op == "UNARY_NOT":
            self.emit("TO_BOOL")
            self.emit("UNARY_NOT")
        else:
            self.emit(op)

    def emit_call_exit_with_nones(self) -> None:
        self.emit("LOAD_CONST", None)
        self.emit("LOAD_CONST", None)
        self.emit("LOAD_CONST", None)
        self.emit("CALL", 3)

    def emit_with_except_finish(self, cleanup: Block) -> None:
        suppress = self.newBlock("suppress")
        self.emit_cond_jump("POP_JUMP_IF_TRUE", suppress)
        self.nextBlock()
        self.emit("RERAISE", 2)

        self.nextBlock(suppress)
        self.emit("POP_TOP")
        self.emit("POP_BLOCK")
        self.emit("POP_EXCEPT")
        self.emit("POP_TOP")
        self.emit("POP_TOP")
        self.emit("POP_TOP")

        exit = self.newBlock("exit")
        self.emit("JUMP_NO_INTERRUPT", exit)

        self.nextBlock(cleanup)
        self.set_no_pos()
        self.emit("COPY", 3)
        self.emit("POP_EXCEPT")
        self.emit("RERAISE", 1)

        self.nextBlock(exit)

    def _visitSequenceLoad(
        self,
        elts: list[ast.expr],
        build_op: str,
        add_op: str,
        extend_op: str,
        num_pushed: int = 0,
        is_tuple: bool = False,
    ) -> None:
        return self._visitSequenceLoadNoOpt(
            elts, build_op, add_op, extend_op, num_pushed, is_tuple
        )

    def is_constant_slice(self, node: ast.Slice) -> bool:
        return (
            (node.lower is None or isinstance(node.lower, ast.Constant))
            and (node.upper is None or isinstance(node.upper, ast.Constant))
            and (node.step is None or isinstance(node.step, ast.Constant))
        )

    def should_apply_two_element_slice_optimization(self, node: ast.Slice) -> bool:
        return super().should_apply_two_element_slice_optimization(
            node
        ) and not self.is_constant_slice(node)

    def visitSlice(self, node: ast.Slice) -> None:
        if self.is_constant_slice(node):
            s = slice(
                node.lower.value if isinstance(node.lower, ast.Constant) else None,
                node.upper.value if isinstance(node.upper, ast.Constant) else None,
                node.step.value if isinstance(node.step, ast.Constant) else None,
            )
            self.emit("LOAD_CONST", s)
            return

        super().visitSlice(node)

    def emit_call_function_ex(self, nkwelts: int) -> None:
        if nkwelts == 0:
            self.emit("PUSH_NULL")
        self.emit("CALL_FUNCTION_EX")

    def emit_end_for(self) -> None:
        self.emit_noline("END_FOR")
        self.emit_noline("POP_ITER")

    def visitWhile(self, node: ast.While) -> None:
        with self.conditional_block():
            loop = self.newBlock("while_loop")
            body = self.newBlock("while_body")
            else_ = self.newBlock("while_else")
            after = self.newBlock("while_after")

            self.push_loop(WHILE_LOOP, loop, after)

            self.nextBlock(loop)
            self.compileJumpIf(node.test, else_, False)

            self.nextBlock(body)
            self.visitStatements(node.body)
            self.emit_noline("JUMP", loop)

            self.pop_loop()
            self.nextBlock(else_)
            if node.orelse:
                self.visitStatements(node.orelse)

            self.nextBlock(after)

    def emit_yield_value_for_yield_from(self) -> None:
        self.emit("YIELD_VALUE", 1)

    def emit_load_build_class(self) -> None:
        self.emit("LOAD_BUILD_CLASS")
        self.emit("PUSH_NULL")

    def checkAnnotation(self, node: ast.AnnAssign) -> None:
        # this is used for evaluating the annotation of an annotated assign. This is
        # a nop in 3.14.
        pass

    def emit_simple_ann_assign(self, node: ast.AnnAssign) -> None:
        if not node.simple or not isinstance(self.tree, (ast.Module, ast.ClassDef)):
            return

        if self.future_flags & CO_FUTURE_ANNOTATIONS:
            # If we have a simple name in a module or class, store the annotation
            assert isinstance(node.target, ast.Name)
            self.emit_store_annotation(node.target.id, node)
            return

        self.deferred_annotations.append(node)
        if isinstance(self.scope, ModuleScope) or self.in_conditional_block:
            conditional_index = self.next_conditional_annotation_index
            self.next_conditional_annotation_index += 1
        else:
            conditional_index = -1
        self.conditional_annotation_indices.append(conditional_index)
        if conditional_index >= 0:
            self.emit(
                "LOAD_DEREF" if isinstance(self.scope, ClassScope) else "LOAD_NAME",
                "__conditional_annotations__",
            )
            self.emit("LOAD_CONST", conditional_index)
            self.emit("SET_ADD", 1)
            self.emit("POP_TOP")

    def setup_annotations(
        self,
        loc: AST | SrcLocation,
        node: ast.arguments | Annotations,
        gen_param_scope: Scope,
    ) -> CodeGenerator314:
        graph = self.flow_graph(
            gen_param_scope.name,
            self.graph.filename,
            gen_param_scope,
            flags=CO_NESTED if gen_param_scope.nested else 0,
            args=(".format",),
            kwonlyargs=(),
            starargs=(),
            optimized=1,
            docstring=None,
            firstline=loc.lineno,  # pyre-ignore[16]: no attribute lineno
            posonlyargs=1,
        )

        outer_gen = self.make_child_codegen(node, graph, name=graph.name)
        outer_gen.optimized = 1
        body = outer_gen.newBlock("body")
        outer_gen.set_pos(loc)
        outer_gen.class_name = self.class_name
        outer_gen.emit("LOAD_FAST", ".format")
        outer_gen.emit("LOAD_CONST", 2)  # _Py_ANNOTATE_FORMAT_VALUE_WITH_FAKE_GLOBALS
        outer_gen.emit("COMPARE_OP", ">")
        outer_gen.emit("POP_JUMP_IF_FALSE", body)
        outer_gen.nextBlock()
        outer_gen.emit("LOAD_COMMON_CONSTANT", NotImplementedError)
        outer_gen.emit("RAISE_VARARGS", 1)
        outer_gen.nextBlock(body)
        assert isinstance(outer_gen, CodeGenerator314)
        return outer_gen

    def leave_annotations(self, gen: CodeGenerator) -> None:
        self.emit_closure(AnnotationsCodeHolder(gen), 0)

    def emit_argannotation(
        self, id: str, annotation: ast.expr, loc: AST | SrcLocation
    ) -> None:
        mangled = self.mangle(id)
        self.graph.emit_with_loc("LOAD_CONST", mangled, loc)
        self._visitAnnotation(annotation)

    def emit_argannotations(self, args: list[ast.arg], loc: AST | SrcLocation) -> int:
        count = 0
        for arg in args:
            if arg.annotation is not None:
                self.emit_argannotation(arg.arg, arg.annotation, loc)
                count += 1
        return count

    def emit_function_annotations(
        self, loc: AST | SrcLocation, args: ast.arguments, returns: ast.expr | None
    ) -> bool:
        scope = self.scopes.get(args)
        if not scope:
            return False
        assert isinstance(scope, AnnotationScope)
        if not scope.annotations_used:
            return False

        ann_gen = self.setup_annotations(loc, args, scope)

        count = ann_gen.emit_argannotations(args.args, loc)
        count += ann_gen.emit_argannotations(args.posonlyargs, loc)
        if args.vararg and args.vararg.annotation is not None:
            ann_gen.emit_argannotation(args.vararg.arg, args.vararg.annotation, loc)
            count += 1
        count += ann_gen.emit_argannotations(args.kwonlyargs, loc)
        if args.kwarg and args.kwarg.annotation is not None:
            ann_gen.emit_argannotation(args.kwarg.arg, args.kwarg.annotation, loc)
            count += 1
        if returns is not None:
            ann_gen.emit_argannotation("return", returns, loc)
            count += 1

        ann_gen.graph.emit_with_loc("BUILD_MAP", count, loc)
        ann_gen.graph.emit_with_loc("RETURN_VALUE", 0, loc)
        self.leave_annotations(ann_gen)
        return True

    def build_annotations(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef | ast.Lambda
    ) -> int:
        if self.emit_function_annotations(
            node, node.args, None if isinstance(node, ast.Lambda) else node.returns
        ):
            return MAKE_FUNCTION_ANNOTATE
        return 0

    def emit_deferred_annotations_body(
        self,
        loc: AST | SrcLocation,
        deferred: list[ast.AnnAssign],
        conditional_indicies: list[int],
    ) -> None:
        self.graph.emit_with_loc("BUILD_MAP", 0, loc)
        for augassign, conditional_index in zip(deferred, conditional_indicies):
            assert isinstance(augassign.target, ast.Name)
            mangled = self.mangle(augassign.target.id)
            not_set = self.newBlock("not_set")
            self.set_pos(augassign)
            if conditional_index != -1:
                self.emit("LOAD_CONST", conditional_index)
                if isinstance(self.scope, ClassScope):
                    self.emit("LOAD_DEREF", "__conditional_annotations__")
                else:
                    self.emit("LOAD_GLOBAL", "__conditional_annotations__")
                self.emit("CONTAINS_OP")
                self.emit("POP_JUMP_IF_FALSE", not_set)
                self.nextBlock()

            self.visit(augassign.annotation)
            self.emit("COPY", 2)
            self.emit("LOAD_CONST", mangled)
            self.set_pos(loc)
            self.emit("STORE_SUBSCR")
            self.nextBlock(not_set)

    def process_deferred_annotations(
        self,
        loc: AST | SrcLocation,
        gen: CodeGenerator314,
        ann_scope: AnnotationScope | None,
    ) -> None:
        if not self.deferred_annotations:
            return

        assert ann_scope is not None
        need_separate_block = isinstance(self.scope, ModuleScope)
        start_block = None
        if need_separate_block:
            start_block = self.graph.current
            self.graph.current = self.graph.newBlock("annotations_setup")

        ann_gen = self.setup_annotations(loc, Annotations(gen.scope), ann_scope)

        ann_gen.emit_deferred_annotations_body(
            loc, self.deferred_annotations, self.conditional_annotation_indices
        )

        ann_gen.graph.emit_with_loc("RETURN_VALUE", 0, loc)
        self.set_pos(loc)
        self.leave_annotations(ann_gen)

        self._nameOp(
            "STORE",
            "__annotate_func__"
            if isinstance(self.scope, ClassScope)
            else "__annotate__",
        )

        if need_separate_block:
            self.graph.annotations_block = self.graph.current
            self.graph.current = start_block

    def emit_init_class_attrs(
        self, gen: CodeGenerator, node: ast.ClassDef, first_lineno: int
    ) -> None:
        gen.emit("LOAD_NAME", "__name__")
        gen.storeName("__module__")

        gen.emit("LOAD_CONST", gen.get_qual_prefix(gen) + gen.name)
        gen.storeName("__qualname__")

        gen.emit("LOAD_CONST", first_lineno)
        gen.storeName("__firstlineno__")

    def emit_pop_subject(self, case: ast.match_case) -> None:
        # Use the next location to give better locations for branch events
        self.graph.emit_with_loc("POP_TOP", 0, NEXT_LOCATION)

    def maybe_add_static_attribute_to_class(self, node: ast.Attribute) -> None:
        attr_value = node.value
        if (
            not isinstance(attr_value, ast.Name)
            or not isinstance(node.ctx, ast.Store)
            or attr_value.id != "self"
        ):
            return

        cur = self
        while cur is not None:
            if isinstance(cur.scope, ClassScope):
                assert isinstance(cur, CodeGenerator314)
                cur.static_attributes.add(node.attr)
                break
            cur = cur.parent_code_gen

    def visitAttribute(self, node: ast.Attribute) -> None:
        self.maybe_add_static_attribute_to_class(node)
        super().visitAttribute(node)

    def emit_module_return(self, node: ast.Module) -> None:
        loc = node.body[0] if node.body else SrcLocation(1, 1, 0, 0)
        self.process_deferred_annotations(loc, self, self.scope.annotations)
        super().emit_module_return(node)

    def setup_module_annotations(self, node: ast.Module) -> None:
        self.emit("ANNOTATIONS_PLACEHOLDER")
        if self.scope.has_conditional_annotations:
            self.emit("BUILD_SET", 0)
            self.emit("STORE_NAME", "__conditional_annotations__")

        if self.future_flags & CO_FUTURE_ANNOTATIONS:
            super().setup_module_annotations(node)

    def compile_body(self, gen: CodeGenerator, node: ast.ClassDef) -> None:
        assert isinstance(gen, CodeGenerator314)
        if (self.future_flags & CO_FUTURE_ANNOTATIONS) and gen.findAnn(node.body):
            gen.did_setup_annotations = True
            gen.emit("SETUP_ANNOTATIONS")
        if not node.body:
            return

        doc = gen.get_docstring(node)
        if doc is not None:
            gen.set_pos(node.body[0])
            gen.emit("LOAD_CONST", doc)
            gen.storeName("__doc__")

        self.walkClassBody(node, gen)

        if not (self.future_flags & CO_FUTURE_ANNOTATIONS):
            lineno = (
                node.lineno
                if not node.decorator_list
                else node.decorator_list[0].lineno
            )
            gen.process_deferred_annotations(
                SrcLocation(lineno, lineno, 0, 0), gen, gen.scope.annotations
            )

        attrs = list(gen.static_attributes)
        attrs.sort()
        gen.set_no_pos()
        gen.emit_noline("LOAD_CONST", tuple(attrs))
        gen.storeName("__static_attributes__")

    # pyre-ignore[11]: No TemplateStr
    def visitTemplateStr(self, node: ast.TemplateStr) -> None:
        last_was_interpolation = True
        stringslen = 0
        for value in node.values:
            # pyre-ignore[16]: No Interpolation
            if isinstance(value, ast.Interpolation):
                if last_was_interpolation:
                    self.emit("LOAD_CONST", "")
                    stringslen += 1
                last_was_interpolation = True
            else:
                self.visit(value)
                stringslen += 1
                last_was_interpolation = False
        if last_was_interpolation:
            self.emit("LOAD_CONST", "")
            stringslen += 1
        self.emit("BUILD_TUPLE", stringslen)

        interpolationslen = 0
        for value in node.values:
            # pyre-ignore[16]: No Interpolation
            if isinstance(value, ast.Interpolation):
                self.visit(value)
                interpolationslen += 1
        self.emit("BUILD_TUPLE", interpolationslen)
        self.emit("BUILD_TEMPLATE")

    # pyre-ignore[11]: No Interpolation
    def visitInterpolation(self, node: ast.Interpolation) -> None:
        self.visit(node.value)
        self.emit("LOAD_CONST", node.str)

        oparg = 2
        if node.format_spec:
            oparg += 1
            self.visit(node.format_spec)

        conversion = node.conversion
        if conversion != -1:
            match chr(conversion):
                case "s":
                    oparg |= FVC_STR << 2
                case "r":
                    oparg |= FVC_REPR << 2
                case "a":
                    oparg |= FVC_ASCII << 2
                case _:
                    raise SystemError(f"Unrecognized conversion character {conversion}")

        self.emit("BUILD_INTERPOLATION", oparg)


class CodeGenerator315(CodeGenerator314):
    flow_graph = PyFlowGraph315

    def emit_prologue(self) -> None:
        if self.scope.coroutine or self.scope.generator:
            with self.temp_lineno(self.graph.firstline):
                self.emit("RETURN_GENERATOR", 0)
                self.emit("POP_TOP", 0)
        super().emit_prologue()

    SUPPORTED_FUNCTION_CALL_OPS: tuple[str, ...] = (
        "all",
        "any",
        "tuple",
        "list",
        "set",
    )

    def unwind_setup_entry(self, e: Entry, preserve_tos: int) -> None:
        if e.kind == FOR_LOOP:
            if preserve_tos:
                self.emit("SWAP", 3)
            self.emit("POP_TOP")
            self.emit("POP_TOP")
            return
        elif e.kind == ASYNC_FOR_LOOP:
            if preserve_tos:
                self.emit_rotate_stack(2)
            self.emit("POP_TOP")
            return

        return super().unwind_setup_entry(e, preserve_tos)

    def compile_comprehension(
        self,
        node: CompNode,
        name: str,
        elt: ast.expr,
        val: ast.expr | None,
        opcode: str | None,
        oparg: object = 0,
    ) -> None:
        self.check_async_comprehension(node)

        # fetch the scope that corresponds to comprehension
        scope = self.scopes[node]
        assert isinstance(scope, GenExprScope)

        outermost = node.generators[0]
        inlined_state: InlinedComprehensionState | None = None
        iter_state: IterStackState
        if scope.inlined:
            # for inlined comprehension process with current generator
            gen = self
            gen.compile_comprehension_iter(outermost)
            inlined_state = self.push_inlined_comprehension_state(scope)
            iter_state = IterStackState.IterableOnStack
        else:
            gen = cast(CodeGenerator312, self.make_comprehension_codegen(node, name))
            if isinstance(node, ast.GeneratorExp):
                # Insert GET_ITER before RETURN_GENERATOR.
                # https://docs.python.org/3/reference/expressions.html#generator-expressions
                gen.graph.entry.insts[0:0] = [
                    Instruction("LOAD_FAST", 0, 0, outermost.iter),
                    Instruction(
                        "GET_AITER" if outermost.is_async else "GET_ITER",
                        0,
                        0,
                        outermost.iter,
                    ),
                ]
                iter_state = IterStackState.IteratorOnStack
            else:
                iter_state = IterStackState.IterableInLocal
        gen.set_pos(node)
        start = gen.newBlock("start")
        gen.setups.append(Entry(STOP_ITERATION, start, None, None))
        if opcode:
            gen.emit(opcode, oparg)
            if scope.inlined:
                gen.emit("SWAP", 2)

        assert isinstance(gen, CodeGenerator312)
        gen.compile_comprehension_generator(
            node, 0, 0, elt, val, type(node), iter_state
        )
        if inlined_state is not None:
            self.pop_fblock(STOP_ITERATION)
            self.pop_inlined_comprehension_state(scope, inlined_state)
            return

        if not isinstance(node, ast.GeneratorExp):
            gen.emit("RETURN_VALUE")

        gen.wrap_in_stopiteration_handler()
        gen.pop_fblock(STOP_ITERATION)

        self.finish_comprehension(gen, node)

    def _compile_async_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        start = self.newBlock("start")
        except_ = self.newBlock("except")
        if_cleanup = self.newBlock("if_cleanup")
        send = self.newBlock("send")

        gen = comp.generators[gen_index]
        if iter_on_stack == IterStackState.IterableInLocal:
            if gen_index == 0:
                self.loadName(".0")
            else:
                self.visit(gen.iter)
                self.set_pos(comp)
        if iter_on_stack != IterStackState.IteratorOnStack:
            self.graph.emit_with_loc("GET_AITER", 0, gen.iter)

        self.nextBlock(start)
        self.emit("SETUP_FINALLY", except_)
        self.emit("GET_ANEXT")
        self.emit("LOAD_CONST", None)
        self.nextBlock(send)
        self.emit_yield_from(await_=True)
        self.emit("POP_BLOCK")
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        depth += 1
        gen_index += 1
        elt_loc = elt
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, IterStackState.IterableInLocal
            )
        elif type is ast.GeneratorExp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit_yield(self.scopes[comp])
            self.emit("POP_TOP")
        elif type is ast.ListComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("LIST_APPEND", depth + 1)
        elif type is ast.SetComp:
            self.visit(elt)
            self.set_pos(elt)
            self.emit("SET_ADD", depth + 1)
        elif type is ast.DictComp:
            assert val is not None
            self.compile_dictcomp_element(elt, val)
            elt_loc = SrcLocation(
                elt.lineno,
                val.end_lineno or 0,
                elt.col_offset,
                val.end_col_offset or 0,
            )
            self.set_pos(elt_loc)
            self.emit("MAP_ADD", depth + 1)
        else:
            raise NotImplementedError("unknown comprehension type")

        self.nextBlock(if_cleanup)
        self.set_pos(elt_loc)
        self.emitJump(start)

        self.nextBlock(except_)
        self.set_pos(comp)
        self.emit("END_ASYNC_FOR", send)

    def _compile_sync_comprehension(
        self,
        comp: CompNode,
        gen_index: int,
        depth: int,
        elt: ast.expr,
        val: ast.expr | None,
        type: type[ast.AST],
        iter_on_stack: IterStackState,
    ) -> None:
        start = self.newBlock("start")
        skip = self.newBlock("skip")
        if_cleanup = self.newBlock("if_cleanup")
        anchor = self.newBlock("anchor")

        gen = comp.generators[gen_index]
        if iter_on_stack == IterStackState.IterableInLocal:
            if gen_index == 0:
                self.loadName(".0")
            else:
                if isinstance(gen.iter, (ast.Tuple, ast.List)):
                    elts = gen.iter.elts
                    if len(elts) == 1 and not isinstance(elts[0], ast.Starred):
                        self.visit(elts[0])
                        start = None
                if start:
                    self.set_pos(comp)
                    self.compile_comprehension_iter(gen)

        if start:
            if iter_on_stack != IterStackState.IteratorOnStack:
                self.graph.emit_with_loc("GET_ITER", 0, gen.iter)
                depth += 1
            self.nextBlock(start)
            depth += 1
            if IS_3_12_8:
                self.graph.set_pos(gen.iter)
                self.graph.emit_with_loc("FOR_ITER", anchor, gen.iter)
            else:
                self.emit("FOR_ITER", anchor)
            self.nextBlock()
        self.visit(gen.target)

        for if_ in gen.ifs:
            self.compileJumpIf(if_, if_cleanup, False)
            self.newBlock()

        gen_index += 1
        elt_loc = elt
        if gen_index < len(comp.generators):
            self.compile_comprehension_generator(
                comp, gen_index, depth, elt, val, type, IterStackState.IterableInLocal
            )
        else:
            if type is ast.GeneratorExp:
                self.set_pos(elt)
                self.visit(elt)
                self.emit_yield(self.scopes[comp])
                self.emit("POP_TOP")
            elif type is ast.ListComp:
                self.visit(elt)
                self.set_pos(elt)
                self.emit("LIST_APPEND", depth + 1)
            elif type is ast.SetComp:
                self.set_pos(elt)
                self.visit(elt)
                self.emit("SET_ADD", depth + 1)
            elif type is ast.DictComp:
                assert elt is not None and val is not None
                self.compile_dictcomp_element(elt, val)
                elt_loc = SrcLocation(
                    elt.lineno,
                    val.end_lineno or 0,
                    elt.col_offset,
                    val.end_col_offset or 0,
                )
                self.set_pos(elt_loc)
                self.emit("MAP_ADD", depth + 1)
            else:
                raise NotImplementedError("unknown comprehension type")

            self.nextBlock(skip)
        self.nextBlock(if_cleanup)
        if start:
            self.set_pos(elt_loc)
            self.emitJump(start)
            self.nextBlock(anchor)
            self.set_pos(comp)
            self.emit_end_for()

    def compile_comprehension_iter(self, gen: ast.comprehension) -> None:
        self.visit(gen.iter)

    def push_async_for_loop(self, start: Block, end: Block) -> None:
        self.push_loop(ASYNC_FOR_LOOP, start, end)

    def pop_async_for_loop(self) -> None:
        self.pop_fblock(ASYNC_FOR_LOOP)

    def prepare_opt_function_loop(self, loop: Block, cleanup: Block) -> None:
        self.emit("PUSH_NULL")  # Push NULL index for loop
        self.nextBlock(loop)
        self.emit("FOR_ITER", cleanup)

    def emit_opt_function_loop_append(self) -> None:
        self.emit("LIST_APPEND", 3)


class CinderCodeGenerator310(CinderCodeGenBase, CodeGenerator310):
    flow_graph = PyFlowGraphCinder310
    _SymbolVisitor = CinderSymbolVisitor

    def compile_comprehension(
        self,
        node: CompNode,
        name: str,
        elt: ast.expr,
        val: ast.expr | None,
        opcode: str | None,
        oparg: object = 0,
    ) -> None:
        self.check_async_comprehension(node)

        # fetch the scope that corresponds to comprehension
        scope = self.scopes[node]
        assert isinstance(scope, GenExprScope)

        if scope.inlined:
            # for inlined comprehension process with current generator
            gen = self
        else:
            gen = cast(
                CinderCodeGenerator310, self.make_comprehension_codegen(node, name)
            )
        gen.set_pos(node)

        if opcode:
            gen.emit(opcode, oparg)

        assert isinstance(gen, CinderCodeGenerator310)
        gen.compile_comprehension_generator(
            node, 0, 0, elt, val, type(node), not scope.inlined
        )

        if scope.inlined:
            # collect list of defs that were introduced by comprehension
            # note that we need to exclude:
            # - .0 parameter since it is used
            # - non-local names (typically named expressions), they are
            #   defined in enclosing scope and thus should not be deleted
            parent = scope.parent
            assert parent

            to_delete = [
                v
                for v in scope.defs
                if v != ".0" and v not in scope.nonlocals and v not in parent.cells
            ]
            # sort names to have deterministic deletion order
            to_delete.sort()
            for v in to_delete:
                self.delName(v)
            return

        if not isinstance(node, ast.GeneratorExp):
            gen.emit("RETURN_VALUE")

        gen.finish_function()

        self.finish_comprehension(gen, node)

    def emit_super_attribute(self, node: ast.Attribute) -> None:
        call = node.value
        assert isinstance(call, ast.Call)
        self.emit("LOAD_GLOBAL", "super")
        load_arg = self._emit_args_for_super(call, node.attr)
        self.emit("LOAD_ATTR_SUPER", load_arg)

    def cx_super_attribute(self, node: ast.Attribute) -> None:
        self.emit_super_attribute(node)

    def cx_super_call(self, node: ast.Call) -> None:
        attr = node.func
        assert isinstance(attr, ast.Attribute)
        self.emit("LOAD_GLOBAL", "super")

        call = attr.value
        assert isinstance(call, ast.Call)
        load_arg = self._emit_args_for_super(call, attr.attr)
        self.emit("LOAD_METHOD_SUPER", load_arg)
        for arg in node.args:
            self.visit(arg)
        self.emit("CALL_METHOD", len(node.args))


class CinderCodeGenerator312(CinderCodeGenBase, CodeGenerator312):
    flow_graph = PyFlowGraph312

    def set_qual_name(self, qualname: str) -> None:
        cast(PyFlowGraph312, self.graph).qualname = qualname

    def cx_super_attribute(self, node: ast.Attribute) -> None:
        self.emit_super_attribute(node)

    def cx_super_call(self, node: ast.Call) -> None:
        CodeGenerator312.visitCall(self, node)


class CinderCodeGenerator314(CinderCodeGenerator312, CodeGenerator314):
    flow_graph = PyFlowGraph314


class CinderCodeGenerator315(CinderCodeGenerator312, CodeGenerator315):
    flow_graph = PyFlowGraph315


def get_default_cinder_generator() -> type[CodeGenerator]:
    if sys.version_info >= (3, 15):
        return CinderCodeGenerator315
    if sys.version_info >= (3, 14):
        return CinderCodeGenerator314
    elif sys.version_info >= (3, 12):
        return CinderCodeGenerator312

    return CinderCodeGenerator310


def get_default_cpython_generator() -> type[CodeGenerator]:
    if sys.version_info >= (3, 15):
        return CodeGenerator315
    if sys.version_info >= (3, 14):
        return CodeGenerator314
    elif sys.version_info >= (3, 12):
        return CodeGenerator312

    return CodeGenerator310


def get_default_generator() -> type[CodeGenerator]:
    cinder = "cinder" in sys.version
    return get_default_cinder_generator() if cinder else get_default_cpython_generator()


def get_docstring(
    node: ast.Module | ast.ClassDef | ast.FunctionDef | ast.AsyncFunctionDef,
) -> str | None:
    if (
        node.body
        and (b0 := node.body[0])
        and isinstance(b0, ast.Expr)
        and (b0v := b0.value)
        and isinstance(b0v, ast.Constant)
        and isinstance(b0v.value, str)
    ):
        return b0v.value


CinderCodeGenerator: type[CodeGenerator] = get_default_cinder_generator()
PythonCodeGenerator: type[CodeGenerator] = get_default_generator()


class CodeHolder(Protocol):
    def getCode(self) -> CodeType: ...
    @property
    def scope(self) -> Scope: ...
    @property
    def name(self) -> str: ...


class AnnotationsCodeHolder(CodeHolder):
    def __init__(self, code_gen: CodeGenerator) -> None:
        self.code_gen = code_gen

    @property
    def tree(self) -> ast.AST:
        return self.code_gen.tree

    @property
    def scope(self) -> Scope:
        return self.code_gen.scope

    @property
    def name(self) -> str:
        return self.code_gen.name

    def getCode(self) -> CodeType:
        code = self.code_gen.getCode()
        # We want the parameter to __annotate__ to be named "format" in the
        # signature  shown by inspect.signature(), but we need to use a
        # different name (.format) in the symtable; if the name
        # "format" appears in the annotations, it doesn't get clobbered
        # by this name.
        return code.replace(co_varnames=("format",))


if __name__ == "__main__":
    for file in sys.argv[1:]:
        compileFile(file)
