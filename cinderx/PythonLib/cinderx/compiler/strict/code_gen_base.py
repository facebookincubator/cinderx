# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

import ast
import builtins
import sys
from ast import (
    AST,
    AsyncFunctionDef,
    Call,
    ClassDef,
    expr,
    FunctionDef,
    ImportFrom,
    Name,
    NodeVisitor,
    stmt,
)
from typing import Any, cast, final, Mapping

from ..consts import CO_FUTURE_ANNOTATIONS

from ..pyassem import PyFlowGraph
from ..pycodegen import CinderCodeGenBase, CodeGenerator, find_futures, FOR_LOOP
from ..symbols import BaseSymbolVisitor, FunctionScope
from .common import FIXED_MODULES, lineinfo
from .feature_extractor import _IMPLICIT_GLOBALS, FeatureExtractor


GLOBALS_HELPER_ALIAS = "<globals-helper>"


def is_mutable(node: AST) -> bool:
    return isinstance(node, Name) and node.id in ("mutable")


class FindClassDef(NodeVisitor):
    def __init__(self) -> None:
        self.has_class = False

    def check(self, node: AST) -> None:
        self.generic_visit(node)

    def visit_ClassDef(self, node: ClassDef) -> None:
        for n in node.decorator_list:
            if is_mutable(n):
                # do not consider this class for freezing
                break
        else:
            self.has_class = True
        for body_stmt in node.body:
            # also consider inner classes
            self.visit(body_stmt)

    def visit_FunctionDef(self, node: FunctionDef) -> None:
        # do not go into func body
        pass

    def visit_AsyncFunctionDef(self, node: AsyncFunctionDef) -> None:
        # do not go into func body
        pass


class ForBodyHook(ast.stmt):
    def __init__(self, node: list[ast.stmt], target: ast.expr) -> None:
        self.body: list[ast.stmt] = node
        self.target = target
        lineinfo(self, target)


class TryFinallyHook(ast.stmt):
    def __init__(self, finally_body: list[ast.stmt]) -> None:
        self.finally_body = finally_body
        # List of (handler, builtin_name)
        self.handlers_to_restore: list[tuple[str, str]] = []
        lineinfo(self, finally_body[0] if finally_body else None)


class TryHandlerBodyHook(ast.stmt):
    def __init__(self, handler_body: list[ast.stmt], tracker_name: str) -> None:
        self.handler_body = handler_body
        self.tracker_name = tracker_name
        lineinfo(self, handler_body[0] if handler_body else None)


class TryBodyHook(ast.stmt):
    def __init__(self, body: list[ast.stmt]) -> None:
        self.body = body
        lineinfo(self, body[0] if body else None)
        self.trackers: list[str] = []


def get_is_assigned_tracking_name(name: str) -> str:
    return f"<assigned:{name}>"


