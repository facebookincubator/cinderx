---
id: introduction
title: Introduction
description: "CinderX is a Python extension that improves the performance of the Python runtime, featuring a JIT compiler and Static Python."
slug: /
sidebar_label: Introduction
sidebar_position: 0
---

# CinderX

CinderX is a Python extension that improves the performance of the Python
runtime.

## Status

CinderX is under active development. It is used in production at Meta for
use-cases like the Instagram Django service. It is **experimental** for external
users. New releases are published to [PyPI](https://pypi.org/project/cinderx/) on
a weekly basis.

## Features

- **[JIT Compiler](./Jit/index.md)** — Just-in-time compilation of Python
  bytecode to native machine code.
- **[Static Python](./StaticPython/index.md)** — A stricter form/subset of
  Python, for type safety and optimization.

The codebase includes other features as well, such as a parallel garbage
collector and a lighter weight implementation of Python interpreter frames.
However these features are not compatible with the stock CPython runtime yet.

## Requirements

- Python 3.14
- GCC 13+ or Clang 18+

|         |        Linux       |        macOS       |       Windows      |
| ------- | :----------------: | :----------------: | :----------------: |
|  x86-64 | ✅                 | ❌                 | ✅                 |
| aarch64 | ✅                 | ✅                 | ❌                 |

## Installation

```bash
pip install cinderx
```

## Using the JIT

The recommended way to start using the JIT is to do:

```python
import cinderx.jit

cinderx.jit.auto()
```

This will configure the CinderX extension to automatically compile Python
functions to machine code. It will track what functions are called frequently
and compile the hottest ones automatically.

See the [JIT documentation](./Jit/index.md) for more details.

## Performance

CinderX features deliver significant performance improvements over the standard CPython interpreter and is continually being improved. The following benchmarks were run comparing four configurations and were run on an x64 Linux Broadwell 2GHz machine.

| Benchmark | No JIT | CinderX JIT | Static CinderX JIT | Static + Primitives |
| --- | --- | --- | --- | --- |
| [Fannkuch](https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/fannkuchredux.html)  | 4.24s | 4.40s | 0.84s | 0.59s |
| [Richards](https://www.google.com/search?q=richards+benchmark+operating+system) | 3.73s | 2.12s | 0.88s | 0.45s |
| [nbody](https://benchmarksgame-team.pages.debian.net/benchmarksgame/description/nbody.html) | 2.89s | 2.15s | 2.31s | 0.23s |


![CinderX benchmark results across JIT and Static Python configurations](/img/performance.png)

### Configuration Details

- **No JIT** — Baseline CPython interpreter with no JIT compilation enabled.
- **CinderX JIT** — CinderX JIT compiler enabled with standard dynamically-typed Python code.
- **Static CinderX JIT** — CinderX JIT with [Static Python](https://github.com/facebookincubator/cinderx/blob/main/documentation/static_python.md) type annotations, enabling ahead-of-time type analysis and optimised code generation.
- **Static + Primitives** — Static Python with [primitive type specialisation](https://github.com/facebookincubator/cinderx/blob/main/documentation/static_python.md#primitive-types), allowing the JIT to use unboxed machine-native integers and floats instead of Python objects.



## License

CinderX is MIT licensed, see the LICENSE file.
