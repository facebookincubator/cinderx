# Preloading

Before the JIT compiler starts transforming Python bytecode, it will first
"preload" the code object. This is a preliminary step that resolves global
variable names and type/field descrs in the code object to concrete Python
objects.

Preloading is the dedicated place in the JIT compilation pipeline where the JIT
is allowed to execute arbitrary Python code. In general, the JIT cannot tolerate
Python code being run as it will break too many of the JIT's assumptions. This
is why preloading is done as early as possible, so that the rest of the
compilation pipeline can assume it runs without unintended side effects.

The features that strongly depend on preloading in the JIT are:

* Lazy Imports. The JIT sets up global caches which need to resolve global
  variables to an initial value. Getting that value can trigger a lazy import,
  execute arbitrary code, and break the JIT's assumptions.

* Static Python opcodes. An example opcode here is `LOAD_FIELD`. To compile it,
  the JIT needs to resolve the `LOAD_FIELD`'s type descr to know at what memory
  offset the field will be found.

* Multi-threaded compilation. The compiler's worker threads do not have their
  own `PyInterpreterState` object and will crash trying to run anything but the
  most basic Python operations. They depend on preloading to resolve all the
  Python data the rest of the compilation pipeline might want.

The features that weakly depend on preloading to improve their success rate are:

* Inlining. When the inliner examines calls from `foo` to `bar`, it wants to
  have access to the bytecode of `bar` to attempt to inline the call. If `bar`
  is not loaded / not available, then inlining cannot occur.

* Static Python calls. When one Static Python function `foo` calls another
  Static Python function `bar` and `bar` is already compiled then the JIT is
  able to emit an optimized native call from `foo` to `bar`. So when the JIT
  sees these kinds of static-to-static calls, it will aim to preload and compile
  the callee function first and then compile the caller.
