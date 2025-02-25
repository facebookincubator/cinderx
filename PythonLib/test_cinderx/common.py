# Copyright (c) Meta Platforms, Inc. and affiliates.

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
