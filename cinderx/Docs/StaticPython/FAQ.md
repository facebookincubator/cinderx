# Static Python FAQ

## What are the perfomance benefits?

We ran a comprehensive benchmarking matrix, across three benchmarks,
covering multiple combinations of Cinder features and code changes.

Each benchmark has several versions of its code that accomplish the same
work: the Original untyped version, a Typed version which just adds type
annotations without otherwise changing the code, and a TypedOpt version
converted to take advantage of Static Python features for maximum
performance under Static Python and the JIT. Fannkuch and DeltaBlue also
have TypedMinOpt, which uses just a few Static Python specific features
where it can be done in a targeted way without significant changes to
the code.

The other axes of our test matrix are SP (whether the Static Python
compiler is used), JIT (whether the Cinder JIT is enabled), and SF
(whether the JIT shadow-frame mode is enabled, which reduces Python
frame allocation costs.) The matrix is not full, since using the Static
Python compiler on untyped code has no noticeable effect, and
shadow-frame mode is only relevant under the JIT.

This is a preview of the detailed benchmarks results, which are pending
publication. We present below a normalized graph of \"speedup\" compared
to \"Original ()\" (e.g. \"1\" is original untyped version with no JIT
enabled). Values greater than 1 represent speedup in that benchmark
configuration, and values smaller than 1 represent slowdown in that
benchmark configuration. The graph is in log scale.

![Static Python Normalized Benchmarks Results](../images/static_python_normalized_speedups.png)

Raw data (normalized speedups) behind the graph:

|           | TypedOpt (SP, JIT) | TypedOpt (SP) | TypedOpt (JIT, SF) | TypedOpt (JIT) | TypedOpt () | TypedMinOpt (SP, JIT, SF) | TypedMinOpt (SP, JIT) | TypedMinOpt (SP) | TypedMinOpt (JIT, SF) | TypedMinOpt (JIT) | TypedMinOpt () | Typed (SP, JIT, SF) | Typed (SP, JIT) | Typed (SP) | Typed (JIT, SF) | Typed (JIT) | Typed () | Original (JIT, SF) | Original (JIT) | Original () |   |
|-----------|--------------------|---------------|--------------------|----------------|-------------|---------------------------|-----------------------|------------------|-----------------------|-------------------|----------------|---------------------|-----------------|------------|-----------------|-------------|----------|--------------------|----------------|-------------|---|
| Richards  |                    | 7.63          | 1.27               | 1.16           | 0.98        | 0.62                      |                       |                  |                       |                   |                |                     | 7.18            | 3.81       | 1.29            | 1.77        | 1.38     | 0.74               | 3.81           | 2.56        | 1 |
| Fannkuch  | 3.28               | 3.14          | 0.12               | 0.10           | 0.10        | 0.08                      | 1.67                  | 1.63             | 0.57                  | 1.29              | 1.32           | 0.91                | 1.45            | 1.48       | 0.67            | 1.41        | 1.43     | 1.00               | 1.50           | 1.46        | 1 |
| DeltaBlue | 4.20               | 3.07          | 0.97               | 2.32           | 1.89        | 1.25                      | 2.34                  | 1.86             | 0.87                  | 1.98              | 1.62           | 1.06                | 2.12            | 1.73       | 0.94            | 1.77        | 1.49     | 0.96               | 1.87           | 1.55        | 1 |

Raw data (average runtime in seconds over 10 runs, before
normalization):

|           | TypedOpt (SP, JIT, SF) | TypedOpt (SP, JIT) | TypedOpt (SP) | TypedOpt (JIT, SF) | TypedOpt (JIT) | TypedOpt () | Typed (SP, JIT, SF) | Typed (SP, JIT) | Typed (SP) | Typed (JIT, SF) | Typed (JIT) | Typed () | TypedMinOpt (SP, JIT, SF) | TypedMinOpt (SP, JIT) | TypedMinOpt (SP) | TypedMinOpt (JIT, SF) | TypedMinOpt (JIT) | TypedMinOpt () | Original (JIT, SF) | Original (JIT) | Original () |
|-----------|------------------------|--------------------|---------------|--------------------|----------------|-------------|---------------------|-----------------|------------|-----------------|-------------|----------|---------------------------|-----------------------|------------------|-----------------------|-------------------|----------------|--------------------|----------------|-------------|
| Richards  | 0.567                  | 1.319              | 7.938         | 8.635              | 10.267         | 16.134      | 1.401               | 2.642           | 7.805      | 5.688           | 7.308       | 13.602   |                           |                       |                  |                       |                   |                | 2.642              | 3.935          | 10.059      |
| Fannkuch  | 1.263                  | 1.318              | 34.899        | 40.564             | 40.589         | 48.759      | 2.846               | 2.792           | 6.176      | 2.942           | 2.891       | 4.14     | 2.477                     | 2.531                 | 7.241            | 3.216                 | 3.146             | 4.557          | 2.763              | 2.833          | 4.137       |
| DeltaBlue | 0.295                  | 0.403              | 1.274         | 0.534              | 0.655          | 0.992       | 0.585               | 0.718           | 1.323      | 0.7             | 0.833       | 1.285    | 0.529                     | 0.665                 | 1.42             | 0.626                 | 0.763             | 1.173          | 0.661              | 0.801          | 1.239       |


