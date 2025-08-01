# Static Python Tutorial

## Getting Started

Static Python is still under development, and there are a lot of rough
edges, including probably bugs that can crash the interpreter.

Getting full benefit from Static Python (with cross-module compilation)
requires a module loader able to detect Static Python modules based on
some marker (we use the presence of `import __static__`) and compile
them using the Static Python bytecode compiler. Such a loader is
included (at
`compiler.strict.loader.StrictSourceFileLoader`) and you can
install it by calling `compiler.strict.loader.install()` in
the `main` module of your program (before anything else is imported.)
Note this means the main module itself cannot be Static Python. You can
also just set the `PYTHONINSTALLSTRICTLOADER` environment variable to a
nonzero value, and the loader will be installed for you.

Once you've installed the loader, any module with `import __static__`
as its first line of code (barring optional docstring and optional
`__future__` imports) will be compiled as Static Python. (You can also
use `import __strict__` if you just want Strict Module semantics --
immutable modules that can\'t have side effects at import time --
without Static Python. `import __static__` also implies Strict, so you
should never use both.)

It is also possible to try out Static Python on simple examples by
running `./python -m compiler --static somemod.py`. This will compile
and execute `somemod.py` as Static Python. Add `--dis` to also dump a
disassembly of the emitted bytecode, and add `--strict` to compile the
module as an immutable StrictModule, which enables some additional
optimizations. Add `-X jit` to also enable the Cinder JIT to get maximum
performance. You can also use `-c --output somemod.pyc` to output a
compiled `.pyc` file instead of executing the module.

## `__static__` imports reference

You may see some unfamiliar imports from a new `__static__` module in
modules using Static Python. This reference should clarify their
purpose.

### `from __static__ import cbool, int8, uint8, int16, uint16, int32, uint32, int64, uint64, char, double`

These are primitive types, or C types. They can be used as type
annotations in Static Python modules to signal to the Cinder JIT that it
can use unboxed C types for these values. The static compiler
automatically interprets literals appropriately if in a primitive type
context; e.g. `x: cbool = True` will create a primitive boolean, not a
Python `True` (despite the RHS appearing to be Python `True`),
`y: int64 = 3` or `int64(3)` will create a primitive `int64` with value
`3`, etc. (For literals, this happens in the compiler, so at runtime we
are not creating a boxed Python integer object and then unboxing the
value from it, we directly just create the primitive value.) You can
also at runtime unbox a Python object to a primitive value with e.g.
`int64(some_python_int)`. This might raise `TypeError` at runtime (if
`some_python_int` is not actually an int), or it might raise
`OverflowError` (if the python int is too large for the target primitive
size.)

Some things to know about primitives:

1.  Performance will generally be better. Creation and reference
    counting and destruction of Python objects is inherently expensive,
    and all these costs can be eliminated. In particular arithmetic with
    primitive values (which can directly translate to assembly
    arithmetic instructions) will perform much better than arithmetic
    with dynamic Python objects.
2.  Unlike Python's numeric types, primitive `intxx` and `double` have
    limited bit width and can overflow, and you are responsible to avoid
    this, just as when writing C code. Currently overflow is undefined
    behavior (usually silent wraparound, in some cases `OverflowError`);
    in the future we aim to make it always raise `OverflowError`.
3.  To avoid unnecessary creation of expensive Python objects in hot
    paths, comparing two primitives produces a `cbool` not a Python
    `bool`. E.g. if `x: int64 = 3` and `y: int64 = 4`, then after
    `z = (x == y)`, `z` will have type `cbool`.
4.  Primitive types cannot mix with Python types or with each other;
    their type must always be known exactly by the static compiler. The
    compiler will error if you ever try to have a value (even
    transiently) of `Union` type including a primitive type as part of
    the union. One notable case where this might bite you is due to
    primitive comparisons producing `cbool` and the short-circuiting
    behavior of Python's `and` and `or` operators. This means that code
    such as `a_prim == b_prim or x_py_int > y_py_int` is illegal,
    because the first compare will produce a `cbool` and the second will
    produce a `bool`, and the overall expression might produce either
    one depending whether the first compare is true or false. In such
    cases you must either explicitly box or unbox some of the compares
    so that the chain all produce the same type, or split it into
    separate (maybe nested) `if` statements.
5.  You can pass primitive types as arguments and return values in
    function calls within static Python. If a non-static function calls
    a Static Python function that takes primitive arguments, it can pass
    the Python equivalent type (`float` for `double`, `int` for
    `[u]intxx`, `bool` for `cbool`) and the value will be implicitly
    unboxed (possibly raising `OverflowError`). Similarly, static Python
    functions returning primitive values to a non-static caller will
    implicitly box the value to the corresponding Python type.
