# JIT

This is an implementation of a just-in-time compiler for Python code.  It
compiles Python bytecode down to machine code at runtime.

This is a method JIT, where a compilation unit is all the bytecode contained
within a Python code object.  The most common examples of code objects are
Python functions and methods.  Lambdas and module bodies are also code objects.

## Recommended Use

The suggested way to start use the JIT is to have your main Python module look
like this:

```python
import cinderx.jit


def main() -> None:
    # This enables the JIT.
    cinderx.jit.auto()

    # Application code here...
    ...


if __name__ == "__main__":
    main()
```

It's best to enable the JIT early on in your application's code.  That way you
can get the most out of the compiled code that it generates.

## Deciding What to Compile

The suggested way to use the JIT is what we call "auto mode", where the JIT
decides on its own what functions should be compiled.  Exactly what logic is
used by the JIT is meant to be an implementation detail, but one assumption that
the user can made is that functions that are called frequently will likely be
selected for compilation.

```python
import cinderx.jit

# Configure the JIT to compile functions automatically, letting it determine
# which functions would benefit from compilation.
cinderx.jit.auto()
```

### Manual Control

The `cinderx.jit` module provides more fine-grained control over exactly what
code to compile.  Users can hand-pick individual functions for compilation.

```python
import cinderx.jit

def foo(): ...
def bar(): ...

# Compile `foo` immediately.
cinderx.jit.force_compile(foo)

# Compile `bar` the next time it is called.
cinderx.jit.lazy_compile(bar)
```

The JIT can also be configured to compile functions after they are called a
certain number of times.

```python
import cinderx.jit

# Compile functions after they are called 100 times.
cinderx.jit.compile_after_n_calls(100)

# Compile functions right before the first time they are called.
#
# Note: This will compile everything in your codebase.  It can be interesting
# for experimentation and testing but it will greatly slow down your
# application's startup and initialization time.  The cost of compilation will
# probably outweigh the performance benefit of the compiled code.
cinderx.jit.compile_after_n_calls(0)
```

## Introspecting the JIT

### Python API

The JIT can be queried from Python code to inspect what it's doing.  Some
examples here include:

- **`cinderx.jit.is_jit_compiled(func)`**: Check if function `func` is compiled.

- **`cinderx.jit.disassemble(func)`**: Get the machine code the JIT generated
  for function `func` in a disassembled, human-readable form.

- **`cinderx.jit.get_compiled_functions()`**: Get the list of all functions
  compiled by the JIT.

### Environment Variables / -X CLI Flags

Some useful environment variables and CLI options include:

- **`CINDERX_JIT_DEBUG=1` / `-X cinderx-jit-debug=1`**: Print high-level
  information about what the JIT is doing.

- **`CINDERX_JIT_DUMP_ASM=1` / `-X cinderx-jit-dump-asm=1`**: Print the machine
  code generated for all functions in a human-readable format.

- **`CINDERX_JIT_LOG_FILE=/path/to/file` / `-X cinderx-jit-log-file=/path/to/file`**:
  Control which output file the JIT will log to.

## Controlling the JIT at Runtime

- **`cinderx.jit.disable(deopt_all=False)`**: Turn the JIT off and prevent more
  functions from being compiled.  By default this leaves already-compiled
  functions as they are.  Call the function with `deopt_all=True` to force all
  compiled functions back to running in the interpreter.

- **`cinderx.jit.enable()`**: Re-enable the JIT after it has been disabled with
  `cinderx.jit.disable()`.

- **`cinderx.jit.pause(deopt_all=False)`**: Context manager for temporarily
  disabling the JIT for a block of code.  Takes the same arguments as
  `cinderx.jit.disable()`.

- **`cinderx.jit.jit_suppress(func)`**: Prevent the JIT from compiling function
  `func`.

- **`cinderx.jit.jit_unsuppress(func)`**: Allow function `func`, previously
  suppressed by `cinderx.jit.jit_suppress(func)`, to be compilable by the JIT.
