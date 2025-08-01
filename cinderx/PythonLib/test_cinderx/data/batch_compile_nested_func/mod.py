# Copyright (c) Meta Platforms, Inc. and affiliates.

def outer():
    def inner():
        return 42

    return inner
