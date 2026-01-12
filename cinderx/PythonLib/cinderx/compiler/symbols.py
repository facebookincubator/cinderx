# Portions copyright (c) Meta Platforms, Inc. and affiliates.
# pyre-strict

"""Module symbol-table generator"""

from __future__ import annotations

import ast
import os
import sys
from contextlib import contextmanager
from typing import Generator, Iterable

from .consts import (
    CO_FUTURE_ANNOTATIONS,
    SC_CELL,
    SC_FREE,
    SC_GLOBAL_EXPLICIT,
    SC_GLOBAL_IMPLICIT,
    SC_LOCAL,
    SC_UNKNOWN,
)
from .misc import mangle
from .visitor import ASTVisitor

if sys.version_info[0] >= 3:
    long = int

MANGLE_LEN = 256

DEF_GLOBAL = 1
DEF_LOCAL = 2
DEF_COMP_ITER = 2
DEF_PARAM: int = 2 << 1
USE: int = 2 << 3


NodeWithTypeParams: object = None
if sys.version_info >= (3, 12):
    NodeWithTypeParams = (
        ast.FunctionDef | ast.AsyncFunctionDef | ast.ClassDef | ast.TypeAlias
    )


class TypeParams(ast.AST):
    """Artificial node to store a tuple of type params."""

    _fields = ("params",)

    # pyre-ignore[11]: Annotation `NodeWithTypeParams` is not defined as a type.
    def __init__(self, node: NodeWithTypeParams) -> None:
        params = getattr(node, "type_params", None)
        assert params, "TypeParams needs a node with a type_params field"
        first = params[0]
        last = params[-1]
        # pyre-ignore[11]: ast.type_param added in 3.12, pyre still running in 3.10 mode.
        self.params: tuple[ast.type_param] = tuple(params)
        self.lineno: int = first.lineno
        self.end_lineno: int = last.end_lineno
        self.col_offset: int = first.col_offset
        self.end_col_offset: int = last.end_col_offset

    def __eq__(self, other: object) -> bool:
        return isinstance(other, TypeParams) and self.params == other.params

    def __hash__(self) -> int:
        return hash(self.params)


class Annotations(ast.AST):
    """Artificial node to store the scope for annotations"""

    def __init__(self, parent: Scope) -> None:
        self.parent = parent

    def __eq__(self, other: object) -> bool:
        return isinstance(other, Annotations) and self.parent == other.parent

    def __hash__(self) -> int:
        return hash(self.parent)


class TypeVarDefault(ast.AST):
    """Artificial node to store the scope for a TypeParam w/ default values"""

    # pyre-ignore[11]: Annotation `ast.TypeVar` is not defined as a type.
    def __init__(self, parent: ast.TypeVar) -> None:
        self.parent: ast.TypeVar = parent

    def __eq__(self, other: object) -> bool:
        return isinstance(other, TypeVarDefault) and self.parent == other.parent

    def __hash__(self) -> int:
        return hash(self.parent)


