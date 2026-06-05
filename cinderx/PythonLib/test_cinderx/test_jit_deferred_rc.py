# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import functools
import gc
import io
import pickle
import subprocess
import sys
import textwrap
import types
import unittest
import weakref
from collections.abc import Callable
from typing import Any, cast

import cinderx.jit
import cinderx.test_support as cinder_support
from cinderx.jit import get_function_hir_opcode_counts
from cinderx.test_support import passUnless


def make_cycle():
    obj = cast(Any, types.ModuleType("DeferredRCModule"))
    obj.self = obj
    box = {"obj": obj}
    return box, weakref.ref(obj)


def noop_callback() -> None:
    return None


# Close over the callback so these helpers stay jitted through the callsite
# instead of deopting on a module-global callback lookup.
def make_hold_arg_across_gc(
    callback: Callable[[], None],
) -> Callable[[Any], bool]:
    @cinder_support.failUnlessJITCompiled
    def hold_arg_across_gc(obj: Any) -> bool:
        callback()
        return obj is not None

    return hold_arg_across_gc


def make_return_arg_after_gc(
    callback: Callable[[], None],
) -> Callable[[Any], Any]:
    @cinder_support.failUnlessJITCompiled
    def return_arg_after_gc(obj: Any) -> Any:
        callback()
        return obj

    return return_arg_after_gc


@cinder_support.failUnlessJITCompiled
def save_reduce_with_owned_dead_branch(pickler, func, args):
    save = pickler.save
    write = pickler.write
    func_name = getattr(func, "__name__", "")
    if pickler.proto >= 2 and func_name == "__newobj_ex__":
        func = functools.partial(abs, 1)
    save(func)
    save(args)
    write(pickle.REDUCE)
    return sys.getrefcount(abs)


@cinder_support.failUnlessJITCompiled
def yield_arg_after_start(obj):
    yield None
    yield obj


@cinderx.jit.jit_suppress
def run_hold_arg_gc_scenario() -> dict[str, Any]:
    box, weak_obj = make_cycle()
    state = {}

    def drop_and_collect():
        box["obj"] = None
        gc.collect()
        state["alive_during_collect"] = weak_obj() is not None

    hold_arg_across_gc = make_hold_arg_across_gc(drop_and_collect)
    assert cinderx.jit._would_tag_if_deferred(box["obj"])
    cinderx.jit.get_and_clear_runtime_stats()
    call_result = hold_arg_across_gc(box["obj"])
    runtime_stats = cast(dict[str, Any], cinderx.jit.get_and_clear_runtime_stats())
    return {
        "call_result": call_result,
        "alive_during_collect": state["alive_during_collect"],
        "deopt_count": len(runtime_stats["deopt"]),
    }


@cinderx.jit.jit_suppress
def run_return_arg_gc_scenario() -> dict[str, Any]:
    box, weak_obj = make_cycle()
    state = {}

    def drop_and_collect():
        box["obj"] = None
        gc.collect()
        state["alive_during_collect"] = weak_obj() is not None

    return_arg_after_gc = make_return_arg_after_gc(drop_and_collect)
    assert cinderx.jit._would_tag_if_deferred(box["obj"])
    cinderx.jit.get_and_clear_runtime_stats()
    result = return_arg_after_gc(box["obj"])
    runtime_stats = cast(dict[str, Any], cinderx.jit.get_and_clear_runtime_stats())
    result_is_same = result is weak_obj()
    result_self_cycle = result.self is result
    del result
    return {
        "alive_during_collect": state["alive_during_collect"],
        "deopt_count": len(runtime_stats["deopt"]),
        "result_is_same": result_is_same,
        "result_self_cycle": result_self_cycle,
    }


