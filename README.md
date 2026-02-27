# CinderX

[![PyPI - Version](https://img.shields.io/pypi/v/cinderx.svg)](https://pypi.org/pypi/cinderx/)

CinderX is a Python extension that improves the performance of the Python
runtime.

## Status

CinderX is under active development.  It is used in production at Meta for
use-cases like the Instagram Django service.  It is **experimental** for
external users.  New releases are published to PyPI on a weekly basis.

## Features

- **JIT Compiler** - Just-in-time compilation of Python bytecode to native
  machine code
- **Static Python** - A stricter form/subset of Python, for type safety and
  optimization

The codebase includes other features as well, such as a parallel garbage
collector and a lighter weight implementation of Python interpreter frames.
However these features are not compatible with the stock CPython runtime yet.

## Requirements

- Python 3.14.3 or later
- Linux (x86_64)
- GCC 13+ or Clang 18+

The extension should build and import on macOS but most features will be
disabled at runtime.  Windows is not yet supported at all.

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
functions to machine code.  It will track what functions are called frequently
and compile the hottest ones automatically.

For more control over this process, you can pass the call count threshold to use
for compilation instead of using `auto()`:

```python
import cinderx.jit

# Compile functions after they are called 10 times.
cinderx.jit.compile_after_n_calls(10)
```

If you want to compile individual functions, you can do so manually:

```python
import cinderx.jit

def foo(): ...
def bar(): ...

# Compile `foo` immediately.
cinderx.jit.force_compile(foo)

# Compile `bar` the next time it is called.
cinderx.jit.lazy_compile(bar)
```

## CinderX vs Cinder

[Cinder](https://github.com/facebookincubator/cinder) was a fork of the CPython
runtime developed at Meta.  It included runtime optimizations (e.g. JIT) and was
specifically targeted at the Instagram Django codebase.  For Python 3.10, Meta
decided to turn it into a Python extension to improve compatibility with newer
Python versions.  This extension is now known as CinderX ("the X" is for
"extension").

For Python versions 3.10 through 3.12, CinderX still depends on patches to
Meta's fork of the Python runtime.  Python 3.14 is the first version of stock
CPython that CinderX supports.

## License

CinderX is MIT licensed, see the LICENSE file.

## Terms of Use

https://opensource.fb.com/legal/terms

## Privacy Policy

https://opensource.fb.com/legal/privacy

---

Copyright © 2025 Meta Platforms, Inc.
