# Copyright (c) Meta Platforms, Inc. and affiliates.

"""CPU-only coverage of the opt-in, immutable CPython evaluator policy."""

import functools
import os
import subprocess
import sys
import sysconfig
import textwrap
import types
import unittest

import cinderx
import cinderx.jit as jit
from cinderx.test_support import run_in_fresh_process, subprocess_env


_mode_inline_frames = []
_mode_eval_api = None


def _mode_inline_callee(value):
    return value + 1


def _mode_inline_caller(value):
    _mode_eval_api.set_eval_frame_record(_mode_inline_frames)
    result = _mode_inline_callee(value)
    for _ in range(10):
        result += 1
    return result


cpython_process = functools.partial(
    run_in_fresh_process,
    additional_env={"CINDERX_INTERPRETER_MODE": "cpython"},
)


@unittest.skipUnless(
    sys.version_info[:2] == (3, 14)
    and "+meta" not in sys.version
    and not sysconfig.get_config_var("Py_GIL_DISABLED")
    and cinderx.is_initialized(),
    "requires the stock CPython 3.14 GIL runtime",
)
class CPythonInterpreterModeTest(unittest.TestCase):
    def assert_policy(self):
        self.assertEqual(cinderx.get_interpreter_mode(), "cpython")
        self.assertFalse(cinderx.is_frame_evaluator_installed())

    @cpython_process
    def test_manual_compilation_and_enable_disable_preserve_policy(self):
        def target(value):
            return value + 1

        self.assert_policy()
        jit.enable()
        self.assertTrue(jit.force_compile(target))
        self.assertTrue(jit.is_jit_compiled(target))
        self.assertEqual(target(41), 42)
        jit.disable(deopt_all=True)
        self.assertEqual(target(9), 10)
        self.assert_policy()
        jit.enable()
        self.assertTrue(jit.is_jit_compiled(target))
        self.assertEqual(target(41), 42)
        self.assert_policy()

    @cpython_process
    def test_lazy_compilation_preserves_policy(self):
        def target(value):
            return value + 1

        self.assertTrue(jit.lazy_compile(target))
        self.assertFalse(jit.is_jit_compiled(target))
        self.assertEqual(target(41), 42)
        self.assertTrue(jit.is_jit_compiled(target))
        self.assert_policy()

    @cpython_process
    def test_positive_threshold_for_existing_and_new_functions(self):
        def existing(value):
            return value + 1

        jit.background_compile(False)
        jit.compile_after_n_calls(3)

        def created(value):
            return value * 2

        for target, expected in ((existing, 4), (created, 6)):
            before = jit.count_interpreted_calls(target)
            for _ in range(3):
                self.assertEqual(target(3), expected)
                self.assertFalse(jit.is_jit_compiled(target))
            self.assertEqual(jit.count_interpreted_calls(target) - before, 3)
            self.assertEqual(target(3), expected)
            self.assertTrue(jit.is_jit_compiled(target))
        self.assert_policy()

    @cpython_process
    def test_auto_uses_call_threshold_only(self):
        jit.background_compile(False)
        jit.auto()
        self.assertIsNone(jit.get_compile_after_n_bytecodes())
        threshold = jit.get_compile_after_n_calls()
        self.assertIsNotNone(threshold)

        def target(value):
            return value + 1

        for _ in range(threshold + 1):
            self.assertEqual(target(41), 42)
        self.assertTrue(jit.is_jit_compiled(target))
        self.assert_policy()

    @cpython_process
    def test_jit_list_preserves_policy(self):
        jit.background_compile(False)

        def target(value):
            return value + 1

        jit.append_jit_list(f"{target.__module__}:{target.__qualname__}")
        self.assertEqual(target(41), 42)
        self.assertTrue(jit.is_jit_compiled(target))
        self.assert_policy()

    @cpython_process
    def test_policy_is_immutable_and_unsupported_features_are_atomic(self):
        os.environ["CINDERX_INTERPRETER_MODE"] = "cinderx"
        before_calls = jit.get_compile_after_n_calls()
        before_bytecodes = jit.get_compile_after_n_bytecodes()
        for operation in (
            cinderx.install_frame_evaluator,
            lambda: jit.compile_after_n_bytecodes(1),
            jit.enable_specialized_opcodes,
        ):
            with self.assertRaisesRegex(RuntimeError, "INTERPRETER_MODE"):
                operation()
        self.assertEqual(jit.get_compile_after_n_calls(), before_calls)
        self.assertEqual(jit.get_compile_after_n_bytecodes(), before_bytecodes)
        cinderx.init()
        self.assert_policy()

    @cpython_process
    def test_foreign_hook_survives_compile_and_removal(self):
        import _testinternalcapi

        def target(value):
            return value + 1

        frames = []
        _testinternalcapi.set_eval_frame_record(frames)
        try:
            jit.force_compile(target)
            cinderx.remove_frame_evaluator()
            self.assertEqual(target(41), 42)
        finally:
            _testinternalcapi.set_eval_frame_default()
        self.assertEqual(frames.count("target"), 1)
        self.assertTrue(jit.is_jit_compiled(target))
        self.assert_policy()

    @cpython_process
    def test_inline_parent_deopt_uses_cpython_without_reentering_hook(self):
        global _mode_eval_api
        import _testinternalcapi as _mode_eval_api

        caller = _mode_inline_caller
        callee = _mode_inline_callee
        _mode_inline_frames.clear()
        jit.enable_hir_inliner()
        jit.force_compile(callee)
        jit.force_compile(caller)
        self.assertGreater(jit.get_num_inlined_functions(caller), 0)
        before = jit.count_interpreted_bytecodes(caller)
        try:
            self.assertEqual(caller(31), 42)
        finally:
            _mode_eval_api.set_eval_frame_default()
        self.assertEqual(_mode_inline_frames.count(callee.__name__), 1)
        self.assertEqual(_mode_inline_frames.count(caller.__name__), 0)
        # The callee's entry check deopts through its suspended caller. The
        # remaining loop must use CPython even while a foreign hook is active;
        # CinderX's interpreter would increment this bytecode counter.
        self.assertEqual(jit.count_interpreted_bytecodes(caller), before)
        self.assertTrue(jit.is_jit_compiled(caller))
        self.assertTrue(jit.is_jit_compiled(callee))
        self.assert_policy()

    @cpython_process
    def test_native_generator_ordinary_bytecode(self):
        def target(value):
            yield value
            yield value + 1

        jit.force_compile(target)
        self.assertTrue(jit.is_jit_compiled(target))
        self.assertEqual(list(target(41)), [41, 42])
        self.assert_policy()

    @cpython_process
    def test_rejects_static_flags_prefix_and_nested_code(self):
        from cinderx.compiler.consts import CI_CO_STATICALLY_COMPILED

        def target():
            return 42

        flagged = target.__code__.replace(
            co_flags=target.__code__.co_flags | CI_CO_STATICALLY_COMPILED
        )
        # Unflagged generated Static Python helpers also use this prefix.
        prefixed = target.__code__.replace(co_code=bytes((126, 0, 24, 0, 36, 0)))
        nested = target.__code__.replace(co_consts=(None, prefixed))
        for code in (flagged, prefixed, nested):
            with self.assertRaisesRegex(RuntimeError, "Static Python/custom bytecode"):
                types.FunctionType(code, {})
            with self.assertRaisesRegex(RuntimeError, "Static Python/custom bytecode"):
                exec(code, {})
            with self.assertRaisesRegex(RuntimeError, "Static Python/custom bytecode"):
                target.__code__ = code
        self.assertEqual(target(), 42)

    @cpython_process
    def test_static_compiler_and_loader_rejected(self):
        from cinderx.compiler import compile as cinder_compile
        from cinderx.compiler.static import StaticCodeGenerator
        from cinderx.compiler.strict.loader import init_static_python

        with self.assertRaisesRegex(RuntimeError, "INTERPRETER_MODE"):
            cinder_compile("def f(): return 42", "static_test", "exec",
                           compiler=StaticCodeGenerator)
        with self.assertRaisesRegex(RuntimeError, "INTERPRETER_MODE"):
            init_static_python()
        self.assert_policy()

    def run_startup(self, source, **options):
        env = {key: value for key, value in os.environ.items()
               if not key.startswith(("CINDERX_", "CINDERJIT_"))}
        env.update(subprocess_env(), CINDERX_INTERPRETER_MODE="cpython", PYTHON_JIT="0")
        env.update(options)
        result = subprocess.run([sys.executable, "-X", "faulthandler", "-c",
                                 textwrap.dedent(source)], env=env,
                                capture_output=True, text=True, timeout=90)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_startup_auto_flag_uses_calls(self):
        self.run_startup("""
            import cinderx, cinderx.jit as jit
            assert cinderx.get_interpreter_mode() == 'cpython'
            assert jit.get_compile_after_n_calls() == 1000
            assert jit.get_compile_after_n_bytecodes() is None
            assert not cinderx.is_frame_evaluator_installed()
        """, CINDERX_JIT_AUTO="1")

    def test_invalid_configuration_and_caught_failure_recovery(self):
        for option, value in (
            ("CINDERX_JIT_COMPILE_N_BYTECODES", "1"),
            ("CINDERX_JIT_MAX_CODE_SIZE", "9999999999999999999999999999999999999999999999"),
        ):
            with self.subTest(option=option):
                self.run_startup("""
                    import os, gc, builtins
                    option = os.environ["TEST_INVALID_OPTION"]
                    os.environ[option] = os.environ["TEST_INVALID_VALUE"]
                    try:
                        import _cinderx
                    except (ValueError, RuntimeError) as exc:
                        retained_failure = exc
                    else:
                        raise AssertionError('unsupported startup policy accepted')
                    del os.environ[option]
                    gc.collect()
                    for _ in range(10):
                        exec('value = 42', {})
                    original_import = builtins.__import__
                    observed = []
                    def wrapped_import(name, *args, **kwargs):
                        if name == 'builtins':
                            observed.append(name)
                            exec('probe_value = 42', {})
                        return original_import(name, *args, **kwargs)
                    builtins.__import__ = wrapped_import
                    try:
                        import cinderx, cinderx.jit as jit
                    finally:
                        builtins.__import__ = original_import
                    assert observed
                    assert cinderx.get_interpreter_mode() == 'cpython'
                    def target(value): return value + 1
                    assert jit.force_compile(target)
                    assert target(41) == 42
                    assert not cinderx.is_frame_evaluator_installed()
                    del retained_failure
                    gc.collect()
                    assert target(41) == 42
                """, TEST_INVALID_OPTION=option, TEST_INVALID_VALUE=value)

    def test_preexisting_specialized_callsite_is_invalidated(self):
        self.run_startup("""
            import dis, types
            module_code = compile('def target(value): return value + 1', '<preimport>', 'exec')
            module_code = module_code.replace(co_consts=tuple(
                value.replace(co_flags=value.co_flags | 0x4000)
                if isinstance(value, types.CodeType) else value
                for value in module_code.co_consts))
            exec(module_code, globals())
            def caller(): return target(41)
            for _ in range(1000): assert caller() == 42
            assert any(i.opname == 'CALL_PY_EXACT_ARGS'
                       for i in dis.get_instructions(caller, adaptive=True))
            import cinderx
            try:
                caller()
            except RuntimeError as exc:
                assert 'Static Python/custom bytecode' in str(exc)
            else:
                raise AssertionError('specialized callsite bypassed rejection')
            import cinderx.jit as jit
            jit.enable_hir_inliner()
            # The ordinary caller must not inline/preload its unsupported
            # static callee and thereby bypass the rejecting vectorcall.
            try:
                compiled = jit.force_compile(caller)
            except RuntimeError as exc:
                assert 'INTERPRETER_MODE' in str(exc)
            else:
                if compiled:
                    assert jit.get_num_inlined_functions(caller) == 0
                try:
                    caller()
                except RuntimeError as exc:
                    assert 'Static Python/custom bytecode' in str(exc)
                else:
                    raise AssertionError('native caller bypassed static rejection')
        """)

    def test_import_preserves_existing_foreign_evaluator(self):
        self.run_startup("""
            import _testinternalcapi as api
            frames = []
            api.set_eval_frame_record(frames)
            try:
                import cinderx, cinderx.jit as jit
                def target(value): return value + 1
                assert jit.force_compile(target)
                assert target(41) == 42
                assert frames.count('target') == 1
                assert not cinderx.is_frame_evaluator_installed()
            finally:
                api.set_eval_frame_default()
        """)


if __name__ == "__main__":
    unittest.main()