@cinderx.jit.jit_suppress
def run_generator_arg_scenario() -> dict[str, Any]:
    box, weak_obj = make_cycle()
    gen = yield_arg_after_start(box["obj"])

    first_yield = next(gen)
    box["obj"] = None
    gc.collect()
    alive_after_suspend = weak_obj() is not None

    result = next(gen)
    result_is_same = result is weak_obj()
    result_self_cycle = result.self is result
    del result

    stop_iteration_raised = False
    try:
        next(gen)
    except StopIteration:
        stop_iteration_raised = True
    del gen
    return {
        "first_yield": first_yield,
        "alive_after_suspend": alive_after_suspend,
        "result_is_same": result_is_same,
        "result_self_cycle": result_self_cycle,
        "stop_iteration_raised": stop_iteration_raised,
    }


@cinderx.jit.jit_suppress
def run_batch_decref_tagged_builtin_scenario() -> dict[str, Any]:
    pickler = cast(Any, pickle._Pickler(io.BytesIO(), 1))

    assert cinderx.jit._would_tag_if_deferred(abs)
    before = sys.getrefcount(abs)
    inside = save_reduce_with_owned_dead_branch(pickler, abs, (-1,))
    after = sys.getrefcount(abs)
    return {
        "before": before,
        "inside": inside,
        "after": after,
        "memo_contains_func": id(abs) in pickler.memo,
    }


