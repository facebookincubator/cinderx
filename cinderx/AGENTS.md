# AGENTS.md

This file provides guidance to AI coding agents when working with code in this
directory.

If the file Internal/AGENTS.md exists, read this too. It contains details of
things which are relevant to Meta's internal developer environment but do not
apply for non Meta environments.

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
