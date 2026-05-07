# Copyright (c) Meta Platforms, Inc. and affiliates.

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest


class TestStaticModuleRunner(unittest.TestCase):
    def test_entry_script_is_statically_compiled(self):
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".py", delete=False
        ) as f:
            f.write(textwrap.dedent("""
                import __static__
                from cinderx.compiler.consts import CI_CO_STATICALLY_COMPILED

                def add(x: int, y: int) -> int:
                    return x + y

                assert add.__code__.co_flags & CI_CO_STATICALLY_COMPILED
            """))
            script_path = f.name

        try:
            result = subprocess.run(
                [sys.executable, "-m", "__static__", script_path],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
        finally:
            os.unlink(script_path)

    def test_imported_module_is_statically_compiled(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            lib_path = os.path.join(tmpdir, "sp_lib.py")
            with open(lib_path, "w") as f:
                f.write(textwrap.dedent("""
                    import __static__

                    def compute(x: int, y: int) -> int:
                        return x + y
                """))

            main_path = os.path.join(tmpdir, "sp_main.py")
            with open(main_path, "w") as f:
                f.write(textwrap.dedent("""
                    import __static__
                    from cinderx.compiler.consts import CI_CO_STATICALLY_COMPILED
                    from sp_lib import compute

                    assert compute.__code__.co_flags & CI_CO_STATICALLY_COMPILED
                """))

            result = subprocess.run(
                [sys.executable, "-m", "__static__", main_path],
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