class StrictCodeGenBase(CinderCodeGenBase):
    flow_graph = PyFlowGraph
    class_list_name: str = "<classes>"

    def __init__(
        self,
        parent: CodeGenerator | None,
        node: AST,
        symbols: BaseSymbolVisitor,
        graph: PyFlowGraph,
        flags: int = 0,
        optimization_lvl: int = 0,
        builtins: dict[str, Any] = builtins.__dict__,
        future_flags: int | None = None,
        name: str | None = None,
    ) -> None:
        super().__init__(
            parent,
            node,
            symbols,
            graph,
            flags=flags,
            optimization_lvl=optimization_lvl,
            future_flags=future_flags,
            name=name,
        )
        self.has_class: bool = self.has_classDef(node)
        self.made_class_list = False
        self.builtins = builtins
        # record the lineno of the first toplevel statement
        # this will be the lineno for all extra generated code
        # in the beginning of the strict module
        self.first_body_node: ast.stmt | None = None
        if not parent and isinstance(node, ast.Module) and len(node.body) > 0:
            self.first_body_node = node.body[0]

        if parent and isinstance(parent, StrictCodeGenBase):
            self.feature_extractor: FeatureExtractor = parent.feature_extractor
        else:
            self.feature_extractor = FeatureExtractor(builtins, self.future_flags)
            self.feature_extractor.visit(node)

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
        builtins: dict[str, Any] = builtins.__dict__,
    ) -> StrictCodeGenBase:
        future_flags = find_futures(flags, tree)
        if ast_optimizer_enabled:
            tree = cls.optimize_tree(
                optimize, tree, bool(future_flags & CO_FUTURE_ANNOTATIONS)
            )
        s = cls._SymbolVisitor(future_flags)
        s.visit(tree)

        graph = cls.flow_graph(module_name, filename, s.scopes[tree])
        code_gen = cls(
            None,
            tree,
            s,
            graph,
            flags=flags,
            optimization_lvl=optimize,
            builtins=builtins,
            future_flags=future_flags,
        )
        code_gen._qual_name = module_name
        code_gen.visit(tree)
        return code_gen

    def is_functionScope(self) -> bool:
        """Whether the generator has any parent function scope"""
        scope = self.scope
        while scope is not None:
            if isinstance(scope, FunctionScope):
                return True
            scope = scope.parent
        return False

    def has_classDef(self, node: AST) -> bool:
        visitor = FindClassDef()
        visitor.check(node)
        return visitor.has_class

    def emit_load_fixed_methods(self) -> None:
        # load and store <strict-modules>
        self.emit("LOAD_NAME", "<fixed-modules>")
        self.emit("LOAD_CONST", "__strict__")
        self.emit("BINARY_SUBSCR")
        self.emit_dup()
        self.emit("STORE_GLOBAL", "<strict-modules>")
        self.emit("LOAD_CONST", "freeze_type")
        self.emit("BINARY_SUBSCR")
        self.emit("STORE_GLOBAL", "<freeze-type>")

    def emit_load_builtin(self, name: str) -> None:
        """
        Directly load a builtin, bypassing any local aliasing
        that may be in effect.
        """

        if name == "globals":
            self.emit("LOAD_NAME", GLOBALS_HELPER_ALIAS)
        else:
            self.emit("LOAD_NAME", "<builtins>")
            self.emit("LOAD_CONST", name)
            self.emit("BINARY_SUBSCR")

    def emit_init_globals(self) -> None:
        # Initialize the globals assignment list
        for name in self.feature_extractor.globals:
            if name in self.builtins:
                self.emit("LOAD_CONST", name in _IMPLICIT_GLOBALS)
                self.scope.add_global(get_is_assigned_tracking_name(name))
                self.emit("STORE_NAME", get_is_assigned_tracking_name(name))

            if name == "globals":
                # we need to provide access to the globals, we just grab __dict__
                # from our strict module object, which produces a new fresh snapshot
                # of the globals when accessed.
                self.emit_globals_function()

                self.emit("LOAD_NAME", GLOBALS_HELPER_ALIAS)
                self.emit("STORE_NAME", "globals")
            elif name in self.builtins and name not in _IMPLICIT_GLOBALS:
                self.emit_restore_builtin(name)

    def strictPreVisitCall(self, node: Call) -> None:
        func = node.func
        if isinstance(func, ast.Name):
            # We don't currently allow aliasing or shadowing exec/eval
            # so this check is currently sufficient.
            if func.id == "exec" or func.id == "eval":
                # exec and eval don't take keyword args, so we only need to
                # inspect the normal arguments
                if len(node.args) < 2:
                    # we're implicitly capturing globals, make it explicit so
                    # that we get our globals dictionary.  exec/eval won't be
                    # able to mutate them though.  We also need to explicitly
                    # call locals() here because the behavior of exec/eval is
                    # to use globals as locals if it is explicitly supplied.
                    def call_function(line_node: AST, name: str) -> expr:
                        load = lineinfo(ast.Name(name, ast.Load()))
                        call = lineinfo(ast.Call(load, [], []))
                        return call

                    node.args.append(call_function(node.args[0], "globals"))
                    node.args.append(call_function(node.args[0], "locals"))

    def visitCall(self, node: Call) -> None:
        self.strictPreVisitCall(node)

        super().visitCall(node)

    def emit_update_global_del_state(self, name: str, assigned: bool) -> None:
        self.emit("LOAD_CONST", assigned)
        self.storeName(get_is_assigned_tracking_name(name))

    def emit_extra_assigns(self, target: AST) -> None:
        """
        Update the global delete state if required for target.
        """
        if isinstance(target, ast.Name):
            if (
                target.id in self.builtins
                and target.id in self.feature_extractor.global_dels
                and self.feature_extractor.is_global(target.id, self.scope)
            ):
                self.emit_update_global_del_state(target.id, True)
        elif isinstance(target, (ast.List, ast.Tuple)):
            for item in target.elts:
                self.emit_extra_assigns(item)

    def visitForBodyHook(self, node: ForBodyHook) -> None:
        self.visit_list(node.body)
        self.emit_extra_assigns(node.target)

    def strictPreVisitFor(self, node: ast.For) -> None:
        node.body = [ForBodyHook(node.body, node.target)]

    def strictPostVisitFor(self, node: ast.For) -> None:
        # And undo to keep any introspection happy
        node.body = cast(ForBodyHook, node.body[0]).body

    def visitFor(self, node: ast.For) -> None:
        self.strictPreVisitFor(node)
        super().visitFor(node)
        self.strictPostVisitFor(node)

    @final
    def visitFunctionDef(self, node: ast.FunctionDef) -> None:
        super().visitFunctionDef(node)
        if self.feature_extractor.is_global(node.name, self.scope):
            self.emit_update_global_del_state(node.name, True)

    @final
    def visitAsyncFunctionDef(self, node: ast.AsyncFunctionDef) -> None:
        super().visitAsyncFunctionDef(node)
        if self.feature_extractor.is_global(node.name, self.scope):
            self.emit_update_global_del_state(node.name, True)

    @final
    def visitClassDef(self, node: ast.ClassDef) -> None:
        super().visitClassDef(node)
        if self.feature_extractor.is_global(node.name, self.scope):
            self.emit_update_global_del_state(node.name, True)

    def visitAssign(self, node: ast.Assign) -> None:
        super().visitAssign(node)
        self.strictPostVisitAssign(node)

    def strictPostVisitAssign(self, node: ast.Assign) -> None:
        for target in node.targets:
            self.emit_extra_assigns(target)

    def visitTryFinallyHook(self, node: TryFinallyHook) -> None:
        for tracker_name, builtin_name in node.handlers_to_restore:
            # if tracker_name:
            #   restore_builtin(builtin_name)
            # del tracker_name
            after = self.newBlock()
            self.loadName(tracker_name)
            self.emit("POP_JUMP_IF_FALSE", after)
            self.emit_restore_builtin(builtin_name)
            self.nextBlock(after)
            self.emit("DELETE_NAME", tracker_name)

        self.visit_list(node.finally_body)

    def visitTryHandlerBodyHook(self, node: TryHandlerBodyHook) -> None:
        self.emit("LOAD_CONST", True)
        self.storeName(node.tracker_name)
        self.visit_list(node.handler_body)

    def visitTryBodyHook(self, node: TryBodyHook) -> None:
        for tracker_name in node.trackers:
            self.emit("LOAD_CONST", False)
            self.emit("STORE_NAME", tracker_name)
        self.visit_list(node.body)

    @final
    def visitTry(self, node: ast.Try) -> None:
        def get_handler_tracker(handler: ast.ExceptHandler) -> str:
            """gets a variable name to track exception state for aliased global"""
            handler_type = handler.type
            if isinstance(handler_type, Name):
                desc = handler_type.id
            elif handler.type is None:
                desc = "bare"
            else:
                desc = "complex"

            return f"<{desc} {handler.name} at {str(id(handler))}>"

        for handler in node.handlers:
            handler_name = handler.name
            if handler_name is None or not self.feature_extractor.is_global(
                handler_name, self.scope
            ):
                continue
            if handler_name not in self.builtins:
                continue

            tracker_name = get_handler_tracker(handler)
            self.scope.add_def(tracker_name)

            if not isinstance(node.body, TryBodyHook):
                node.body = [TryBodyHook(node.body)]
            cast(TryBodyHook, node.body[0]).trackers.append(tracker_name)

            handler.body = [TryHandlerBodyHook(handler.body, tracker_name)]

            if not isinstance(node.finalbody, TryFinallyHook):
                node.finalbody = [TryFinallyHook(node.finalbody)]
            cast(TryFinallyHook, node.finalbody[0]).handlers_to_restore.append(
                (tracker_name, handler_name)
            )

        super().visitTry(node)
        return

    def emit_restore_builtin(self, name: str) -> None:
        self.emit_load_builtin(name)
        self.storeName(name)

    @final
    def visitDelete(self, node: ast.Delete) -> None:
        for target in node.targets:
            if isinstance(target, ast.Name):
                name = target.id
                if name in self.builtins and self.feature_extractor.is_global(
                    name, self.scope
                ):
                    # Transform a builtin delete into restoring the builtin value
                    # If we can potentially delete the builtin value then we
                    # also need to track whether or not it has been deleted, this
                    # gives the right error for:
                    #   min = 42
                    #   del min
                    #   del min

                    # if not is_assigned(name):
                    #   raise NameError(f"name '{name}' is not defined")
                    after = self.newBlock()
                    self.emit("LOAD_NAME", get_is_assigned_tracking_name(name))
                    self.emit("POP_JUMP_IF_TRUE", after)
                    self.emit_load_builtin("NameError")
                    self.emit("LOAD_CONST", f"name '{name}' is not defined")
                    self.emit_call_one_arg()
                    self.emit("RAISE_VARARGS", 1)
                    self.nextBlock(after)

                    self.emit_restore_builtin(name)

                    if name in self.feature_extractor.global_dels:
                        self.emit_update_global_del_state(name, False)
                    continue

            # preserve this deletion
            self.visit(target)

    def make_function(
        self, name: str, body: list[stmt], location_node: ast.AST | None = None
    ) -> None:
        # pyre-fixme[20]: Argument `args` expected.
        func = lineinfo(ast.FunctionDef(), location_node)
        func.name = name
        func.decorator_list = []
        func.returns = None
        func.type_comment = ""
        func.body = body
        # pyre-fixme[20]: Argument `args` expected.
        args = lineinfo(ast.arguments())
        func.args = args

        args.kwonlyargs = []
        args.kw_defaults = []
        args.defaults = []
        args.args = []
        args.posonlyargs = []
        args.kwarg = None
        args.vararg = None

        scope = FunctionScope(name, self.scope.module, self.scope.klass)
        scope.parent = self.scope
        self.feature_extractor.scopes[func] = scope
        self.scopes[func] = scope

        self.visitFunctionDef(func)

    def emit_globals_function(self) -> None:
        """Produces our faked out globals() which just grabs __dict__ from our
        strict module object where the actual work of collecting the globals
        comes from."""
        self.make_function(
            name=GLOBALS_HELPER_ALIAS,
            body=[
                lineinfo(
                    ast.Return(
                        lineinfo(
                            ast.Attribute(
                                lineinfo(ast.Name("<strict_module>", ast.Load())),
                                "__dict__",
                                ast.Load(),
                            )
                        )
                    )
                ),
            ],
            location_node=self.first_body_node,
        )

    def emit_append_class_list(self) -> None:
        """
        Assumes the class to append is TOS1.
        Assumes `<classes>` is at TOS
        Pops that class and append to `<classes>`
        Do not Leave `<classes>` on the stack
        """
        self.emit_rotate_stack(2)
        self.emit("LIST_APPEND", 1)
        self.emit("POP_TOP")

    def emit_create_class_list(self) -> None:
        """create and store an empty list `<classes>`"""
        self.emit("BUILD_LIST", 0)
        op = "STORE_FAST" if self.is_functionScope() else "STORE_GLOBAL"
        self.emit(op, self.class_list_name)
        self.made_class_list = True

    def emit_freeze_class_list(self) -> None:
        """
        create a for loop that iterates on all of `<classes>`
        assign the value to `<class>`, and call
        freeze_type on `<class>`
        """

        start = self.newBlock()
        anchor = self.newBlock()
        after = self.newBlock()

        self.push_loop(FOR_LOOP, start, after)
        self.emit_load_class_list()
        self.emit("GET_ITER")

        self.nextBlock(start)
        # for <class> in <classes>
        # we don't actually need to assign to <class>
        self.emit("FOR_ITER", anchor)
        self.emit("LOAD_GLOBAL", "<freeze-type>")
        # argument need to be top most
        self.emit_rotate_stack(2)
        self.emit_call_one_arg()
        # discard the result of call
        self.emit("POP_TOP")

        self.emitJump(start)
        self.nextBlock(anchor)
        self.emit_end_for()
        self.pop_loop()
        self.nextBlock(after)

    def emit_load_class_list(self) -> None:
        """load `<classes>` on top of stack"""
        op = "LOAD_FAST" if self.is_functionScope() else "LOAD_GLOBAL"
        self.emit(op, self.class_list_name)

    def find_immutability_flag(self, node: ClassDef) -> bool:
        old_size = len(node.decorator_list)
        node.decorator_list = [d for d in node.decorator_list if not is_mutable(d)]
        return old_size == len(node.decorator_list)

    def register_immutability(self, node: ClassDef, flag: bool) -> None:
        if self.has_class and flag:
            self.emit_dup()
            self.emit_load_class_list()
            self.emit_append_class_list()
        super().register_immutability(node, flag)

    def processBody(
        self,
        node: ast.FunctionDef | ast.AsyncFunctionDef | ast.Lambda,
        body: list[ast.stmt],
        gen: CodeGenerator,
    ) -> None:
        if (
            isinstance(node, (FunctionDef, AsyncFunctionDef))
            and isinstance(gen, StrictCodeGenBase)
            and gen.has_class
        ):
            # initialize the <classes> list
            if not gen.made_class_list:
                gen.emit_create_class_list()
            # create a try + finally structure where we freeze all classes
            # in the finally block
            gen.emit_try_finally(
                None,
                lambda: super(StrictCodeGenBase, self).processBody(node, body, gen),
                lambda: gen.emit_freeze_class_list(),
            )
        else:
            super().processBody(node, body, gen)

    def startModule(self) -> None:
        first_node = self.first_body_node
        if first_node:
            self.set_pos(first_node)
        self.emit_load_fixed_methods()
        self.emit_init_globals()
        if self.has_class and not self.made_class_list:
            self.emit_create_class_list()
        super().startModule()

    def emit_module_return(self, node: ast.Module) -> None:
        if self.has_class:
            self.emit_freeze_class_list()
        super().emit_module_return(node)

    def emit_import_fixed_modules(
        self, node: ImportFrom, mod_name: str, mod: Mapping[str, object]
    ) -> None:
        new_names = [n for n in node.names if n.name not in mod]
        if new_names:
            # We have names we don't know about, keep them around...
            new_node = ImportFrom(mod_name, new_names, node.level)
            super().visitImportFrom(new_node)

        # Load the module into TOS...
        self.emit("LOAD_NAME", "<fixed-modules>")
        self.emit("LOAD_CONST", mod_name)
        self.emit("BINARY_SUBSCR")  # TOS = mod

        # Store all of the imported names from the module
        for _i, name in enumerate(node.names):
            var_name = name.name
            asname = name.asname or var_name
            value = mod.get(var_name)
            if value is not None:
                # duplicate TOS (mod)
                self.emit_dup()
                # var name
                self.emit("LOAD_CONST", var_name)
                self.emit("BINARY_SUBSCR")
                self.emit("STORE_GLOBAL", asname)
        # remove TOS (mod)
        self.emit("POP_TOP")

    @final
    def visitImportFrom(self, node: ImportFrom) -> None:
        if node.level == 0 and node.module is not None and node.module in FIXED_MODULES:
            module_name = node.module
            assert module_name is not None
            mod = FIXED_MODULES[module_name]
            self.emit_import_fixed_modules(node, module_name, mod)
            return
        super().visitImportFrom(node)
