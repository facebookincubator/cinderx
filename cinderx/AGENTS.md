# AGENTS.md

This file provides guidance to AI coding agents when working with code in this
directory.

**Important**: If the file `Internal/AGENTS.md` exists, you MUST read it before
proceeding with any task. It contains additional context for internal
development environments.

## Overview

CinderX is a Python runtime extension that adds (among other things)
a bytecode JIT compiler to Python, and enhanced handling of Python's
type annotations overall named Static Python. To enable Static Python
a custom Python to bytecode compiler is provided that leverages type
annotations to produce custom bytecode which can be further optimized by
CinderX's JIT. When in operation CinderX also replaces Python's default
interpreter loop with a modified version that supports Static Python
bytecodes and has other minor tweaks to help with JIT integration.

## Python Version Support

At any one time CinderX supports multiple versions of Python. In C/C++
code the `PY_VERSION_HEX` and `Py_GIL_DISABLED` macros are used to
select code to target different Python versions. Utilities are provided
in `Common/` to abstract commonly used features which changed between
Python versions.

## Different Target Architectures

CinderX supports multiple architectures in its code generation backend
(e.g. x86-64, aarch64). The preference is to have all code compile under all
possible architectures, even on architectures where it is not used. Small blocks
of code can check the value of the `kBuildArch` constant with an `if constexpr`
statement. For entire functions, it's preferred to keep them unconditionally
defined, using the `[[maybe_unused]]` attribute to avoid unused code
warnings. Both of these cases assume that the underlying code can be compiled
across all architectures. If there is code that can only compile under a
specific hardware architecture, then that can be conditionally compiled by
checking preprocessor defines like `CINDER_X86_64` and `CINDER_AARCH64`.

One specific case to highlight is struct/class fields that are only used on
specific architectures. If they are part of singletons then it's fine to define
them always even if they are unused, but otherwise they should be conditionally
compiled via the preprocessor defines, to save on memory usage.

## Non-public Python APIs

Where possible CinderX tries to use public Python APIs. When not
possible CinderX has an automated system for copying - "borrowing"
- upstream code and bundling it into a private library for used by
CinderX. Copying code into CinderX avoids need to modify Python to make
internal API available. It's very important to try and use automated
borrowing rather than manually copying code from Python as this keeps
code provenance clear and is far more maintainable.

Implementation of the borrowing tool is in `UpstreamBorrow/` with
most of the implementation in `UpstreamBorrow.py`. There is one set of
borrow directives per Python version. E.g. `borrowed-3.12.c.template` or
`borrowed-3.14.free-threading.c.template` (for the "t" version). There
is a single header file `borrowed.h` used across all versions. Borrowed
code is automatically built into a library during a build.

An exception to the rule of not copying code is the CinderX interpreter
which has a mixture of manually copied code, borrowed code, and uses
Python interpreter generator tools to override opcodes. There is a new
implementation per Python version in `cinderx/Interpreter/<version>`. In
each version `borrowed-ceval.c.template` contains local borrows for the
interpreter, and `cinder-bytecodes.c` has implementation overrides
and new bytecodes for CinderX. The base definition of bytecodes
in upstream Python is in `<python source>/Python/bytecodes.c`.

## Adding a New HIR Instruction

The JIT uses a High-level Intermediate Representation (HIR) defined in
`Jit/hir/`. Adding a new HIR instruction requires updates to multiple files:

1. **Jit/hir/opcode.h** - Add the new opcode to the `FOREACH_OPCODE` macro.
   This auto-generates the enum value and `Is<Opcode>()` predicates.

2. **Jit/hir/hir.h** - Define the instruction class. Simple instructions
   with no special behavior can use `DEFINE_SIMPLE_INSTR`. More complex
   instructions use `INSTR_CLASS` and inherit from `InstrT` with appropriate
   template parameters (`HasOutput`, `Operands<N>`, `DeoptBase`, etc.).

3. **Jit/lir/generator.cpp** - Add a case in `TranslateOneBasicBlock()` to
   lower the HIR instruction to LIR/machine code. This typically involves
   `bbb.appendCallInstruction()` for runtime calls or `bbb.appendInstr()`
   for inline code generation.

4. **Jit/hir/instr_effects.cpp** - Add the opcode to both switch statements:
   - `memoryEffects()`: Defines memory read/write effects for optimization
   - `hasArbitraryExecution()`: Whether the instruction can run user code

5. **Jit/hir/hir.cpp** - Add the opcode to:
   - `isReplayable()`: Whether the instruction can be safely re-executed
   - `isPassthrough()`: Whether it passes through its input unchanged
     (instructions with no output go in the abort list)

6. **Jit/hir/printer.cpp** - Add to `format_immediates()` to control how
   the instruction prints. Instructions with no special immediates return "".

7. **Jit/hir/pass.cpp** - Add to `outputType()` to specify the instruction's
   output type. Instructions with no output go in the "no destination" list.

8. **Jit/hir/parser.cpp** - Add parsing support in `parseInstr()` if the
   instruction needs to be parsed from text HIR (used in tests).

If the instruction needs to call a custom runtime helper function:
- Declare it in **Jit/jit_rt.h**
- Implement it in **Jit/jit_rt.cpp**

## Handling JIT compile-time errors

If an error is hit when JIT-compiling a Python function, the preference is to
raise a C++ exception. This will unwind the stack and silently fail the compile,
causing the Python function to return back to the interpreter as usual.

The `JIT_THROW` and `JIT_THROW_IF` macros make it easy to raise an exception
that is tagged with the offending file and line number, to make debugging
easier. These are the preferred tool for handling irrecoverable JIT-compilation
errors.

The `JIT_ABORT` and `JIT_CHECK` macros are similar but will crash the entire
process, which is usually undesirable. They should be used sparingly, and in
very restricted scenarios where throwing a C++ exception does not make sense.

The `JIT_DCHECK` macro is intended for invariants that we'd like to enforce, but
cannot do so in production builds because they lie in performance-sensitive code
paths (e.g. the function vectorcall entry point that we install).

## Investigating JIT failures

If you're investigating a JIT issue you may want to isolate the issue to a
single function.  You can use `cinderx.jit.force_compile` to compile an
individual function if you suspect that a specific function is problematic.

If you run the test with PYTHONJITDUMPASM=1 you can see the assembly dumped
along with the HIR to understand what the compiled code looks like and what
the underlying issue is.

## Code style

See `Internal/docs/style.md`.
