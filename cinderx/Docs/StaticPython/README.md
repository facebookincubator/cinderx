# Static Python

## What is Static Python?

Static Python is an experimental alternative bytecode compiler and set
of runtime extensions to CPython. It takes advantage of existing type
annotations in the code for increased efficiency at runtime. It heavily
leverages the Cinder JIT, and provides additional typing information to
it, allowing a lot of operations to run at native speed. For example,
attribute accesses for classes in Static Python are a single indexed
load, because we use slot-based instance layouts and can resolve the
attribute offset at compile time.

Static Python modules are just specialized Python modules, and can
seamlessly call into or be called from normal Python code.

Except for a few cases where we tighten up semantics (e.g. modules and
types are immutable after construction, classes are auto-slotified), the
Static Python compiler can compile normal untyped or typed Python code
without changes.

## Why do we need Static Python?

Static Python provides Cython level performance, with no added syntax or
build steps. It provides a safe set of tools to perform low level
operations such as integer/floating-point arithmetic without sacrificing
on the memory safety provided by Python.

It also performs runtime type checking to ensure the accuracy of type
annotations, which improves the developer experience of typed Python.

## How does it work?

Static Python is implemented as a bytecode compiler, which emits
specialized Python opcodes when it can determine the type of something
at compile time. Whenever it's not possible to determine the type at
compile time, it just assumes the type to be dynamic, and falls back to
normal Python behavior.

When the Cinder JIT sees the special opcodes, it further optimizes the
generated machine code. Consider this Python code:
```py
class C:
    def __init__(self) -> None:
        self.a: int = 1

def fn(instance: C) -> int:
    return instance.a
```

In normal Python, `fn` is compiled to this bytecode:

```assembly
6           0 LOAD_FAST                0 (instance)
            2 LOAD_ATTR                0 (a)
            4 RETURN_VALUE
```

Here, `LOAD_ATTR` is a CPU bottleneck, because of the number
of ways of looking up Python attributes. Static Python however, knows
that `a` is a slot of class `C`, so it generates this bytecode
instead:

```assembly
6           0 LOAD_FAST                0 (instance)
            2 LOAD_FIELD               2 (('__main__', 'A', 'a'))
            4 RETURN_VALUE
```

With the static compiler, the `LOAD_ATTR` is now
`LOAD_FIELD`. Within the JIT, this opcode is compiled to
these three machine instructions:

```assembly
    mov    0x10(%rdi),%rbx
    test   %rbx,%rbx
    je     0x7fc1d9836daa
```

Compared with the [standard Python attribute
lookup](https://github.com/python/cpython/blob/b38b2fa0218911ccc20d576ff504f39c9c9d47ec/Objects/object.c#L910),
this is way faster! The tradeoff is, at runtime there\'s a hidden
prologue verifying the argument types, so that we only look into the
memory location when the type is correct. These checks are extremely
fast, and omitted when the caller function is also part of a Static
Python module.
