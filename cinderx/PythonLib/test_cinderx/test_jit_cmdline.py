# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Tests for the JIT's command line options.

Every JIT option can be given either as an ``-X`` flag or as an environment
variable, and the JIT only reads them once, during startup.  So each case here
runs a child interpreter twice: once with the option as an ``-X`` flag and once
with it in the environment.

Most of the JIT config isn't readable from Python, so options are checked by
their observable effect: log output, allocator stats, or whether a function
ends up compiled.  The handful of options with no such effect are checked
against the acknowledgement line the flag processor logs under ``-X
jit-debug``, which shows the option was recognized and its callback ran.
"""

import os
import subprocess
import sys
import tempfile
import textwrap
import unittest
from collections.abc import Callable, Mapping, Sequence
from pathlib import Path

from cinderx.test_support import ENCODING, passIf, subprocess_env


# Printed by a child that ran to completion.
_OK = "CHILD_OK"

# The JIT starts up with the cinderx extension, so every child has to import it
# for its command line options to be read at all.
_HELLO: str = textwrap.dedent(
    f"""\
    import cinderx

    print({_OK!r})
    """
)

# Compiles a single function, giving the JIT's logging options something to
# report on.
_COMPILE_SAMPLE: str = textwrap.dedent(
    f"""\
    import cinderx.jit as jit


    def sample(x):
        return x + 1


    jit.force_compile(sample)
    assert jit.is_jit_compiled(sample)
    print({_OK!r})
    """
)

# Like _COMPILE_SAMPLE, but leaves compilation up to the JIT list rather than
# forcing it.  Written to a real file so that JIT list entries can refer to it
# by path and line number.
_JIT_LIST_MODULE: str = textwrap.dedent(
    f"""\
    import cinderx.jit as jit


    def sample(x):
        return x + 1


    sample(1)
    print("COMPILED" if jit.is_jit_compiled(sample) else "NOT_COMPILED")
    print({_OK!r})
    """
)

_SAMPLE_LINENO: int = _JIT_LIST_MODULE.splitlines().index("def sample(x):") + 1


class CmdLineTest(unittest.TestCase):
    def _run(
        self,
        args: Sequence[str],
        env_extra: Mapping[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        env = subprocess_env()
        if env_extra is not None:
            env.update(env_extra)
        return subprocess.run(
            [sys.executable, *args],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            encoding=ENCODING,
            env=env,
        )

    def _check_both_forms(
        self,
        flag: str,
        env_var: str,
        args: Sequence[str],
        check: Callable[[subprocess.CompletedProcess[str]], None],
    ) -> None:
        """
        Run a child with the option set as an ``-X`` flag and again with it set
        as an environment variable, running ``check`` over both results.

        ``flag`` and ``env_var`` may carry a value, e.g. ``jit-huge-pages=0``
        and ``PYTHONJITHUGEPAGES=0``.  A valueless option is treated as if it
        were set to 1, matching how the JIT reads ``-X`` flags.
        """

        with self.subTest(form=f"-X {flag}"):
            check(self._run(["-X", flag, *args]))

        name, _, value = env_var.partition("=")
        with self.subTest(form=f"${name}"):
            check(self._run(args, {name: value or "1"}))

    def _check_acknowledged(
        self,
        flag: str,
        env_var: str,
        canonical_flag: str,
        canonical_env_var: str,
    ) -> None:
        """
        Check an option the JIT doesn't expose to Python in any other way.

        The flag processor logs every option it recognizes, under its canonical
        name rather than the legacy alias the option was given as.
        """

        proc = self._run(["-X", "jit-debug", "-X", flag, "-c", _HELLO])
        self._assert_ok(proc)
        self.assertIn(f"{canonical_flag} has been specified", proc.stderr)

        name, _, value = env_var.partition("=")
        proc = self._run(["-X", "jit-debug", "-c", _HELLO], {name: value or "1"})
        self._assert_ok(proc)
        self.assertIn(f"{canonical_env_var} has been specified", proc.stderr)

    def _assert_ok(self, proc: subprocess.CompletedProcess[str]) -> None:
        self.assertEqual(
            proc.returncode,
            0,
            f"child failed\nstdout={proc.stdout!r}\nstderr={proc.stderr!r}",
        )
        self.assertIn(_OK, proc.stdout)

    # Logging options.

    def test_debug(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("JIT: cinderx/", proc.stderr)

        self._check_both_forms("jit-debug", "PYTHONJITDEBUG", ["-c", _HELLO], check)

    def test_debug_refcount(self) -> None:
        self._check_acknowledged(
            "jit-debug-refcount",
            "PYTHONJITDEBUGREFCOUNT",
            "cinderx-jit-debug-refcount",
            "CINDERX_JIT_DEBUG_REFCOUNT",
        )

    def test_debug_inliner(self) -> None:
        self._check_acknowledged(
            "jit-debug-inliner",
            "PYTHONJITDEBUGINLINER",
            "cinderx-jit-debug-inliner",
            "CINDERX_JIT_DEBUG_INLINER",
        )

    def test_dump_hir(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("Initial HIR for __main__:sample", proc.stderr)

        self._check_both_forms(
            "jit-dump-hir", "PYTHONJITDUMPHIR", ["-c", _COMPILE_SAMPLE], check
        )

    def test_dump_hir_passes(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("HIR for __main__:sample before pass", proc.stderr)
            self.assertIn("HIR for __main__:sample after pass", proc.stderr)

        self._check_both_forms(
            "jit-dump-hir-passes",
            "PYTHONJITDUMPHIRPASSES",
            ["-c", _COMPILE_SAMPLE],
            check,
        )

    def test_dump_final_hir(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("Optimized HIR for __main__:sample", proc.stderr)

        self._check_both_forms(
            "jit-dump-final-hir",
            "PYTHONJITDUMPFINALHIR",
            ["-c", _COMPILE_SAMPLE],
            check,
        )

    def test_dump_lir(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("LIR for __main__:sample", proc.stderr)

        self._check_both_forms(
            "jit-dump-lir", "PYTHONJITDUMPLIR", ["-c", _COMPILE_SAMPLE], check
        )

    def test_dump_lir_origin(self) -> None:
        # Enabling LIR origins implies dumping LIR at all.  Origins show up as
        # comment lines naming the HIR instruction a block of LIR came from.
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("LIR for __main__:sample", proc.stderr)
            self.assertIn("\n# ", proc.stderr)

        self._check_both_forms(
            "jit-dump-lir-origin",
            "PYTHONJITDUMPLIRORIGIN",
            ["-c", _COMPILE_SAMPLE],
            check,
        )

        proc = self._run(["-X", "jit-dump-lir-origin=0", "-c", _COMPILE_SAMPLE])
        self._assert_ok(proc)
        self.assertIn("LIR for __main__:sample", proc.stderr)
        self.assertNotIn("\n# ", proc.stderr)

    def test_dump_asm(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            # Builds without a disassembler leave asm dumping off and say so.
            self.assertTrue(
                "Disassembly for __main__:sample" in proc.stderr
                or "disassembler not supported by this build" in proc.stderr,
                proc.stderr,
            )

        self._check_both_forms(
            "jit-dump-asm", "PYTHONJITDUMPASM", ["-c", _COMPILE_SAMPLE], check
        )

    def test_gdb_support(self) -> None:
        # GDB support also turns on JIT debug logging.
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("JIT: cinderx/", proc.stderr)

        self._check_both_forms(
            "jit-gdb-support", "PYTHONJITGDBSUPPORT", ["-c", _HELLO], check
        )

    def test_gdb_write_elf(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("JIT: cinderx/", proc.stderr)

        self._check_both_forms(
            "jit-gdb-write-elf", "PYTHONJITGDBWRITEELF", ["-c", _HELLO], check
        )

    def test_dump_stats(self) -> None:
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("JIT runtime stats:", proc.stderr)

        self._check_both_forms(
            "jit-dump-stats",
            "PYTHONJITDUMPSTATS",
            ["-c", _COMPILE_SAMPLE],
            check,
        )

    def test_log_file(self) -> None:
        source = textwrap.dedent(
            f"""\
            import cinderx.jit as jit

            print({_OK!r})
            """
        )

        with tempfile.TemporaryDirectory() as tmp_dir:
            log_file = Path(tmp_dir) / "jit.log"

            def check(proc: subprocess.CompletedProcess[str]) -> None:
                self._assert_ok(proc)
                self.assertEqual(proc.stderr, "")
                self.assertIn("JIT: cinderx/", log_file.read_text())
                log_file.unlink()

            self._check_both_forms(
                f"jit-log-file={log_file}",
                f"PYTHONJITLOGFILE={log_file}",
                ["-X", "jit-debug", "-c", source],
                check,
            )

    # Code allocation options.

    def test_huge_pages(self) -> None:
        # Huge page allocations are only visible once the JIT has actually
        # emitted code, hence jit-all.
        source = textwrap.dedent(
            f"""\
            import cinderx.jit as jit

            print("HUGE" if "huge_allocs" in jit.get_allocator_stats() else "SMALL")
            print({_OK!r})
            """
        )
        args = ["-X", "jit-all", "-c", source]

        def check_on(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("HUGE", proc.stdout)

        def check_off(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("SMALL", proc.stdout)

        self._check_both_forms(
            "jit-huge-pages=1", "PYTHONJITHUGEPAGES=1", args, check_on
        )
        self._check_both_forms(
            "jit-huge-pages=0", "PYTHONJITHUGEPAGES=0", args, check_off
        )

    # Compilation options.

    def test_jit_all(self) -> None:
        source = textwrap.dedent(
            f"""\
            import cinderx.jit as jit

            assert jit.is_enabled()
            assert jit.get_compile_after_n_calls() == 0
            print({_OK!r})
            """
        )
        self._check_both_forms(
            "jit-all", "PYTHONJITALL", ["-c", source], self._assert_ok
        )

    def test_jit_disable(self) -> None:
        source = textwrap.dedent(
            f"""\
            import cinderx.jit as jit

            assert not jit.is_enabled()
            print({_OK!r})
            """
        )
        self._check_both_forms(
            "jit-disable", "PYTHONJITDISABLE", ["-c", source], self._assert_ok
        )

    def test_multithreaded_compile_test(self) -> None:
        # cinderx.jit doesn't re-export this predicate, so go to the native
        # module directly.  It only exists once the JIT has started up.
        source = textwrap.dedent(
            f"""\
            import cinderx

            import cinderjit

            assert cinderjit.is_multithreaded_compile_test_enabled()
            print({_OK!r})
            """
        )
        self._check_both_forms(
            "jit-multithreaded-compile-test",
            "PYTHONJITMULTITHREADEDCOMPILETEST",
            ["-c", source],
            self._assert_ok,
        )

    def test_batch_compile_workers(self) -> None:
        self._check_acknowledged(
            "jit-batch-compile-workers=21",
            "PYTHONJITBATCHCOMPILEWORKERS=21",
            "cinderx-jit-batch-compile-workers",
            "CINDERX_JIT_BATCH_COMPILE_WORKERS",
        )

    def test_lir_inliner(self) -> None:
        self._check_acknowledged(
            "jit-lir-inliner",
            "PYTHONJITLIRINLINER",
            "cinderx-jit-lir-inliner",
            "CINDERX_JIT_LIR_INLINER",
        )

    def test_all_static_functions(self) -> None:
        self._check_acknowledged(
            "jit-all-static-functions",
            "PYTHONJITALLSTATICFUNCTIONS",
            "cinderx-jit-all-static-functions",
            "CINDERX_JIT_ALL_STATIC_FUNCTIONS",
        )

    # JIT list options.

    def _write_jit_list_module(self, tmp_dir: str) -> Path:
        module = Path(tmp_dir) / "mod.py"
        module.write_text(_JIT_LIST_MODULE)
        return module

    def test_jit_list_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            module = self._write_jit_list_module(tmp_dir)
            jit_list = Path(tmp_dir) / "jitlist.txt"
            jit_list.write_text("__main__:sample\n")

            def check(proc: subprocess.CompletedProcess[str]) -> None:
                self._assert_ok(proc)
                self.assertIn("COMPILED", proc.stdout)

            self._check_both_forms(
                f"jit-list-file={jit_list}",
                f"PYTHONJITLISTFILE={jit_list}",
                [str(module)],
                check,
            )

    def test_jit_list_wildcards(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            module = self._write_jit_list_module(tmp_dir)
            jit_list = Path(tmp_dir) / "jitlist.txt"
            jit_list.write_text("__main__:*\n")
            args = ["-X", f"jit-list-file={jit_list}", str(module)]

            def check(proc: subprocess.CompletedProcess[str]) -> None:
                self._assert_ok(proc)
                self.assertIn("COMPILED", proc.stdout)

            self._check_both_forms(
                "jit-enable-jit-list-wildcards",
                "PYTHONJITENABLEJITLISTWILDCARDS",
                args,
                check,
            )

            # Without the option the wildcard is taken literally and matches
            # nothing.
            proc = self._run(args)
            self._assert_ok(proc)
            self.assertIn("NOT_COMPILED", proc.stdout)

    def test_jit_list_match_line_numbers(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:
            module = self._write_jit_list_module(tmp_dir)

            matching = Path(tmp_dir) / "matching.txt"
            matching.write_text(f"sample@{module}:{_SAMPLE_LINENO}\n")
            stale = Path(tmp_dir) / "stale.txt"
            stale.write_text(f"sample@{module}:{_SAMPLE_LINENO + 100}\n")

            def check(proc: subprocess.CompletedProcess[str]) -> None:
                self._assert_ok(proc)
                self.assertIn("COMPILED", proc.stdout)

            self._check_both_forms(
                "jit-list-match-line-numbers",
                "PYTHONJITLISTMATCHLINENUMBERS",
                ["-X", f"jit-list-file={matching}", str(module)],
                check,
            )

            # A stale line number only matters when the option is on.
            proc = self._run(
                [
                    "-X",
                    "jit-list-match-line-numbers",
                    "-X",
                    f"jit-list-file={stale}",
                    str(module),
                ]
            )
            self._assert_ok(proc)
            self.assertIn("NOT_COMPILED", proc.stdout)

            proc = self._run(["-X", f"jit-list-file={stale}", str(module)])
            self._assert_ok(proc)
            self.assertIn("COMPILED", proc.stdout)

    # Profiling options.

    @passIf(sys.platform == "win32", "Perf support is Linux-only")
    def test_perfmap(self) -> None:
        # The JIT always records its symbols in CPython's perf map; this option
        # additionally keeps the map open so it can be copied across a fork.
        # test_jit_perf_map.py covers the forking behavior itself.
        def check(proc: subprocess.CompletedProcess[str]) -> None:
            self._assert_ok(proc)
            self.assertIn("Opened JIT perf-map file: /tmp/perf-", proc.stderr)

        self._check_both_forms(
            "jit-perfmap",
            "JIT_PERFMAP",
            ["-X", "jit-debug", "-c", _COMPILE_SAMPLE],
            check,
        )

    @passIf(sys.platform == "win32", "Perf support is Linux-only")
    def test_perf_dumpdir(self) -> None:
        with tempfile.TemporaryDirectory() as tmp_dir:

            def check(proc: subprocess.CompletedProcess[str]) -> None:
                self._assert_ok(proc)
                dumps = list(Path(tmp_dir).glob("jit-*.dump"))
                self.assertNotEqual(dumps, [], f"no jitdump written to {tmp_dir}")
                for dump in dumps:
                    dump.unlink()

            self._check_both_forms(
                f"jit-perf-dumpdir={tmp_dir}{os.sep}",
                f"JIT_DUMPDIR={tmp_dir}{os.sep}",
                ["-c", _COMPILE_SAMPLE],
                check,
            )

    # Help.

    def test_help(self) -> None:
        # jit-help prints the option list and stops the interpreter from
        # starting up, so the child's body never runs.  It has no environment
        # variable equivalent.
        proc = self._run(["-X", "jit-help", "-c", _HELLO])
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("-X opt : set Cinder JIT-specific option.", proc.stdout)
        self.assertNotIn(_OK, proc.stdout)


if __name__ == "__main__":
    unittest.main()
