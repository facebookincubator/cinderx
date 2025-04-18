# Copyright (c) Meta Platforms, Inc. and affiliates.
import inspect
import types
import unittest
import unittest.mock
from os.path import normcase

try:
    from concurrent.futures import ThreadPoolExecutor
except ImportError:
    ThreadPoolExecutor = None

from test.support import cpython_only

try:
    from test import inspect_fodder as mod
except ImportError:
    from test.test_inspect import inspect_fodder as mod

# Functions tested in this suite:
# ismodule, isclass, ismethod, isfunction, istraceback, isframe, iscode,
# isbuiltin, isroutine, isgenerator, isgeneratorfunction, getmembers,
# getdoc, getfile, getmodule, getsourcefile, getcomments, getsource,
# getclasstree, getargvalues, formatargspec, formatargvalues,
# currentframe, stack, trace, isdatadescriptor

# NOTE: There are some additional tests relating to interaction with
#       zipimport in the test_zipimport_support test module.

modfile = mod.__file__
if modfile.endswith(("c", "o")):
    modfile = modfile[:-1]

# Normalize file names: on Windows, the case of file names of compiled
# modules depends on the path used to start the python executable.
modfile = normcase(modfile)


def revise(filename, *args):
    return (normcase(filename),) + args


git = mod.StupidGit()


class CinderX_TestSignatureBind(unittest.TestCase):
    @staticmethod
    def call(func, *args, **kwargs):
        sig = inspect.signature(func)
        ba = sig.bind(*args, **kwargs)
        return func(*ba.args, **ba.kwargs)

    @cpython_only
    def test_signature_bind_implicit_arg(self):
        # Issue #19611: getcallargs should work with set comprehensions
        # CinderX: Modified for comprehension inlining
        def make_gen():
            return (z * z for z in range(5))

        gencomp_code = make_gen.__code__.co_consts[1]
        gencomp_func = types.FunctionType(gencomp_code, {})

        iterator = iter(range(5))
        self.assertEqual(set(self.call(gencomp_func, iterator)), {0, 1, 4, 9, 16})