class Scope:
    is_function_scope = False
    has_docstring = False

    # XXX how much information do I need about each name?
    def __init__(
        self, name: str, module: Scope, klass: str | None = None, lineno: int = 0
    ) -> None:
        self.name: str = name
        self.module: Scope = module
        self.lineno: int = lineno
        self.defs: dict[str, int] = {}
        self.uses: dict[str, int] = {}
        # Defs and uses
        self.symbols: dict[str, int] = {}
        self.globals: dict[str, int] = {}
        self.explicit_globals: dict[str, int] = {}
        self.nonlocals: dict[str, int] = {}
        self.params: dict[str, int] = {}
        self.frees: dict[str, int] = {}
        self.cells: dict[str, int] = {}
        self.type_params: set[str] = set()
        self.children: list[Scope] = []
        # Names imported in this scope (the symbols, not the modules)
        self.imports: set[str] = set()
        self.parent: Scope | None = None
        self.coroutine: bool = False
        self.comp_iter_target: int = 0
        self.comp_iter_expr: int = 0
        # nested is true if the class could contain free variables,
        # i.e. if it is nested within another function.
        self.nested: bool = False
        # It's possible to define a scope (class, function) at the nested level,
        # but explicitly mark it as global. Bytecode-wise, this is handled
        # automagically, but we need to generate proper __qualname__ for these.
        self.global_scope: bool = False
        self.generator: bool = False
        self.klass: str | None = None
        self.suppress_jit: bool = False
        self.can_see_class_scope: bool = False
        self.child_free: bool = False
        self.free: bool = False
        self.annotations: AnnotationScope | None = None
        self.has_conditional_annotations = False
        self.in_unevaluated_annotation = False
        if klass is not None:
            for i in range(len(klass)):
                if klass[i] != "_":
                    self.klass = klass[i:]
                    break

    def __repr__(self) -> str:
        return "<{}: {}>".format(self.__class__.__name__, self.name)

    def mangle(self, name: str) -> str:
        if self.klass is None:
            return name
        return mangle(name, self.klass)

    def add_def(self, name: str, kind: int = DEF_LOCAL) -> None:
        mangled = self.mangle(name)
        if name not in self.nonlocals:
            self.defs[mangled] = kind | self.defs.get(mangled, 1)
            self.symbols[mangled] = kind | self.defs.get(mangled, 1)

    def add_import(self, name: str) -> None:
        self.imports.add(name)

    def add_use(self, name: str) -> None:
        self.uses[self.mangle(name)] = 1
        self.symbols[self.mangle(name)] = USE

    def add_global(self, name: str) -> None:
        name = self.mangle(name)
        if name in self.uses or name in self.defs:
            pass  # XXX warn about global following def/use
        if name in self.params:
            raise SyntaxError(
                "{} in {} is global and parameter".format(name, self.name)
            )
        self.explicit_globals[name] = 1
        self.module.add_def(name, DEF_GLOBAL)
        # Seems to be behavior of Py3.5, "global foo" sets foo as
        # explicit global for module too
        self.module.explicit_globals[name] = 1

    def add_param(self, name: str) -> None:
        name = self.mangle(name)
        self.defs[name] = 1
        self.params[name] = 1
        self.symbols[name] = DEF_PARAM

    def add_type_param(self, name: str) -> None:
        name = self.mangle(name)
        if name in self.type_params:
            raise SyntaxError("duplicated type parameter: {!r}".format(name))
        self.type_params.add(name)

    def add_child(self, child: Scope) -> None:
        self.children.append(child)
        child.parent = self

    def get_children(self) -> list[Scope]:
        return self.children

    def DEBUG(self) -> None:
        print(
            self.name,
            type(self),
            self.nested and "nested" or "",
            "global scope" if self.global_scope else "",
        )
        print("\tglobals: ", self.globals)
        print("\texplicit_globals: ", self.explicit_globals)
        print("\tcells: ", self.cells)
        print("\tdefs: ", self.defs)
        print("\tuses: ", self.uses)
        print("\tfrees:", self.frees)
        print("\tnonlocals:", self.nonlocals)
        print("\tparams:", self.params)
        for child in self.children:
            child.DEBUG()

    def check_name(self, name: str) -> int:
        """Return scope of name.

        The scope of a name could be LOCAL, GLOBAL, FREE, or CELL.
        """
        if name in self.explicit_globals:
            return SC_GLOBAL_EXPLICIT
        if name in self.globals:
            return SC_GLOBAL_IMPLICIT
        if name in self.cells:
            return SC_CELL
        if name in self.frees:
            if isinstance(self, ClassScope) and name in self.defs:
                # When a class has a free variable for something it
                # defines it's not really a free variable in the class
                # scope. CPython handles this with DEF_FREE_CLASS
                return SC_LOCAL
            return SC_FREE
        if name in self.defs:
            return SC_LOCAL
        if self.nested and name in self.uses:
            return SC_FREE
        if self.nested:
            return SC_UNKNOWN
        else:
            return SC_GLOBAL_IMPLICIT

    def is_import(self, name: str) -> bool:
        return name in self.imports

    def get_free_vars(self) -> list[str]:
        return sorted(self.frees.keys())

    def update_symbols(self, bound: set[str] | None, new_free: set[str]) -> None:
        # Record not yet resolved free variables from children (if any)
        for name in new_free:
            if isinstance(self, ClassScope) and (
                name in self.defs or name in self.globals
            ):
                # Handle a free variable in a method of
                # the class that has the same name as a local
                # or global in the class scope.
                self.frees[name] = 1
                continue
            if bound and name not in bound:
                # it's a global
                continue

            # Propagate new free symbol up the lexical stack
            self.frees[name] = 1

    def local_names_include_defs(self, local_names: set[str]) -> bool:
        overlap = local_names.intersection(self.defs.keys())

        # skip non-locals as they are defined in enclosing scope
        overlap.difference_update(self.nonlocals.keys())
        return len(overlap) != 0 and overlap != {".0"}

    def analyze_cells(self, free: set[str]) -> None:
        for name in self.defs:
            if name in free and name not in self.explicit_globals:
                self.cells[name] = 1
                if name in free:
                    free.remove(name)

    def analyze_names(
        self,
        bound: set[str] | None,
        local: set[str],
        free: set[str],
        global_vars: set[str],
        class_entry: Scope | None = None,
    ) -> None:
        # Go through all the known symbols in the block and analyze them
        for name in self.explicit_globals:
            if name in self.nonlocals:
                err = SyntaxError(f"name {name} is nonlocal and global")
                err.lineno = self.lineno
                raise err
            global_vars.add(name)
            if bound and name in bound:
                bound.remove(name)

        for name in self.nonlocals:
            if not bound:
                # TODO: We should flow in and set the filename too
                err = SyntaxError(f"nonlocal declaration not allowed at module level")
                err.lineno = self.lineno
                raise err
            if name not in bound:
                err = SyntaxError(f"no binding for nonlocal '{name}' found")
                err.lineno = self.lineno
                raise err
            self.frees[name] = 1
            self.free = True
            free.add(name)

        for name in self.defs:
            if name in self.explicit_globals or name in self.globals:
                continue
            local.add(name)
            global_vars.discard(name)

        for name in self.uses:
            # If we were passed class_entry (i.e., we're in an ste_can_see_class_scope scope)
            # and the bound name is in that set, then the name is potentially bound both by
            # the immediately enclosing class namespace, and also by an outer function namespace.
            # In that case, we want the runtime name resolution to look at only the class
            # namespace and the globals (not the namespace providing the bound).
            # Similarly, if the name is explicitly global in the class namespace (through the
            # global statement), we want to also treat it as a global in this scope.
            if class_entry is not None and name != "__classdict__":
                if name in class_entry.explicit_globals:
                    self.explicit_globals[name] = 1
                    continue
                elif name in class_entry.defs and name not in class_entry.nonlocals:
                    self.globals[name] = 1
                    continue

            if name in self.defs or name in self.explicit_globals:
                continue
            if bound is not None and name in bound:
                self.frees[name] = 1
                self.free = True
                free.add(name)
            elif name in global_vars:
                self.globals[name] = 1
            else:
                if self.nested:
                    self.free = True
                self.globals[name] = 1

    def get_cell_vars(self) -> Iterable[str]:
        keys = list(self.cells.keys())
        if self.has_conditional_annotations:
            keys.append("__conditional_annotations__")
        return sorted(keys)


class ModuleScope(Scope):
    def __init__(self) -> None:
        # Set lineno to 0 so it sorted guaranteedly before any other scope
        super().__init__("global", self, lineno=0)


class FunctionScope(Scope):
    is_function_scope = True
    is_method = False
    _inline_comprehensions = False


class GenExprScope(FunctionScope):
    is_function_scope = False
    inlined = False

    __counter = 1

    def __init__(
        self, name: str, module: ModuleScope, klass: str | None = None, lineno: int = 0
    ) -> None:
        self.__counter += 1
        super().__init__(name, module, klass, lineno)
        self.add_param(".0")


class LambdaScope(FunctionScope):
    __counter = 1

    def __init__(
        self, module: ModuleScope, klass: str | None = None, lineno: int = 0
    ) -> None:
        self.__counter += 1
        super().__init__("<lambda>", module, klass, lineno=lineno)


class ClassScope(Scope):
    def __init__(self, name: str, module: ModuleScope, lineno: int = 0) -> None:
        super().__init__(name, module, name, lineno=lineno)
        self.needs_class_closure = False
        self.needs_classdict = False

    def get_cell_vars(self) -> Iterable[str]:
        yield from self.cells.keys()
        if self.needs_class_closure:
            yield "__class__"
        if self.needs_classdict:
            yield "__classdict__"


class TypeParamScope(Scope):
    pass


class TypeAliasScope(Scope):
    pass


class TypeVarBoundScope(Scope):
    pass


class AnnotationScope(Scope):
    annotations_used = False


FUNCTION_LIKE_SCOPES = (
    FunctionScope,
    TypeVarBoundScope,
    TypeParamScope,
    TypeAliasScope,
)