6.  You can index into lists, tuples etc using primitive integers. In
    the general case this is equivalent to `l[box(an_int64)]` but for
    actual builtin sequence types it is optimized in the JIT to avoid
    the creation of a Python int.

### `from __static__ import box`

The `__static__.box` function explicitly converts a primitive value to
the corresponding Python type. E.g. `x: int = box(an_int64)`,
`y: bool = box(a_cbool)`, `z: float = box(a_double)`.

### `from __static__ import CheckedDict, CheckedList`

`__static__.CheckedDict` is a Python dictionary that enforces the
contained types at runtime. E.g. if `d: CheckedDict[int, str]` then it
will be a runtime `TypeError` to place a non-int key or non-str value
into `d`. Within static Python code this is unnecessary since the
compiler will already enforce correct types (and in fact we bypass the
check in this case, so there's also no overhead.) But you can safely
pass a `CheckedDict` out of Static Python code and into normal Python
code and if it is later passed back into Static Python code, the static
compiler will be able to trust that its keys are definitely ints and its
values definitely strings. (For normal Python containers, which don't do
any runtime enforcement, Static Python always treats their contents as
of dynamic, unknown type.)

Similarly, `CheckedList` is just like a Python list, except its
contained type is enforced at runtime.

(You may be wondering why the hidden prologue described above doesn't
fully validate the contained types of e.g. a Python dict passed as an
argument to a Static Python function, so that we can trust them. The
answer is that it's far too expensive to do this in general, since it is
necessarily `O(n)` in the size of the container.)

### `from __static__ import Array, Vector`

`__static__.Array` is a fixed-size contiguous array of primitive values,
like a C array. `__static__.Vector` is similar but dynamically sized.

### `from __static__ import clen`

The `__static__.clen` function gets the `len()` of a Python object as a
primitive `int64`. In the general case this is equivalent to
`int64(len(obj))`, but if `obj` is a builtin Python list, dictionary,
`__static__.CheckedDict`, `__static__.Array`, or `__static__.Vector`, we
are able to emit a much faster length check without ever creating a
Python `int`.

### `from __static__ import crange`

Similar to `__static__.clen` but this allows getting a range with an
unboxed integer. It can be used in a `for` loop to loop over a set
range without allocating the intermediate integers.

### `from __static__ import inline`

The `@inline` decorator allows the static compiler to inline a one-line
function directly into its (statically compiled) callers for efficiency.
The function body must consist only of a single `return` statement.

### `from __static__ import dynamic_return`

The `@dynamic_return` decorator causes the static compiler to not trust
the annotated return type of a function. It is useful in cases where we
intentionally lie about the return type.

For example, if we return a weakref, or a lazily evaluated string
translation, we may annotate the return value as the weakly-referenced
type, or as a string. In these scenarios, Static Python will try to
ensure the returned object matches the annotation, but that\'ll fail.
Using `dynamic_return` is a workaround for such scenarios so
that MyPy or Pyre can still see the more specific annotation, but Static
Python will treat it as dynamically typed.

### `from __static__ import cast`

The `__static__.cast()` function is similar to `typing.cast()` in its
usage, but unlike `typing.cast()` it performs a runtime type check to
validate that the object is in fact of the type you are casting it to,
allowing the static compiler to trust that type. E.g. if
`x = cast(int, some_non_static_function())`, then the static compiler
will know that `x` is of type `int`, even though it doesn't know and
cannot trust the return type of `some_non_static_function()`. If at
runtime the function returns something that is not an `int`, the `cast`
will raise `TypeError`.

In most cases you shouldn't need `__static__.cast()`, because the
compiler can handle values of unknown type (it just treats them as
dynamically typed Python objects, same as Python normally would). If you
use an object of unknown type in a place where a specific type is
required, the static compiler will allow you to do so and will
automatically insert a cast to the needed type at that point.

### `from __static__ import ClassDecorator, TClass`

Frequently users will want to use a decorator with a class which can
apply transformations to them such as attaching extra data. The `ClassDecorator`
and `TClass` types allow this transformation to be marked in a type-safe way:

```
from __static__ import TClass

def mydec(cls: TClass) -> TClass:
    cls.my_tag = 42
    return cls

@mydec
class C:
    def __init__(self) -> None:
        self.x: int = 42
```

When the static compiler recognizes that a function implements the `__static__.ClassDecorator` protocol by accepting and returning `TClass` it will allow
the decorator to be applied to a static class without invalidating the normal
analysis.

### `from cinderx import cached_property, async_cached_property`

Cached properties are a useful helper for areas where you'd like to produce
a value lazily. Cinder includes cached property classes for both async
and sync forms. When these are applied to a method in a class the compiler
recognizes the decorator and auto-generates the associated storage for the
cached version as a slot in the instance.

```
from cinder import cached_property
class C:
    @cached_property
    def foo(self) -> int:
        return some_expensive_computation()
```
