# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

from __future__ import annotations

from ast import (
    AnnAssign,
    Assign,
    AST,
    AsyncFor,
    AsyncFunctionDef,
    AsyncWith,
    ClassDef,
    For,
    FunctionDef,
    If,
    Import,
    ImportFrom,
    Name,
    Try,
    While,
    With,
)
from typing import TYPE_CHECKING, Union

from .module_table import DeferredImport, ModuleTable
from .types import (
    AwaitableTypeRef,
    Class,
    DecoratedMethod,
    Function,
    InitSubclassFunction,
    ResolvedTypeRef,
    TypeEnvironment,
    TypeName,
    TypeRef,
    UnknownDecoratedMethod,
)
from .util import make_qualname, sys_hexversion_check
from .visitor import GenericVisitor

if TYPE_CHECKING:
    from .compiler import Compiler


class NestedScope:
    def __init__(self, parent_qualname: str | None) -> None:
        self.qualname: str = make_qualname(parent_qualname, "<nested>")

    def declare_class(self, node: AST, klass: Class) -> None:
        pass

    def declare_function(self, func: Function | DecoratedMethod) -> None:
        pass

    def declare_variable(self, node: AnnAssign, module: ModuleTable) -> None:
        pass

    def declare_variables(self, node: Assign, module: ModuleTable) -> None:
        pass


TScopeTypes = Union[ModuleTable, Class, Function, NestedScope]


