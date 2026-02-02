# AGENTS.md

This file provides guidance to AI coding agents when working with code
in the fbsource/fbcode/cinderx directory.

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

The source code to Python being used when build with Buck is in:
fbsource/third-party/python/<python version>/['patched/' - for 3.14+/]

Upstream Python is synced into fbsource/third-party/python/main/patched/
daily. Currently 3.15 in CinderX refers to 'main' branch in upstream
Python code.

If a version has a "t" postfix, it's a Python build with
free-threading/no-GIL support (`Py_GIL_DISABLED` macro is defined).

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
code is automatically built into a library during a Buck build. However,
before committing changes `cinderx/Internal/regen-cached-borrow-files.sh`
should be run as this makes a static version of the latest borrowed code
for use in OSS builds.

An exception to the rule of not copying code is the CinderX interpreter
which has a mixture of manually copied code, borrowed code, and uses
Python interpreter generator tools to override opcodes. There is a new
implementation per Python version in `cinderx/Interpreter/<version>`. In
each version `borrowed-ceval.c.template` contains local borrows for the
interpreter, and `cinder-bytecodes.c` has implementation overrides
and new bytecodes for CinderX. The base definition of bytecodes
in upstream Python is in `<python source>/Python/bytecodes.c`. If
the bytecodes are changed (in CinderX or Python) then the relevant
`cinderx/Interpreter/regen-cases-<version>.sh` script needs to be run.

## Build, Run, Test

The quickest way to build + run a working "Python binary" with CinderX
loaded is: `buck run fbcode//cinderx:python<version> --`. After the
`--` any normal `python` arguments can be used (e.g. `-c 'print("hello
world\n")'`). If no arguments are specified the usual Python REPL
will be started - use a headless tmux session to interact with
this. The `<version>` in the target name can be things like 3.14,
3.14t, 3.15 etc. see keys for the long table in `PY_VERSION_MAP` from
`cinderx/defs.bzl` for possible versions.

By default the CinderX JIT is NOT enabled. To run in a mode where all
functions are immediately JIT add a `-jit-all` postfix to the `python`
target. E.g. `fbcode//cinderx:python3.14-jit-all`.

CinderX can run tests from the upstream Python test suite using the Cinder
Test Runner script (`cinderx/TestScripts/cinder_test_runner312.py`)
which has a target base name of `ctr`. E.g.: `buck run
fbcode//cinderx:ctr3.14-jit-all -- test.test_coroutines` to run the
3.14 upstream coroutine tests with JIT enabled. We use the CTR script
to launch upstream tests as it has logic to skip tests which are known
not to work with CinderX see the .txt files in `cinderx/TestScripts/`
for details. Source for upstream tests can be found in the Python source
code under `Lib/test`.

There are also Python tests for CinderX in
`cinderx/PythonLib/test_cinderx/`. These can also be run with CTR e.g.:
`buck run fbcode//cinderx:ctr3.14-jit-all -- test_cinderx.test_jit_frame`.

CinderX has C++ unit tests to exercise internal functionality more
directly. These are in `cinderx/RuntimeTests`. Run with e.g. `buck test
fbcode//cinderx/RuntimeTests:RuntimeTests_<version>`.

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
