# Strict Modules

> [*Python is more dynamic than the language I \*wanted\* to design. — Guido van Rossum*](https://mail.python.org/pipermail/python-list/2001-August/073435.html)


## What are strict modules?

Strict modules are an opt-in mechanism for restricting the dynamism of
top-level (module-level) code in a Python module. These stricter
semantics are designed to have multiple benefits:

* Eliminate common classes of developer errors
* Improve the developer experience
* Unlock new opportunities for optimizing Python code and simplify other classes of optimizations

The strict module analyzer is no longer supported. The only impact
of strict modules is that modules marked strict are immutable and their types
are frozen at runtime after the module definition completes.

## Overview