class DeclarationVisitor(GenericVisitor[None]):
    def __init__(
        self, mod_name: str, filename: str, symbols: Compiler, optimize: int
    ) -> None:
        module = symbols[mod_name] = ModuleTable(
            mod_name, filename, symbols, first_pass_done=False
        )
        super().__init__(module)
        self.scopes: list[TScopeTypes] = [self.module]
        self.optimize = optimize
        self.compiler = symbols
        self.type_env: TypeEnvironment = symbols.type_env

    def finish_bind(self) -> None:
        self.module.finish_bind()

    def parent_scope(self) -> TScopeTypes:
        return self.scopes[-1]

    def enter_scope(self, scope: TScopeTypes) -> None:
        self.scopes.append(scope)

    def exit_scope(self) -> None:
        self.scopes.pop()

    def qualify_name(self, name: str) -> str:
        return make_qualname(self.parent_scope().qualname, name)

    def make_nested_scope(self) -> NestedScope:
        return NestedScope(self.parent_scope().qualname)

    def enter_nested_scope(self) -> None:
        self.enter_scope(self.make_nested_scope())

    def visitAnnAssign(self, node: AnnAssign) -> None:
        self.parent_scope().declare_variable(node, self.module)

    def visitAssign(self, node: Assign) -> None:
        self.parent_scope().declare_variables(node, self.module)

    def visitClassDef(self, node: ClassDef) -> None:
        parent_scope = self.parent_scope()
        qualname = make_qualname(parent_scope.qualname, node.name)

        bases = [
            self.module.resolve_type(base, qualname) or self.type_env.dynamic
            for base in node.bases
        ]
        if not bases:
            bases.append(self.type_env.object)

        with self.compiler.error_sink.error_context(self.filename, node):
            klasses = []
            for base in bases:
                klasses.append(
                    base.make_subclass(
                        TypeName(self.module_name, qualname),
                        bases,
                    )
                )
            for cur_type in klasses:
                if type(cur_type) is not type(klasses[0]):
                    self.syntax_error("Incompatible subtypes", node)
            klass = klasses[0]

        for base in bases:
            if base is self.type_env.named_tuple:
                # In named tuples, the fields are actually elements
                # of the tuple, so we can't do any advanced binding against it.
                klass = self.type_env.dynamic
                break

            if base is self.type_env.protocol:
                # Protocols aren't guaranteed to exist in the actual MRO, so let's treat
                # them as dynamic to force dynamic dispatch.
                klass = self.type_env.dynamic
                break

            if base is self.type_env.typed_dict:
                # TODO(T121706684) Supporting typed dicts is tricky, similar
                # to protocols and named tuples
                klass = self.type_env.dynamic
                break

            if base.is_final:
                self.syntax_error(
                    f"Class `{klass.instance.name}` cannot subclass a Final class: `{base.instance.name}`",
                    node,
                )

        # we can't statically load classes nested inside functions
        if not isinstance(parent_scope, (ModuleTable, Class)):
            klass = self.type_env.dynamic

        self.enter_scope(
            self.make_nested_scope() if klass is self.type_env.dynamic else klass
        )
        for item in node.body:
            with self.compiler.error_sink.error_context(self.filename, item):
                self.visit(item)
        self.exit_scope()

        for d in reversed(node.decorator_list):
            if klass is self.type_env.dynamic:
                break
            with self.compiler.error_sink.error_context(self.filename, d):
                decorator = self.module.resolve_decorator(d) or self.type_env.dynamic
                klass = decorator.resolve_decorate_class(klass, d, self)

        parent_scope.declare_class(node, klass.exact_type())
        # We want the name corresponding to `C` to be the exact type when imported.
        self.module.types[node] = klass.exact_type()

    def _visitFunc(self, node: FunctionDef | AsyncFunctionDef) -> None:
        function = self._make_function(node)
        self.parent_scope().declare_function(function)

    def _make_function(self, node: FunctionDef | AsyncFunctionDef) -> Function:
        qualname = self.qualify_name(node.name)
        if node.name == "__init_subclass__":
            func = InitSubclassFunction(
                node, self.module, self.return_type_ref(node, qualname)
            )
            parent_scope = self.parent_scope()
            if isinstance(parent_scope, Class):
                parent_scope.has_init_subclass = True
        else:
            func = Function(node, self.module, self.return_type_ref(node, qualname))
        self.enter_scope(func)
        for item in node.body:
            self.visit(item)
        self.exit_scope()

        func_type = func
        if node.decorator_list:
            # Since we haven't resolved decorators yet (until finish_bind), we
            # don't know what type we should ultimately set for this node;
            # Function.finish_bind() will likely override this.
            func_type = UnknownDecoratedMethod(func)

        self.module.types[node] = func_type
        return func

    def visitFunctionDef(self, node: FunctionDef) -> None:
        self._visitFunc(node)

    def visitAsyncFunctionDef(self, node: AsyncFunctionDef) -> None:
        self._visitFunc(node)

    def return_type_ref(
        self, node: FunctionDef | AsyncFunctionDef, requester: str
    ) -> TypeRef:
        ann = node.returns
        if not ann:
            res = ResolvedTypeRef(self.type_env.dynamic)
        else:
            res = TypeRef(self.module, requester, ann)
        if isinstance(node, AsyncFunctionDef):
            res = AwaitableTypeRef(res, self.module.compiler)
        return res

    def visitImport(self, node: Import) -> None:
        for name in node.names:
            asname = name.asname
            if asname is None:
                top_level_module = name.name.split(".")[0]
                self.module.declare_import(
                    top_level_module,
                    None,
                    DeferredImport(
                        self.module,
                        name.name,
                        top_level_module,
                        self.optimize,
                        self.compiler,
                        mod_to_return=top_level_module,
                    ),
                )
            else:
                self.module.declare_import(
                    asname,
                    None,
                    DeferredImport(
                        self.module, name.name, asname, self.optimize, self.compiler
                    ),
                )

    def visitImportFrom(self, node: ImportFrom) -> None:
        mod_name = node.module
        if not mod_name or node.level:
            raise NotImplementedError("relative imports aren't supported")
        for name in node.names:
            child_name = name.asname or name.name
            self.module.declare_import(
                child_name,
                (mod_name, name.name),
                DeferredImport(
                    self.module,
                    mod_name,
                    name.name,
                    self.optimize,
                    self.compiler,
                    name.name,
                ),
            )

    # We don't pick up declarations in nested statements
    def visitFor(self, node: For) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()

    def visitAsyncFor(self, node: AsyncFor) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()

    def visitWhile(self, node: While) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()

    def visitIf(self, node: If) -> None:
        test = node.test
        if isinstance(test, Name) and test.id == "TYPE_CHECKING":
            self.visit(node.body)
        else:
            result = sys_hexversion_check(node)
            # We should check the version if provided
            if result is not None:
                self.module.mark_known_boolean_test(test, value=bool(result))
                if result:
                    self.visit(node.body)
                else:
                    self.visit(node.orelse)
                return
            else:
                self.enter_nested_scope()
                self.visit(node.body)
                self.exit_scope()

        if node.orelse:
            self.enter_nested_scope()
            self.visit(node.orelse)
            self.exit_scope()

    def visitWith(self, node: With) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()

    def visitAsyncWith(self, node: AsyncWith) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()

    def visitTry(self, node: Try) -> None:
        self.enter_nested_scope()
        self.generic_visit(node)
        self.exit_scope()