## How does compilation work?

In a single sentence, Static Python is a bytecode compiler which uses
existing type annotations to make Python code more efficient.
Internally, it consists of a few components. Broadly, we have runtime
components and compile-time components.

The following diagram shows the overall flow of information. The steps
marked with `*` are newly introduced by Static Python, the
remaining ones also exist in regular CPython:

![Static Python Diagram](../images/static_python_diagram.png)

### Compilation Steps

High level description of each step in the flowchart.

| Step                              | Phase                  | Description                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   |
|-----------------------------------|------------------------|-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| Source Code                       | Pre-compile            | This is just the raw text source code. Technically, it’s not even a step, but it occurs in the diagram, therefore has its own entry.                                                                                                                                                                                                                                                                                                                                                                                                                                          |
| Declaration Visit                 | Compile Time           | When a module is being loaded, we need to know the symbols that are declared within it. Some of our analyses rely on this information (before even getting to the actual code generation steps).                                                                                                                                                                                                                                                                                                                                                                              |
| Strict Modules analysis + rewrite | Compile Time           | This is where we run the Strict Module analyzer and rewriter. The rewriter is an AST transformation, and includes features such as inserting non-changeable builtin functions (e.g len(), into the module namespace. The analyzer is an abstract interpreter which checks for import time side effects (and other things too, but that’s outside the scope of this document).                                                                                                                                                                                                 |
| AST Optimizations                 | Compile Time           | This step performs common optimizations on the AST, such as folding constant operations, optimizing immutable data structures, etc.                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| Type Checker (Type Binder)        | Compile Time           | This is the magic step where we perform type analysis on the given code. We build a mapping from AST Node to Types, while also checking for correctness. In a few cases, we also perform type inference, to improve the experience of writing typed code. The Type Checker can also be run in a linting mode, where we output a list of detected type errors.                                                                                                                                                                                                                 |
| Code Generation                   | Compile Time           | In this step, we actually construct Python bytecode, by walking the AST. We take advantage of all the type information from the previous step, to generate efficient bytecode. In addition to opcodes in “normal” Python, Static Python uses a new specialized set of opcodes, which remove a lot of overhead associated with checking types at runtime. Whenever a type cannot be guaranteed by the Type Checker, we treat it as “dynamic” (or “Any”), and fall back to “normal” opcodes. In this way, our generated bytecode is fully compatible with untyped code as well! |
| Peephole optimizer                | Compile Time           | This performs further optimizations on the generated bytecode. E.g: removing bytecode that is unreachable.                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |
| Strict/Static import loader       | Compile Time + Runtime | The Import loader is an implementation of importlib.abc.SourceFileLoader. It is responsible for stuff like checking whether a module is strict/static, and then running the appropriate kinds of compilation steps on it. It is used at compile time, as well as runtime. During compilation, the Loader creates .pyc files. These can then be packaged and deployed on servers. At runtime, the loader imports and executes this bytecode. After this step, the bytecode may be executed by the interpreter (the eval loop), or may be further compiled by the Cinder JIT.   |
| Interpreter Extensions            | Runtime                | This refers to the new set of opcodes introduced by Static Python (as mentioned above). These are very closely related with the classloader, which we will discuss separately.                                                                                                                                                                                                                                                                                                                                                                                                |
| JIT                               | Runtime                | The JIT is vast enough to require its own set of high-level documentation. For the purposes of Static Python, we can think of it has having three compilation steps: - HIR (High level IR) - LIR (Low level IR) - Assembly (Generation of assembly code) Each of the above steps has its own optimization and analysis passes. Additionally, the JIT interacts heavily with Static Python through its support for primitive types. Needless to say, a majority of Static Python optimizations are enabled by the JIT.                                                         |
