# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import dis


# Decorator to return a new version of the function with an alternate globals
# dict.
def with_globals(gbls):
    def decorator(func):
        new_func = type(func)(
            func.__code__, gbls, func.__name__, func.__defaults__, func.__closure__
        )
        new_func.__module__ = func.__module__
        new_func.__kwdefaults__ = func.__kwdefaults__
        return new_func

    return decorator


def failUnlessHasOpcodes(*required_opnames):
    """Fail a test unless func has all of the opcodes in `required` in its code
    object.
    """

    def decorator(func):
        opnames = {i.opname for i in dis.get_instructions(func)}
        missing = set(required_opnames) - opnames
        if missing:

            def wrapper(*args):
                raise AssertionError(
                    f"Function {func.__qualname__} missing required opcodes: {missing}"
                )

            return wrapper
        return func

    return decorator
