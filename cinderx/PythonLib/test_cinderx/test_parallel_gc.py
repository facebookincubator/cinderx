# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import unittest

import cinderx
import cinderx.jit

# pyre-ignore[21]: Pyre doesn't know about cpython/Lib/test.
import test.test_gc
from cinderx.test_support import passIf, passUnless


def _restore_parallel_gc(settings: dict[str, int] | None) -> None:
    cinderx.disable_parallel_gc()
    if settings is not None:
        cinderx.enable_parallel_gc(
            settings["min_generation"],
            settings["num_threads"],
        )


@passUnless(cinderx.has_parallel_gc(), "Testing the Parallel GC")
class ParallelGCAPITests(unittest.TestCase):
    def setUp(self) -> None:
        self.old_par_gc_settings = cinderx.get_parallel_gc_settings()
        cinderx.disable_parallel_gc()

    def tearDown(self) -> None:
        _restore_parallel_gc(self.old_par_gc_settings)

    def test_get_settings_when_disabled(self) -> None:
        self.assertEqual(cinderx.get_parallel_gc_settings(), None)

    def test_get_settings_when_enabled(self) -> None:
        cinderx.enable_parallel_gc(2, 8)
        settings = cinderx.get_parallel_gc_settings()
        expected = {
            "min_generation": 2,
            "num_threads": 8,
        }
        self.assertEqual(settings, expected)

    def test_set_invalid_generation(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid generation"):
            cinderx.enable_parallel_gc(4, 8)

    def test_set_invalid_num_threads(self) -> None:
        with self.assertRaisesRegex(ValueError, "invalid num_threads"):
            cinderx.enable_parallel_gc(2, -1)


# Run all the GC tests with parallel GC enabled


@passUnless(cinderx.has_parallel_gc(), "Testing the Parallel GC")
# pyre-ignore[11]: Pyre doesn't know about cpython/Lib/test.
class ParallelGCTests(test.test_gc.GCTests):
    @passIf(cinderx.jit.is_enabled(), "Implementation detail of the interpreter")
    def test_frame(self) -> None:
        pass

    @passIf(cinderx.jit.is_enabled(), "Implementation detail of the interpreter")
    def test_get_objects_arguments(self) -> None:
        pass


@passUnless(cinderx.has_parallel_gc(), "Testing the Parallel GC")
# pyre-ignore[11]: Pyre doesn't know about cpython/Lib/test.
class ParallelGCCallbackTests(test.test_gc.GCCallbackTests):
    # Tests implementation details of serial collector
    def test_refcount_errors(self) -> None:
        # necessary for tearDown to succeed
        # pyre-ignore[16]: ParallelGCCallbackTests` has no attribute `visit`
        self.visit = None


@passUnless(cinderx.has_parallel_gc(), "Testing the Parallel GC")
# pyre-ignore[11]: Pyre doesn't know about cpython/Lib/test.
class ParallelGCFinalizationTests(test.test_gc.PythonFinalizationTests):
    pass


OLD_PAR_GC_SETTINGS: dict[str, int] | None = None


def setUpModule() -> None:
    test.test_gc.setUpModule()

    global OLD_PAR_GC_SETTINGS
    OLD_PAR_GC_SETTINGS = cinderx.get_parallel_gc_settings()
    try:
        cinderx.enable_parallel_gc(0, 8)
    except RuntimeError:
        # Parallel GC isn't available on this build
        pass


def tearDownModule() -> None:
    test.test_gc.tearDownModule()

    _restore_parallel_gc(OLD_PAR_GC_SETTINGS)


if __name__ == "__main__":
    unittest.main()
