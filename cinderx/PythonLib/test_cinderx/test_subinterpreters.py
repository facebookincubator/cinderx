# Copyright (c) Meta Platforms, Inc. and affiliates.

import sys
import unittest


@unittest.skipUnless(sys.version_info >= (3, 14), "subinterpreter test requires 3.14+")
class SubinterpreterTest(unittest.TestCase):
    def test_cinderx_import_fails_in_subinterpreter(self) -> None:
        """Verify that _cinderx cannot be loaded in a subinterpreter."""
        import _interpreters

        interp_id = _interpreters.create()
        try:
            # _interpreters.exec returns None on success, or a namespace
            # with exception details on failure.
            exc_info = _interpreters.exec(interp_id, "import _cinderx")
            self.assertIsNotNone(
                exc_info,
                "_cinderx import should fail in a subinterpreter",
            )
            # The exact exception type depends on how _cinderx is packaged:
            # ImportError when the module is found but rejects subinterpreters,
            # ModuleNotFoundError when the extension isn't locatable at all.
            self.assertIn(
                # pyre-ignore[16]: `None` has no attribute `type`.
                exc_info.type.__name__,
                ("ImportError", "ModuleNotFoundError"),
            )
        finally:
            _interpreters.destroy(interp_id)

    def test_cinderx_wrapper_not_initialized_in_subinterpreter(self) -> None:
        """Verify that the cinderx wrapper is not initialized in a subinterpreter."""
        import _interpreters

        interp_id = _interpreters.create()
        try:
            # The cinderx Python wrapper gracefully handles _cinderx being
            # unavailable, but should report as not initialized.
            exc_info = _interpreters.exec(
                interp_id,
                (
                    "import cinderx\n"
                    "assert not cinderx.is_initialized(), "
                    "'cinderx should not be initialized in a subinterpreter'"
                ),
            )
            self.assertIsNone(exc_info, f"unexpected error: {exc_info}")
        finally:
            _interpreters.destroy(interp_id)


if __name__ == "__main__":
    unittest.main()