class BaseSymbolVisitor(ASTVisitor):
    _FunctionScope = FunctionScope
    _GenExprScope = GenExprScope
    _LambdaScope = LambdaScope

    def __init__(self, future_flags: int) -> None:
        super().__init__()
        self.future_annotations: int = future_flags & CO_FUTURE_ANNOTATIONS
        self.scopes: dict[ast.AST, Scope] = {}
        self.klass: str | None = None
        self.module = ModuleScope()
        self.in_conditional_block = False

    @contextmanager
    def conditional_block(self) -> Generator[None, None, None]:
        in_conditional_block = self.in_conditional_block
        self.in_conditional_block = True
        try:
            yield
        finally:
            self.in_conditional_block = in_conditional_block

    def enter_type_params(
        self,
        # pyre-ignore[11]: Pyre doesn't know TypeAlias
        node: ast.ClassDef | ast.FunctionDef | ast.TypeAlias | ast.AsyncFunctionDef,
        parent: Scope,
    ) -> TypeParamScope:
        # pyre-fixme[6]: For 1st argument expected `str` but got `Union[Name, str]`.
        scope = TypeParamScope(node.name, self.module, self.klass, lineno=node.lineno)
        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        parent.add_child(scope)
        scope.parent = parent
        self.scopes[TypeParams(node)] = scope
        if isinstance(parent, ClassScope):
            scope.can_see_class_scope = True
            scope.add_use("__classdict__")
            parent.add_def("__classdict__")

        if isinstance(node, ast.ClassDef):
            scope.add_def(".type_params")
            scope.add_use(".type_params")
            scope.add_def(".generic_base")
            scope.add_use(".generic_base")

        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            scope.add_def(".defaults")
            scope.add_param(".defaults")
            if node.args.kw_defaults:
                scope.add_def(".kwdefaults")
        return scope

    def visit_type_param_bound_or_default(
        self, bound_or_default: ast.expr, name: str, type_param: ast.AST, parent: Scope
    ) -> None:
        if bound_or_default:
            is_in_class = parent.can_see_class_scope
            scope = TypeVarBoundScope(name, self.module)
            scope.parent = parent
            scope.nested = True
            scope.can_see_class_scope = is_in_class
            if is_in_class:
                scope.add_use("__classdict__")

            self.visit(bound_or_default, scope)
            self.scopes[type_param] = scope
            parent.add_child(scope)

    # pyre-ignore[11]: Pyre doesn't know TypeVar
    def visitTypeVar(self, node: ast.TypeVar, parent: Scope) -> None:
        parent.add_def(node.name)
        parent.add_type_param(node.name)

        # pyre-fixme[6]: For 1st argument expected `expr` but got `Optional[expr]`.
        self.visit_type_param_bound_or_default(node.bound, node.name, node, parent)
        if default_value := getattr(node, "default_value", None):
            self.visit_type_param_bound_or_default(
                default_value, node.name, TypeVarDefault(node), parent
            )

    # pyre-ignore[11]: Pyre doesn't know TypeVarTuple
    def visitTypeVarTuple(self, node: ast.TypeVarTuple, parent: Scope) -> None:
        parent.add_def(node.name)
        parent.add_type_param(node.name)

        if default_value := getattr(node, "default_value", None):
            self.visit_type_param_bound_or_default(
                default_value, node.name, node, parent
            )

    # pyre-ignore[11]: Pyre doesn't know ParamSpec
    def visitParamSpec(self, node: ast.ParamSpec, parent: Scope) -> None:
        parent.add_def(node.name)
        parent.add_type_param(node.name)

        if default_value := getattr(node, "default_value", None):
            self.visit_type_param_bound_or_default(
                default_value, node.name, node, parent
            )

    def visitFunctionDef(self, node: ast.FunctionDef, parent: Scope) -> None:
        self._visit_func_impl(node, parent)

    def visitAsyncFunctionDef(self, node: ast.AsyncFunctionDef, parent: Scope) -> None:
        self._visit_func_impl(node, parent)

    def _visit_func_impl(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef, parent: Scope
    ) -> None:
        if node.decorator_list:
            self.visit_list(node.decorator_list, parent)
        parent.add_def(node.name)

        type_params = getattr(node, "type_params", ())
        if type_params:
            parent = self.enter_type_params(node, parent)
            for param in type_params:
                self.visit(param, parent)

        scope = self._FunctionScope(
            node.name, self.module, self.klass, lineno=node.lineno
        )
        scope.coroutine = isinstance(node, ast.AsyncFunctionDef)
        scope.parent = parent
        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        if returns := node.returns:
            if not self.future_annotations:
                self.visit(returns, parent)
        self.visit_list(node.body, scope)

        parent.add_child(scope)

    _scope_names = {
        ast.GeneratorExp: "<genexpr>",
        ast.ListComp: "<listcomp>",
        ast.DictComp: "<dictcomp>",
        ast.SetComp: "<setcomp>",
    }

    def visitAwait(self, node: ast.Await, scope: Scope) -> None:
        scope.coroutine = True
        self.visit(node.value, scope)

    def visit_gen_impl(
        self,
        node: ast.GeneratorExp | ast.ListComp | ast.DictComp | ast.SetComp,
        parent: Scope,
    ) -> None:
        scope = self._GenExprScope(
            self._scope_names[type(node)],
            self.module,
            self.klass,
            lineno=node.lineno,
        )
        scope.parent = parent

        # bpo-37757: For now, disallow *all* assignment expressions in the
        # outermost iterator expression of a comprehension, even those inside
        # a nested comprehension or a lambda expression.
        scope.comp_iter_expr = parent.comp_iter_expr
        if isinstance(node, ast.GeneratorExp):
            scope.generator = True

        if (
            parent.nested
            or isinstance(parent, FUNCTION_LIKE_SCOPES)
            or isinstance(parent, GenExprScope)
        ):
            scope.nested = True

        if isinstance(parent, ClassScope):
            scope.is_method = True

        parent.comp_iter_expr += 1
        self.visit(node.generators[0].iter, parent)
        parent.comp_iter_expr -= 1

        self.visitcomprehension(node.generators[0], scope, True)

        for comp in node.generators[1:]:
            self.visit(comp, scope, False)

        if isinstance(node, ast.DictComp):
            self.visit(node.value, scope)
            self.visit(node.key, scope)
        else:
            self.visit(node.elt, scope)

        self.scopes[node] = scope

        if scope.coroutine and not isinstance(node, ast.GeneratorExp):
            parent.coroutine = True

        parent.add_child(scope)

    # Whether to generate code for comprehensions inline or as nested scope
    # is configurable, but we compute nested scopes for them unconditionally
    # TODO: this may be not correct, check.

    def visitGeneratorExp(self, node: ast.GeneratorExp, scope: Scope) -> None:
        return self.visit_gen_impl(node, scope)

    def visitSetComp(self, node: ast.SetComp, scope: Scope) -> None:
        return self.visit_gen_impl(node, scope)

    def visitListComp(self, node: ast.ListComp, scope: Scope) -> None:
        return self.visit_gen_impl(node, scope)

    def visitDictComp(self, node: ast.DictComp, scope: Scope) -> None:
        return self.visit_gen_impl(node, scope)

    def visitcomprehension(
        self, node: ast.comprehension, scope: Scope, is_outmost: bool
    ) -> None:
        if node.is_async:
            scope.coroutine = True

        scope.comp_iter_target = 1
        self.visit(node.target, scope)
        scope.comp_iter_target = 0
        if is_outmost:
            scope.add_use(".0")
        else:
            scope.comp_iter_expr += 1
            self.visit(node.iter, scope)
            scope.comp_iter_expr -= 1
        for if_ in node.ifs:
            self.visit(if_, scope)

    def visitLambda(self, node: ast.Lambda, parent: Scope) -> None:
        scope = self._LambdaScope(self.module, self.klass, lineno=node.lineno)
        scope.parent = parent
        # bpo-37757: For now, disallow *all* assignment expressions in the
        # outermost iterator expression of a comprehension, even those inside
        # a nested comprehension or a lambda expression.
        scope.comp_iter_expr = parent.comp_iter_expr
        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        self.visit(node.body, scope)
        parent.add_child(scope)

    def _do_args(self, scope: Scope, args: ast.arguments) -> None:
        for n in args.defaults:
            self.visit(n, scope.parent)
        for n in args.kw_defaults:
            if n:
                self.visit(n, scope.parent)

        for arg in args.posonlyargs:
            name = arg.arg
            scope.add_param(name)
            if arg.annotation and not self.future_annotations:
                self.visit(arg.annotation, scope.parent)
        for arg in args.args:
            name = arg.arg
            scope.add_param(name)
            if arg.annotation and not self.future_annotations:
                self.visit(arg.annotation, scope.parent)
        for arg in args.kwonlyargs:
            name = arg.arg
            scope.add_param(name)
            if arg.annotation and not self.future_annotations:
                self.visit(arg.annotation, scope.parent)
        if vararg := args.vararg:
            scope.add_param(vararg.arg)
            if (annotation := vararg.annotation) and not self.future_annotations:
                self.visit(annotation, scope.parent)
        if kwarg := args.kwarg:
            scope.add_param(kwarg.arg)
            if (annotation := kwarg.annotation) and not self.future_annotations:
                self.visit(annotation, scope.parent)

    def visitClassDef(self, node: ast.ClassDef, parent: Scope) -> None:
        if node.decorator_list:
            self.visit_list(node.decorator_list, parent)

        parent.add_def(node.name)

        type_params = getattr(node, "type_params", ())

        if type_params:
            prev = self.klass
            self.klass = node.name
            parent = self.enter_type_params(node, parent)
            for param in type_params:
                self.visit(param, parent)
            self.klass = prev

        for kw in node.keywords:
            self.visit(kw.value, parent)

        for n in node.bases:
            self.visit(n, parent)
        scope = ClassScope(node.name, self.module, lineno=node.lineno)
        # Set parent ASAP. TODO: Probably makes sense to do that for
        # other scope types either.
        scope.parent = parent
        if type_params:
            scope.add_def("__type_params__")
            scope.add_use(".type_params")

        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        doc = ast.get_docstring(node, False)
        if doc is not None:
            scope.add_def("__doc__")
            scope.has_docstring = True
        scope.add_def("__module__")
        scope.add_def("__qualname__")
        self.scopes[node] = scope
        prev = self.klass
        self.klass = node.name
        self.visit_list(node.body, scope)
        self.klass = prev
        parent.add_child(scope)

    def visitTypeAlias(self, node: ast.TypeAlias, parent: Scope) -> None:
        self.visit(node.name, parent)

        in_class = isinstance(parent, ClassScope)
        is_generic = len(node.type_params) > 0
        alias_parent = parent
        if is_generic:
            alias_parent = self.enter_type_params(node, parent)
            self.visit_list(node.type_params, alias_parent)

        scope = TypeAliasScope(node.name.id, self.module)
        scope.klass = self.klass
        if alias_parent.nested or isinstance(alias_parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        scope.parent = alias_parent
        scope.can_see_class_scope = in_class
        if in_class:
            scope.add_use("__classdict__")
            parent.add_def("__classdict__")

        self.scopes[node] = scope
        self.visit(node.value, scope)
        alias_parent.add_child(scope)

    # name can be a def or a use
    def visitName(self, node: ast.Name, scope: Scope) -> None:
        if isinstance(node.ctx, ast.Store):
            if scope.comp_iter_target:
                # This name is an iteration variable in a comprehension,
                # so check for a binding conflict with any named expressions.
                # Otherwise, mark it as an iteration variable so subsequent
                # named expressions can check for conflicts.
                if node.id in scope.nonlocals or node.id in scope.globals:
                    raise SyntaxError(
                        f"comprehension inner loop cannot rebind assignment expression target '{node.id}'"
                    )
                scope.add_def(node.id, DEF_COMP_ITER)

            scope.add_def(node.id)
        elif isinstance(node.ctx, ast.Del):
            # We do something to var, so even if we "undefine" it, it's a def.
            # Implementation-wise, delete is storing special value (usually
            # NULL) to var.
            scope.add_def(node.id)
        else:
            scope.add_use(node.id)

            if node.id == "super" and isinstance(scope, FUNCTION_LIKE_SCOPES):
                # If super() is used, and special cell var __class__ to class
                # definition, and free var to the method. This is complicated
                # by the fact that original Python2 implementation supports
                # free->cell var relationship only if free var is defined in
                # a scope marked as "nested", which normal method in a class
                # isn't.
                scope.add_use("__class__")

    # operations that bind new names

    def visitMatch(self, node: ast.Match, scope: Scope) -> None:
        self.visit(node.subject, scope)
        with self.conditional_block():
            self.visit_list(node.cases, scope)

    def visitMatchAs(self, node: ast.MatchAs, scope: Scope) -> None:
        if node.pattern:
            self.visit(node.pattern, scope)
        if node.name:
            scope.add_def(node.name)

    def visitMatchStar(self, node: ast.MatchStar, scope: Scope) -> None:
        if node.name:
            scope.add_def(node.name)

    def visitMatchMapping(self, node: ast.MatchMapping, scope: Scope) -> None:
        self.visit_list(node.keys, scope)
        self.visit_list(node.patterns, scope)
        if node.rest:
            scope.add_def(node.rest)

    def visitNamedExpr(self, node: ast.NamedExpr, scope: Scope) -> None:
        if scope.comp_iter_expr:
            # Assignment isn't allowed in a comprehension iterable expression
            raise SyntaxError(
                "assignment expression cannot be used in a comprehension iterable expression"
            )

        name = node.target.id
        if isinstance(scope, GenExprScope):
            cur = scope
            mangled = scope.mangle(name)
            while cur:
                if isinstance(cur, GenExprScope):
                    if cur.defs.get(mangled, 0) & DEF_COMP_ITER:
                        raise SyntaxError(
                            f"assignment expression cannot rebind comprehension iteration variable '{name}'"
                        )

                elif isinstance(cur, FunctionScope):
                    # If we find a FunctionBlock entry, add as GLOBAL/LOCAL or NONLOCAL/LOCAL
                    if mangled not in cur.explicit_globals:
                        scope.frees[mangled] = 1
                        scope.nonlocals[mangled] = 1
                    else:
                        scope.explicit_globals[mangled] = 1
                        scope.add_use(mangled)
                    cur.add_def(mangled)
                    break
                elif isinstance(cur, ModuleScope):
                    scope.globals[mangled] = 1
                    scope.add_use(mangled)
                    cur.add_def(mangled)
                    break
                elif isinstance(cur, ClassScope):
                    raise SyntaxError(
                        "assignment expression within a comprehension cannot be used in a class body"
                    )
                cur = cur.parent

        self.visit(node.value, scope)
        self.visit(node.target, scope)

    def visitWhile(self, node: ast.While, scope: Scope) -> None:
        self.visit(node.test, scope)
        with self.conditional_block():
            self.visit_list(node.body, scope)
            if node.orelse:
                self.visit_list(node.orelse, scope)

    def visitFor(self, node: ast.For, scope: Scope) -> None:
        self.visit_for_impl(node, scope)

    def visitAsyncFor(self, node: ast.AsyncFor, scope: Scope) -> None:
        self.visit_for_impl(node, scope)

    def visit_for_impl(self, node: ast.For | ast.AsyncFor, scope: Scope) -> None:
        self.visit(node.target, scope)
        self.visit(node.iter, scope)
        with self.conditional_block():
            self.visit_list(node.body, scope)
            if node.orelse:
                self.visit_list(node.orelse, scope)

    def visitImportFrom(self, node: ast.ImportFrom, scope: Scope) -> None:
        for alias in node.names:
            if alias.name == "*":
                continue
            impname = alias.asname or alias.name
            scope.add_def(impname)
            scope.add_import(impname)

    def visitImport(self, node: ast.Import, scope: Scope) -> None:
        for alias in node.names:
            name = alias.name
            i = name.find(".")
            if i > -1:
                name = name[:i]
            impname = alias.asname or name
            scope.add_def(impname)
            scope.add_import(impname)

    def visitGlobal(self, node: ast.Global, scope: Scope) -> None:
        for name in node.names:
            scope.add_global(name)

    def visitNonlocal(self, node: ast.Nonlocal, scope: Scope) -> None:
        # TODO: Check that var exists in outer scope
        for name in node.names:
            scope.frees[name] = 1
            scope.nonlocals[name] = 1

    def visitAssign(self, node: ast.Assign, scope: Scope) -> None:
        """Propagate assignment flag down to child nodes.

        The Assign node doesn't itself contains the variables being
        assigned to.  Instead, the children in node.nodes are visited
        with the assign flag set to true.  When the names occur in
        those nodes, they are marked as defs.

        Some names that occur in an assignment target are not bound by
        the assignment, e.g. a name occurring inside a slice.  The
        visitor handles these nodes specially; they do not propagate
        the assign flag to their children.
        """
        for n in node.targets:
            self.visit(n, scope)
        self.visit(node.value, scope)

    def visitAnnAssign(self, node: ast.AnnAssign, scope: Scope) -> None:
        target = node.target
        if isinstance(target, ast.Name):
            if not isinstance(scope, ModuleScope) and (
                target.id in scope.nonlocals or target.id in scope.explicit_globals
            ):
                is_nonlocal = target.id in scope.nonlocals
                raise SyntaxError(
                    f"annotated name '{target.id}' can't be {'nonlocal' if is_nonlocal else 'global'}"
                )
            if node.simple or node.value:
                scope.add_def(target.id)
        else:
            self.visit(node.target, scope)
        if annotation := node.annotation:
            self.visit_annotation(annotation, scope)
        if node.value:
            self.visit(node.value, scope)

    def visit_annotation(self, annotation: ast.expr, scope: Scope) -> None:
        if not self.future_annotations:
            self.visit(annotation, scope)

    def visitSubscript(self, node: ast.Subscript, scope: Scope) -> None:
        self.visit(node.value, scope)
        self.visit(node.slice, scope)

    def visitAttribute(self, node: ast.Attribute, scope: Scope) -> None:
        self.visit(node.value, scope)

    def visitSlice(self, node: ast.Slice, scope: Scope) -> None:
        if node.lower:
            self.visit(node.lower, scope)
        if node.upper:
            self.visit(node.upper, scope)
        if node.step:
            self.visit(node.step, scope)

    def visitAugAssign(self, node: ast.AugAssign, scope: Scope) -> None:
        # If the LHS is a name, then this counts as assignment.
        # Otherwise, it's just use.
        self.visit(node.target, scope)
        if isinstance(node.target, ast.Name):
            self.visit(node.target, scope)
        self.visit(node.value, scope)

    # prune if statements if tests are false

    _const_types = str, bytes, int, long, float

    def visitIf(self, node: ast.If, scope: Scope) -> None:
        self.visit(node.test, scope)
        with self.conditional_block():
            self.visit_list(node.body, scope)
            if node.orelse:
                self.visit_list(node.orelse, scope)

    # a yield statement signals a generator

    def visitYield(self, node: ast.Yield, scope: Scope) -> None:
        scope.generator = True
        if node.value:
            self.visit(node.value, scope)

    def visitYieldFrom(self, node: ast.YieldFrom, scope: Scope) -> None:
        scope.generator = True
        if node.value:
            self.visit(node.value, scope)

    def visitTry(self, node: ast.Try, scope: Scope) -> None:
        with self.conditional_block():
            self.visit_list(node.body, scope)
            # Handle exception capturing vars
            for handler in node.handlers:
                if handler.type:
                    self.visit(handler.type, scope)
                if handler.name:
                    scope.add_def(handler.name)
                self.visit_list(handler.body, scope)
            self.visit_list(node.orelse, scope)
            self.visit_list(node.finalbody, scope)

    def visitWith(self, node: ast.With, scope: Scope) -> None:
        with self.conditional_block():
            self.visit_list(node.items, scope)
            self.visit_list(node.body, scope)

    def visitAsyncWith(self, node: ast.AsyncWith, scope: Scope) -> None:
        with self.conditional_block():
            self.visit_list(node.items, scope)
            self.visit_list(node.body, scope)


class SymbolVisitor310(BaseSymbolVisitor):
    def visit_node_with_new_scope(
        self, node: ast.Expression | ast.Interactive | ast.Module
    ) -> None:
        scope = self.module = self.scopes[node] = self.module
        body = node.body
        if isinstance(body, list):
            self.visit_list(body, scope)
        else:
            self.visit(body, scope)
        self.analyze_block(scope, free=set(), global_vars=set())

    def visitModule(self, node: ast.Module) -> None:
        doc = ast.get_docstring(node)
        if doc is not None:
            self.module.has_docstring = True
        self.visit_node_with_new_scope(node)

    def visitInteractive(self, node: ast.Interactive) -> None:
        self.visit_node_with_new_scope(node)

    def visitExpression(self, node: ast.Expression) -> None:
        self.visit_node_with_new_scope(node)

    def analyze_block(
        self,
        scope: Scope,
        free: set[str],
        global_vars: set[str],
        bound: set[str] | None = None,
        implicit_globals: set[str] | None = None,
    ) -> None:
        local: set[str] = set()
        implicit_globals_in_block: set[str] = set()
        inlinable_comprehensions = []
        # Allocate new global and bound variable dictionaries.  These
        # dictionaries hold the names visible in nested blocks.  For
        # ClassScopes, the bound and global names are initialized
        # before analyzing names, because class bindings aren't
        # visible in methods.  For other blocks, they are initialized
        # after names are analyzed.

        new_global: set[str] = set()
        new_free: set[str] = set()
        new_bound: set[str] = set()

        if isinstance(scope, ClassScope):
            new_global |= global_vars
            if bound is not None:
                new_bound |= bound

        scope.analyze_names(bound, local, free, global_vars)
        # Populate global and bound sets to be passed to children.
        if not isinstance(scope, ClassScope):
            if isinstance(scope, FUNCTION_LIKE_SCOPES):
                new_bound |= local
            if bound:
                new_bound |= bound

            new_global |= global_vars
        else:
            # Special case __class__/__classdict__
            new_bound.add("__class__")
            new_bound.add("__classdict__")

        # create set of names that inlined comprehension should never stomp on
        # collect all local defs and params
        local_names = set(scope.defs.keys()) | set(scope.uses.keys())

        # in case comprehension will be inlined, track its set of free locals separately
        all_free: set[str] = set()

        for child in scope.children:
            if child.name in scope.explicit_globals:
                child.global_scope = True

            child_free = all_free

            maybe_inline_comp = (
                isinstance(scope, FunctionScope)
                and scope._inline_comprehensions
                and isinstance(child, GenExprScope)
                and not child.generator
            )
            if maybe_inline_comp:
                child_free = set()

            self.analyze_child_block(
                child,
                new_bound,
                new_free,
                new_global,
                child_free,
                implicit_globals_in_block,
            )
            inline_comp = maybe_inline_comp and not child.child_free

            if inline_comp:
                # record the comprehension
                inlinable_comprehensions.append((child, child_free))
            elif maybe_inline_comp:
                all_free.update(child_free)
                child_free = all_free

            local_names |= child_free

        local_names |= implicit_globals_in_block

        if implicit_globals is not None:
            # merge collected implicit globals into set for the outer scope
            implicit_globals |= implicit_globals_in_block
            implicit_globals.update(scope.globals.keys())

        # collect inlinable comprehensions
        if inlinable_comprehensions:
            for comp, comp_all_free in reversed(inlinable_comprehensions):
                exists = comp.local_names_include_defs(local_names)
                if not exists:
                    # comprehension can be inlined
                    self.merge_comprehension_symbols(scope, comp, comp_all_free)
                    # remove child from parent
                    scope.children.remove(comp)
                    for c in comp.children:
                        c.parent = scope
                    # mark comprehension as inlined
                    comp.inlined = True
                all_free |= comp_all_free

        for child in scope.children:
            if child.free or child.child_free:
                scope.child_free = True

        new_free |= all_free

        if isinstance(scope, FUNCTION_LIKE_SCOPES):
            scope.analyze_cells(new_free)
        elif isinstance(scope, ClassScope):
            # drop class free
            if "__class__" in new_free:
                new_free.remove("__class__")
                scope.needs_class_closure = True
            if "__classdict__" in new_free:
                new_free.remove("__classdict__")
                scope.needs_classdict = True

        scope.update_symbols(bound, new_free)
        free |= new_free

    def analyze_child_block(
        self,
        scope: Scope,
        bound: set[str],
        free: set[str],
        global_vars: set[str],
        child_free: set[str],
        implicit_globals: set[str],
    ) -> None:
        temp_bound = set(bound)
        temp_free = set(free)
        temp_global = set(free)

        self.analyze_block(scope, temp_free, temp_global, temp_bound, implicit_globals)
        child_free |= temp_free

    def merge_comprehension_symbols(
        self, scope: Scope, comp: Scope, comp_all_free: set[str]
    ) -> None:
        # merge defs from comprehension scope into current scope
        for v in comp.defs:
            if v != ".0":
                scope.add_def(v)

        # for names that are free in comprehension
        # and not present in defs of current scope -
        # add them as free in current scope
        for d in comp.uses:
            if comp.check_name(d) == SC_FREE and d not in scope.defs:
                sc = scope.check_name(d)
                if sc == SC_UNKNOWN:
                    # name is missing in current scope - add it
                    scope.frees[d] = 1
            elif comp.check_name(d) == SC_GLOBAL_IMPLICIT:
                scope.globals[d] = 1

        # go through free names in comprehension
        # and check if current scope has corresponding def
        # if yes - name is no longer free after inlining
        for f in list(comp.frees.keys()):
            if f in scope.defs:
                comp_all_free.remove(f)

        # move names uses in comprehension to current scope
        for u in comp.uses.keys():
            if u != ".0" or u == "__classdict__":
                scope.add_use(u)

        # cell vars in comprehension become cells in current scope
        for c in comp.cells.keys():
            if c != ".0":
                scope.cells[c] = 1


# Alias for the default 3.10 visitor
SymbolVisitor = SymbolVisitor310


class CinderFunctionScope(FunctionScope):
    def __init__(
        self, name: str, module: ModuleScope, klass: str | None = None, lineno: int = 0
    ) -> None:
        super().__init__(name=name, module=module, klass=klass, lineno=lineno)
        self._inline_comprehensions: bool = bool(
            os.getenv("PYTHONINLINECOMPREHENSIONS")
        )


class CinderGenExprScope(GenExprScope, CinderFunctionScope):
    inlined = False


class CinderLambdaScope(LambdaScope, CinderFunctionScope):
    pass


class CinderSymbolVisitor(SymbolVisitor):
    _FunctionScope = CinderFunctionScope
    _GenExprScope = CinderGenExprScope
    _LambdaScope = CinderLambdaScope

    def visit_gen_impl(
        self,
        node: ast.GeneratorExp | ast.ListComp | ast.DictComp | ast.SetComp,
        parent: Scope,
    ) -> None:
        scope = self._GenExprScope(
            self._scope_names[type(node)],
            self.module,
            self.klass,
            lineno=node.lineno,
        )
        scope.parent = parent

        # bpo-37757: For now, disallow *all* assignment expressions in the
        # outermost iterator expression of a comprehension, even those inside
        # a nested comprehension or a lambda expression.
        scope.comp_iter_expr = parent.comp_iter_expr
        if isinstance(node, ast.GeneratorExp):
            scope.generator = True

        if (
            parent.nested
            or isinstance(parent, FunctionScope)
            or isinstance(parent, GenExprScope)
        ):
            scope.nested = True

        parent.comp_iter_expr += 1
        self.visit(node.generators[0].iter, parent)
        parent.comp_iter_expr -= 1

        self.visitcomprehension(node.generators[0], scope, True)

        for comp in node.generators[1:]:
            self.visit(comp, scope, False)

        if isinstance(node, ast.DictComp):
            self.visit(node.value, scope)
            self.visit(node.key, scope)
        else:
            self.visit(node.elt, scope)

        self.scopes[node] = scope

        parent.add_child(scope)

    def visitLambda(self, node: ast.Lambda, parent: Scope) -> None:
        scope = self._LambdaScope(self.module, self.klass, lineno=node.lineno)
        scope.parent = parent
        # bpo-37757: For now, disallow *all* assignment expressions in the
        # outermost iterator expression of a comprehension, even those inside
        # a nested comprehension or a lambda expression.
        scope.comp_iter_expr = parent.comp_iter_expr
        if parent.nested or isinstance(parent, FunctionScope):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        self.visit(node.body, scope)

        parent.add_child(scope)

    def visitFunctionDef(self, node: ast.FunctionDef, parent: Scope) -> None:
        self._visit_func_impl(node, parent)

    def visitAsyncFunctionDef(self, node: ast.AsyncFunctionDef, parent: Scope) -> None:
        self._visit_func_impl(node, parent)

    def _visit_func_impl(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef, parent: Scope
    ) -> None:
        if node.decorator_list:
            self.visit_list(node.decorator_list, parent)
        parent.add_def(node.name)
        scope = self._FunctionScope(
            node.name, self.module, self.klass, lineno=node.lineno
        )
        scope.coroutine = isinstance(node, ast.AsyncFunctionDef)
        scope.parent = parent
        if parent.nested or isinstance(parent, FunctionScope):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        if returns := node.returns:
            if not self.future_annotations:
                self.visit(returns, parent)
        self.visit_list(node.body, scope)

        parent.add_child(scope)


class SymbolVisitor312(BaseSymbolVisitor):
    def visit_node_with_new_scope(
        self, node: ast.Expression | ast.Interactive | ast.Module
    ) -> None:
        scope = self.module = self.scopes[node] = self.module
        body = node.body
        if isinstance(body, list):
            self.visit_list(body, scope)
        else:
            self.visit(body, scope)
        self.analyze_block(scope, free=set(), global_vars=set())

    def visitModule(self, node: ast.Module) -> None:
        self.visit_node_with_new_scope(node)

    def visitInteractive(self, node: ast.Interactive) -> None:
        self.visit_node_with_new_scope(node)

    def visitExpression(self, node: ast.Expression) -> None:
        self.visit_node_with_new_scope(node)

    # pyre-ignore[11]: TryStar added in 3.11, pyre still running in 3.10 mode.
    def visitTryStar(self, node: ast.TryStar, scope: Scope) -> None:
        # pyre-fixme[6]: For 1st argument expected `Try` but got `TryStar`.
        return self.visitTry(node, scope)

    def analyze_block(
        self,
        scope: Scope,
        free: set[str],
        global_vars: set[str],
        bound: set[str] | None = None,
        class_entry: Scope | None = None,
    ) -> None:
        local: set[str] = set()
        # Allocate new global and bound variable dictionaries.  These
        # dictionaries hold the names visible in nested blocks.  For
        # ClassScopes, the bound and global names are initialized
        # before analyzing names, because class bindings aren't
        # visible in methods.  For other blocks, they are initialized
        # after names are analyzed.

        new_global: set[str] = set()
        new_free: set[str] = set()
        new_bound: set[str] = set()
        inlined_cells: set[str] = set()

        if isinstance(scope, ClassScope):
            new_global |= global_vars
            if bound is not None:
                new_bound |= bound

        scope.analyze_names(bound, local, free, global_vars, class_entry)
        # Populate global and bound sets to be passed to children.
        if not isinstance(scope, ClassScope):
            if isinstance(scope, FUNCTION_LIKE_SCOPES):
                new_bound |= local
            if bound:
                new_bound |= bound

            new_global |= global_vars
        else:
            # Special case __class__/__classdict__
            new_bound.add("__class__")
            new_bound.add("__classdict__")

        # in case comprehension will be inlined, track its set of free locals separately
        all_free: set[str] = set()

        for child in scope.children:
            if child.name in scope.explicit_globals:
                child.global_scope = True

            child_free = all_free

            inline_comp = isinstance(child, GenExprScope) and not child.generator
            if inline_comp:
                child_free = set()

            new_class_entry = None
            if child.can_see_class_scope:
                if isinstance(scope, ClassScope):
                    new_class_entry = scope
                else:
                    new_class_entry = class_entry

            self.analyze_child_block(
                child, new_bound, new_free, new_global, child_free, new_class_entry
            )

            if inline_comp:
                assert isinstance(child, GenExprScope)
                self.inline_comprehension(scope, child, child_free, inlined_cells)

            new_free |= child_free

        for child in scope.children:
            if child.free or child.child_free:
                scope.child_free = True

        # Splice children of inlined comprehensions into our children list
        for i, child in enumerate(scope.children):
            if isinstance(child, GenExprScope) and child.inlined:
                scope.children[i : i + 1] = child.children

        new_free |= all_free

        if isinstance(scope, FUNCTION_LIKE_SCOPES):
            self.analyze_cells(scope, new_free, inlined_cells)
        elif isinstance(scope, ClassScope):
            # drop class free
            self.drop_class_free(scope, new_free)

        scope.update_symbols(bound, new_free)
        free |= new_free

    def drop_class_free(self, scope: ClassScope, new_free: set[str]) -> None:
        if "__class__" in new_free:
            new_free.remove("__class__")
            scope.needs_class_closure = True
        if "__classdict__" in new_free:
            new_free.remove("__classdict__")
            scope.needs_classdict = True

    def analyze_child_block(
        self,
        scope: Scope,
        bound: set[str],
        free: set[str],
        global_vars: set[str],
        child_free: set[str],
        class_entry: Scope | None = None,
    ) -> None:
        temp_bound = set(bound)
        temp_free = set(free)
        temp_global = set(free)

        self.analyze_block(scope, temp_free, temp_global, temp_bound, class_entry)
        child_free |= temp_free

    def analyze_cells(
        self, scope: Scope, free: set[str], inlined_cells: set[str]
    ) -> None:
        for name in scope.defs:
            if (
                name in free or name in inlined_cells
            ) and name not in scope.explicit_globals:
                scope.cells[name] = 1
                if name in free:
                    free.remove(name)

    def is_free_in_any_child(self, comp: GenExprScope, name: str) -> bool:
        for child in comp.children:
            if name in child.frees:
                return True
        return False

    def inline_comprehension(
        self,
        scope: Scope,
        comp: GenExprScope,
        comp_free: set[str],
        inlined_cells: set[str],
    ) -> None:
        # merge defs from comprehension scope into current scope
        comp.inlined = True
        for v in comp.defs:
            if v != ".0":
                scope.add_def(v)

        # for names that are free in comprehension
        # and not present in defs of current scope -
        # add them as free in current scope
        for d in comp.uses:
            if comp.check_name(d) == SC_FREE and d not in scope.defs:
                sc = scope.check_name(d)
                if sc == SC_UNKNOWN:
                    # name is missing in current scope - add it
                    scope.frees[d] = 1
            elif comp.check_name(d) == SC_GLOBAL_IMPLICIT:
                scope.globals[d] = 1

        remove_dunder_class = False
        # go through free names in comprehension
        # and check if current scope has corresponding def
        # if yes - name is no longer free after inlining
        for f in list(comp.frees.keys()):
            if f in scope.defs:
                # free vars in comprehension that are locals in outer scope can
                # now simply be locals, unless they are free in comp children,
                # or if the outer scope is a class block
                if not self.is_free_in_any_child(comp, f) and not isinstance(
                    scope, ClassScope
                ):
                    comp_free.remove(f)
            elif isinstance(scope, ClassScope) and f == "__class__":
                scope.globals[f] = 1
                remove_dunder_class = True
                comp_free.remove(f)

        # move names uses in comprehension to current scope
        for u in comp.uses.keys():
            if u != ".0" or u == "__classdict__":
                scope.add_use(u)

        # cell vars in comprehension become cells in current scope
        for c in comp.cells.keys():
            if c != ".0":
                inlined_cells.add(c)

        if remove_dunder_class:
            del comp.frees["__class__"]


class SymbolVisitor314(SymbolVisitor312):
    def enter_block(self, scope: Scope) -> None:
        if isinstance(scope, (AnnotationScope, TypeAliasScope, TypeVarBoundScope)):
            scope.add_param(".format")
            scope.add_use(".format")

    def enter_type_params(
        self,
        node: ast.ClassDef | ast.FunctionDef | ast.TypeAlias | ast.AsyncFunctionDef,
        parent: Scope,
    ) -> TypeParamScope:
        res = super().enter_type_params(node, parent)
        self.enter_block(res)
        return res

    def visitName(self, node: ast.Name, scope: Scope) -> None:
        if not scope.in_unevaluated_annotation:
            super().visitName(node, scope)

    def visit_argannotations(self, args: list[ast.arg], scope: AnnotationScope) -> None:
        for arg in args:
            if arg.annotation is not None:
                self.visit(arg.annotation, scope)
                scope.annotations_used = True

    def _do_args(self, scope: Scope, args: ast.arguments) -> None:
        for n in args.defaults:
            self.visit(n, scope.parent)
        for n in args.kw_defaults:
            if n:
                self.visit(n, scope.parent)

        for arg in args.posonlyargs:
            name = arg.arg
            scope.add_param(name)
        for arg in args.args:
            name = arg.arg
            scope.add_param(name)
        for arg in args.kwonlyargs:
            name = arg.arg
            scope.add_param(name)
        if vararg := args.vararg:
            scope.add_param(vararg.arg)
        if kwarg := args.kwarg:
            scope.add_param(kwarg.arg)

    def drop_class_free(self, scope: ClassScope, new_free: set[str]) -> None:
        super().drop_class_free(scope, new_free)
        if "__conditional_annotations__" in new_free:
            new_free.remove("__conditional_annotations__")
            scope.has_conditional_annotations = True

    def visit_annotation(self, annotation: ast.expr, scope: Scope) -> None:
        # Annotations in local scopes are not executed and should not affect the symtable
        is_unevaluated = isinstance(scope, FunctionScope)

        # Module-level annotations are always considered conditional because the module
        # may be partially executed.
        if (
            (isinstance(scope, ClassScope) and self.in_conditional_block)
            or isinstance(scope, ModuleScope)
        ) and not scope.has_conditional_annotations:
            scope.has_conditional_annotations = True
            scope.add_use("__conditional_annotations__")

        if (annotations := scope.annotations) is None:
            annotations = scope.annotations = AnnotationScope(
                "__annotate__", scope.module
            )
            if scope.parent is not None and (
                scope.parent.nested or isinstance(scope.parent, FUNCTION_LIKE_SCOPES)
            ):
                annotations.nested = True
            annotations.parent = scope
            if not self.future_annotations:
                scope.children.append(annotations)
            self.scopes[Annotations(scope)] = annotations
            if isinstance(scope, ClassScope) and not self.future_annotations:
                annotations.can_see_class_scope = True
                annotations.add_use("__classdict__")

        if is_unevaluated:
            annotations.in_unevaluated_annotation = True

        self.visit(annotation, annotations)

        if is_unevaluated:
            annotations.in_unevaluated_annotation = False

    def visit_annotations(
        self,
        args: ast.arguments,
        returns: ast.expr | None,
        parent: Scope,
    ) -> AnnotationScope:
        scope = AnnotationScope("__annotate__", self.module, self.klass)
        self.scopes[args] = scope

        if not self.future_annotations:
            # If "from __future__ import annotations" is active,
            # annotation blocks shouldn't have any affect on the symbol table since in
            # the compilation stage, they will all be transformed to strings.
            parent.add_child(scope)
        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True

        scope.parent = parent
        self.enter_block(scope)
        is_in_class = parent.can_see_class_scope
        if is_in_class or isinstance(parent, ClassScope):
            scope.can_see_class_scope = True
            scope.add_use("__classdict__")

        if args.posonlyargs:
            self.visit_argannotations(args.posonlyargs, scope)
        if args.args:
            self.visit_argannotations(args.args, scope)
        if args.vararg and args.vararg.annotation:
            scope.annotations_used = True
            self.visit(args.vararg, scope)
        if args.kwarg and args.kwarg.annotation:
            scope.annotations_used = True
            self.visit(args.kwarg, scope)
        if args.kwonlyargs:
            self.visit_argannotations(args.kwonlyargs, scope)
        if returns:
            scope.annotations_used = True
            self.visit(returns, scope)
        return scope

    def visitLambda(self, node: ast.Lambda, parent: Scope) -> None:
        scope = self._LambdaScope(self.module, self.klass, lineno=node.lineno)
        scope.parent = parent

        # bpo-37757: For now, disallow *all* assignment expressions in the
        # outermost iterator expression of a comprehension, even those inside
        # a nested comprehension or a lambda expression.
        scope.comp_iter_expr = parent.comp_iter_expr
        if parent.nested or isinstance(parent, FunctionScope):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        self.visit(node.body, scope)
        if isinstance(parent, ClassScope):
            scope.is_method = True

        parent.add_child(scope)

    def _visit_func_impl(
        self, node: ast.FunctionDef | ast.AsyncFunctionDef, parent: Scope
    ) -> None:
        if node.decorator_list:
            self.visit_list(node.decorator_list, parent)
        parent.add_def(node.name)

        type_params = getattr(node, "type_params", ())
        if type_params:
            parent = self.enter_type_params(node, parent)
            for param in type_params:
                self.visit(param, parent)

        scope = self._FunctionScope(
            node.name, self.module, self.klass, lineno=node.lineno
        )

        doc = ast.get_docstring(node)
        if doc is not None:
            scope.has_docstring = True

        if isinstance(parent, ClassScope):
            scope.is_method = True

        scope.annotations = self.visit_annotations(node.args, node.returns, parent)

        scope.coroutine = isinstance(node, ast.AsyncFunctionDef)
        scope.parent = parent
        if parent.nested or isinstance(parent, FUNCTION_LIKE_SCOPES):
            scope.nested = True
        self.scopes[node] = scope
        self._do_args(scope, node.args)
        self.visit_list(node.body, scope)

        parent.add_child(scope)


class SymbolVisitor315(SymbolVisitor314):
    pass