@passUnless(cinderx.jit.is_enabled(), "requires cinderjit")
@passUnless(cinder_support.FREE_THREADING_BUILD, "requires free-threaded build")
class JITDeferredRCTests(unittest.TestCase):
    def test_hir_tags_object_args(self):
        counts = get_function_hir_opcode_counts(make_hold_arg_across_gc(noop_callback))
        self.assertIsNotNone(counts)
        self.assertEqual(counts.get("TagIfDeferred"), 1)

    def test_runtime_only_tags_deferred_objects(self):
        box, weak_obj = make_cycle()
        self.assertTrue(gc.is_tracked(box["obj"]))
        self.assertTrue(cinderx.jit._would_tag_if_deferred(box["obj"]))
        self.assertFalse(cinderx.jit._would_tag_if_deferred(object()))
        self.assertFalse(cinderx.jit._would_tag_if_deferred([]))
        self.assertIsNotNone(weak_obj())

    @cinderx.jit.jit_suppress
    def test_gc_sees_tagged_arg_held_by_jit_frame(self):
        scenario = cast(dict[str, Any], run_hold_arg_gc_scenario())
        self.assertTrue(scenario["call_result"])
        self.assertTrue(scenario["alive_during_collect"])
        self.assertEqual(scenario["deopt_count"], 0)

    @cinderx.jit.jit_suppress
    def test_escaping_tagged_arg_is_materialized(self):
        counts = get_function_hir_opcode_counts(make_return_arg_after_gc(noop_callback))
        self.assertIsNotNone(counts)
        self.assertEqual(counts.get("TagIfDeferred"), 1)
        self.assertGreaterEqual(counts.get("MaterializeRef", 0), 1)

        scenario = cast(dict[str, Any], run_return_arg_gc_scenario())
        self.assertTrue(scenario["alive_during_collect"])
        self.assertEqual(scenario["deopt_count"], 0)
        self.assertTrue(scenario["result_is_same"])
        self.assertTrue(scenario["result_self_cycle"])

    @cinderx.jit.jit_suppress
    def test_generator_arg_survives_suspend_and_materializes_on_yield(self):
        scenario = cast(dict[str, Any], run_generator_arg_scenario())
        self.assertIsNone(scenario["first_yield"])
        self.assertTrue(scenario["alive_after_suspend"])
        self.assertTrue(scenario["result_is_same"])
        self.assertTrue(scenario["result_self_cycle"])
        self.assertTrue(scenario["stop_iteration_raised"])

    @cinderx.jit.jit_suppress
    def test_batch_decref_preserves_tagged_builtin_refs(self):
        counts = get_function_hir_opcode_counts(save_reduce_with_owned_dead_branch)
        self.assertIsNotNone(counts)
        self.assertGreaterEqual(counts.get("BatchDecref", 0), 1)

        scenario = cast(dict[str, Any], run_batch_decref_tagged_builtin_scenario())
        self.assertTrue(scenario["memo_contains_func"])
        self.assertEqual(scenario["inside"], scenario["before"] + 1)
        self.assertEqual(scenario["after"], scenario["before"] + 1)

    @cinderx.jit.jit_suppress
    def test_subprocess_released_args_are_collectable_after_return(self):
        script = textwrap.dedent(
            """
            import gc
            import types
            import weakref

            import cinderx.jit
            import cinderx.test_support as cinder_support

            def make_hold_arg_across_gc(callback):
                @cinder_support.failUnlessJITCompiled
                def hold_arg_across_gc(obj):
                    callback()
                    return obj is not None

                return hold_arg_across_gc

            def make_return_arg_after_gc(callback):
                @cinder_support.failUnlessJITCompiled
                def return_arg_after_gc(obj):
                    callback()
                    return obj

                return return_arg_after_gc

            def make_cycle():
                obj = types.ModuleType("DeferredRCModule")
                obj.self = obj
                return {"obj": obj}, weakref.ref(obj)

            def collect_and_deref(ref):
                ref()
                gc.collect()
                gc.collect()
                return ref()

            def run_hold():
                box, weak_obj = make_cycle()

                def drop_and_collect():
                    box["obj"] = None
                    gc.collect()
                    assert weak_obj() is not None

                hold_arg_across_gc = make_hold_arg_across_gc(drop_and_collect)
                assert hold_arg_across_gc(box["obj"])
                print(
                    "HOLD",
                    collect_and_deref(weak_obj) is None,
                )

            def run_return():
                box, weak_obj = make_cycle()

                def drop_and_collect():
                    box["obj"] = None
                    gc.collect()
                    assert weak_obj() is not None

                return_arg_after_gc = make_return_arg_after_gc(drop_and_collect)
                result = return_arg_after_gc(box["obj"])
                assert result is weak_obj()
                assert result.self is result
                del result
                print(
                    "RETURN",
                    collect_and_deref(weak_obj) is None,
                )

            run_hold()
            run_return()
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", script],
            capture_output=True,
            env=cinder_support.subprocess_env(),
            text=True,
            check=True,
        )
        self.assertIn("HOLD True", proc.stdout)
        self.assertIn("RETURN True", proc.stdout)

    @cinderx.jit.jit_suppress
    def test_subprocess_batch_decref_preserves_tagged_builtin_refs(self):
        script = textwrap.dedent(
            """
            import functools
            import io
            import pickle
            import sys

            import cinderx.jit
            import cinderx.test_support as cinder_support

            @cinder_support.failUnlessJITCompiled
            def save_reduce_with_owned_dead_branch(pickler, func, args):
                save = pickler.save
                write = pickler.write
                func_name = getattr(func, "__name__", "")
                if pickler.proto >= 2 and func_name == "__newobj_ex__":
                    func = functools.partial(abs, 1)
                save(func)
                save(args)
                write(pickle.REDUCE)
                return sys.getrefcount(abs)

            assert cinderx.jit._would_tag_if_deferred(abs)
            pickler = pickle._Pickler(io.BytesIO(), 1)
            before = sys.getrefcount(abs)
            inside = save_reduce_with_owned_dead_branch(pickler, abs, (-1,))
            after = sys.getrefcount(abs)
            assert id(abs) in pickler.memo
            assert inside == before + 1
            assert after == before + 1
            print("BATCH", before, inside, after)
            """
        )
        proc = subprocess.run(
            [sys.executable, "-c", script],
            capture_output=True,
            env=cinder_support.subprocess_env(),
            text=True,
            check=True,
        )
        self.assertIn("BATCH", proc.stdout)
