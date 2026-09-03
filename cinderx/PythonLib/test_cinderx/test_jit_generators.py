# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-unsafe

import gc
import inspect
import sys
import threading
import unittest
import weakref

import cinderx.test_support as cinder_support
from cinderx.jit import _deopt_gen, get_compiled_spill_stack_size, is_jit_compiled

from .common import with_globals


class GeneratorsTest(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def _f1(self):
        yield 1

    def test_basic_operation(self):
        g = self._f1()
        self.assertEqual(g.send(None), 1)
        with self.assertRaises(StopIteration) as exc:
            g.send(None)
        self.assertIsNone(exc.exception.value)

    @cinder_support.failUnlessJITCompiled
    def _f2(self):
        yield 1
        yield 2
        return 3  # noqa: B901

    def test_multi_yield_and_return(self):
        g = self._f2()
        self.assertEqual(g.send(None), 1)
        self.assertEqual(g.send(None), 2)
        with self.assertRaises(StopIteration) as exc:
            g.send(None)
        self.assertEqual(exc.exception.value, 3)

    @cinder_support.failUnlessJITCompiled
    def _f3(self):
        a = yield 1
        b = yield 2
        return a + b

    def test_receive_values(self):
        g = self._f3()
        self.assertEqual(g.send(None), 1)
        self.assertEqual(g.send(100), 2)
        with self.assertRaises(StopIteration) as exc:
            g.send(1000)
        self.assertEqual(exc.exception.value, 1100)

    @cinder_support.failUnlessJITCompiled
    def _f4(self, a):
        yield a
        yield a
        return a  # noqa: B901

    def test_one_arg(self):
        g = self._f4(10)
        self.assertEqual(g.send(None), 10)
        self.assertEqual(g.send(None), 10)
        with self.assertRaises(StopIteration) as exc:
            g.send(None)
        self.assertEqual(exc.exception.value, 10)

    @cinder_support.failUnlessJITCompiled
    def _f5(
        self, a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16
    ):
        v = (
            yield a1
            + a2
            + a3
            + a4
            + a5
            + a6
            + a7
            + a8
            + a9
            + a10
            + a11
            + a12
            + a13
            + a14
            + a15
            + a16
        )
        a1 <<= v
        a2 <<= v
        a3 <<= v
        a4 <<= v
        a5 <<= v
        a6 <<= v
        a7 <<= v
        a8 <<= v
        a9 <<= v
        a10 <<= v
        a11 <<= v
        a12 <<= v
        a13 <<= v
        a14 <<= v
        a15 <<= v
        a16 <<= v
        v = (
            yield a1
            + a2
            + a3
            + a4
            + a5
            + a6
            + a7
            + a8
            + a9
            + a10
            + a11
            + a12
            + a13
            + a14
            + a15
            + a16
        )
        a1 <<= v
        a2 <<= v
        a3 <<= v
        a4 <<= v
        a5 <<= v
        a6 <<= v
        a7 <<= v
        a8 <<= v
        a9 <<= v
        a10 <<= v
        a11 <<= v
        a12 <<= v
        a13 <<= v
        a14 <<= v
        a15 <<= v
        a16 <<= v
        return (
            a1
            + a2
            + a3
            + a4
            + a5
            + a6
            + a7
            + a8
            + a9
            + a10
            + a11
            + a12
            + a13
            + a14
            + a15
            + a16
        )

    def test_save_all_registers_and_spill(self):
        g = self._f5(
            0x1,
            0x2,
            0x4,
            0x8,
            0x10,
            0x20,
            0x40,
            0x80,
            0x100,
            0x200,
            0x400,
            0x800,
            0x1000,
            0x2000,
            0x4000,
            0x8000,
        )
        self.assertEqual(g.send(None), 0xFFFF)
        self.assertEqual(g.send(1), 0xFFFF << 1)
        with self.assertRaises(StopIteration) as exc:
            g.send(2)
        self.assertEqual(exc.exception.value, 0xFFFF << 3)

    def test_for_loop_driven(self):
        li = []
        for x in self._f2():
            li.append(x)
        self.assertEqual(li, [1, 2])

    @cinder_support.failUnlessJITCompiled
    def _f6(self):
        i = 0
        while i < 1000:
            i = yield i

    def test_many_iterations(self):
        g = self._f6()
        self.assertEqual(g.send(None), 0)
        for i in range(1, 1000):
            self.assertEqual(g.send(i), i)
        with self.assertRaises(StopIteration) as exc:
            g.send(1000)
        self.assertIsNone(exc.exception.value)

    def _f_raises(self):
        raise ValueError

    @cinder_support.failUnlessJITCompiled
    def _f7(self):
        self._f_raises()
        yield 1

    def test_raise(self):
        g = self._f7()
        with self.assertRaises(ValueError):
            g.send(None)

    def test_throw_into_initial_yield(self):
        g = self._f1()
        with self.assertRaises(ValueError):
            g.throw(ValueError)

    def test_throw_into_yield(self):
        g = self._f2()
        self.assertEqual(g.send(None), 1)
        with self.assertRaises(ValueError):
            g.throw(ValueError)

    def test_close_on_initial_yield(self):
        g = self._f1()
        g.close()

    def test_close_on_yield(self):
        g = self._f2()
        self.assertEqual(g.send(None), 1)
        g.close()

    @cinder_support.failUnlessJITCompiled
    def _f8(self, a):
        x += yield a  # noqa: F821, F841

    def test_do_not_deopt_before_initial_yield(self):
        g = self._f8(1)
        with self.assertRaises(UnboundLocalError):
            g.send(None)

    @cinder_support.failUnlessJITCompiled
    def _f9(self, a):
        yield
        return a  # noqa: B901

    def test_incref_args(self):
        class X:
            pass

        g = self._f9(X())
        g.send(None)
        with self.assertRaises(StopIteration) as exc:
            g.send(None)
        self.assertIsInstance(exc.exception.value, X)

    def test_sizeof_reports_the_jit_data_size(self):
        func = self._f4.__func__
        g = self._f4(1)
        next(g)
        size = g.__sizeof__()

        # A JIT generator carries the GenDataFooter, a pointer to it and the
        # register spill area on top of what CPython reports.  Only the spill
        # area varies, and it comes from the compiled function.
        spill = get_compiled_spill_stack_size(func) if is_jit_compiled(func) else 0
        self.assertGreater(size, spill)
        self.assertLess(size - spill, 4096)

    @cinder_support.failUnlessJITCompiled
    def _f10(self, X):
        x = X()
        yield weakref.ref(x)
        return x  # noqa: B901

    def test_gc_traversal(self):
        class X:
            pass

        g = self._f10(X)

        weak_ref_x = g.send(None)
        self.assertTrue(any(weak_ref_x() is obj for obj in gc.get_objects()))
        referrers = gc.get_referrers(weak_ref_x())
        self.assertEqual(len(referrers), 1)
        self.assertIs(referrers[0], g)
        with self.assertRaises(StopIteration):
            g.send(None)

    def test_resuming_in_another_thread(self):
        g = self._f1()

        def thread_function(g):
            self.assertEqual(g.send(None), 1)
            with self.assertRaises(StopIteration):
                g.send(None)

        t = threading.Thread(target=thread_function, args=(g,))
        t.start()
        t.join()

    def test_release_data_on_discard(self):
        o = object()
        base_count = sys.getrefcount(o)
        g = self._f9(o)
        self.assertEqual(sys.getrefcount(o), base_count + 1)
        del g
        self.assertEqual(sys.getrefcount(o), base_count)

    @cinder_support.failUnlessJITCompiled
    def _f11(self, obj):
        # obj is deliberately never read, so its last use is the top of the
        # body.  CPython still holds it until the frame is torn down.
        return
        yield

    # A generator's locals outlive its frame state: whatever the release of a
    # local runs must see the generator as closed, not as still executing.
    def test_frame_is_finished_before_locals_are_released(self):
        seen = []

        class Probe:
            def __del__(self):
                seen.append(inspect.getgeneratorstate(g))
                seen.append(g.gi_frame)
                try:
                    next(g)
                except StopIteration:
                    seen.append("StopIteration")

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [inspect.GEN_CLOSED, None, "StopIteration"])

    def test_closing_from_a_local_release_is_silent(self):
        seen = []

        class Probe:
            def __del__(self):
                seen.append(g.close())

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [None])

    def test_sending_from_a_local_release_stops_iteration(self):
        seen = []

        class Probe:
            def __del__(self):
                try:
                    g.send(1)
                except StopIteration as exc:
                    seen.append(exc.value)

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [None])

    def test_throwing_from_a_local_release_propagates_the_exception(self):
        seen = []

        class Probe:
            def __del__(self):
                try:
                    g.throw(ValueError("thrown"))
                except ValueError as exc:
                    seen.append(str(exc))

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, ["thrown"])

    def test_deopting_from_a_local_release_is_refused(self):
        seen = []

        class Probe:
            def __del__(self):
                seen.append(_deopt_gen(g))

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [False])

    def test_resuming_a_coroutine_from_a_local_release_is_an_error(self):
        @cinder_support.failUnlessJITCompiled
        async def coro(obj):
            pass

        seen = []

        class Probe:
            def __del__(self):
                try:
                    c.send(None)
                except RuntimeError as exc:
                    seen.append(str(exc))

        c = coro(Probe())
        with self.assertRaises(StopIteration):
            c.send(None)

        self.assertEqual(seen, ["cannot reuse already awaited coroutine"])

    def test_throwing_into_a_coroutine_from_a_local_release_is_an_error(self):
        @cinder_support.failUnlessJITCompiled
        async def coro(obj):
            pass

        seen = []

        class Probe:
            def __del__(self):
                try:
                    c.throw(ValueError("thrown"))
                except RuntimeError as exc:
                    seen.append(str(exc))

        c = coro(Probe())
        with self.assertRaises(StopIteration):
            c.send(None)

        self.assertEqual(seen, ["cannot reuse already awaited coroutine"])

    def test_frame_is_finished_before_locals_are_released_after_a_yield(self):
        # The generator suspends before it finishes, so the frame state is
        # written on a resume rather than on the first send.
        @cinder_support.failUnlessJITCompiled
        def gen(obj):
            yield 1
            yield 2

        seen = []

        class Probe:
            def __del__(self):
                seen.append(inspect.getgeneratorstate(g))

        g = gen(Probe())
        self.assertEqual(list(g), [1, 2])

        self.assertEqual(seen, [inspect.GEN_CLOSED])

    def test_frame_is_finished_before_cells_are_released(self):
        # obj is captured by inner, so it lives in localsplus as a cell rather
        # than as a plain local.  The same ordering has to hold for those.
        @cinder_support.failUnlessJITCompiled
        def gen(obj):
            def inner():
                return obj

            return
            yield

        seen = []

        class Probe:
            def __del__(self):
                seen.append(inspect.getgeneratorstate(g))

        g = gen(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [inspect.GEN_CLOSED])

    def test_collecting_from_a_local_release_is_safe(self):
        # A collection in this window traverses a generator whose frame state
        # already says cleared while its frame is still populated.
        seen = []

        class Probe:
            def __del__(self):
                gc.collect()
                seen.append(inspect.getgeneratorstate(g))

        g = self._f11(Probe())
        with self.assertRaises(StopIteration):
            next(g)

        self.assertEqual(seen, [inspect.GEN_CLOSED])

    def test_locals_do_not_outlive_the_generator_frame(self):
        # Holding the locals to the Return must not hold them past it.
        class Probe:
            pass

        probe = Probe()
        ref = weakref.ref(probe)
        g = self._f11(probe)
        del probe

        with self.assertRaises(StopIteration):
            next(g)

        self.assertIsNone(ref())

    def test_weakref_callback_on_discard_before_resume(self):
        callbacks = []
        g = self._f1()
        g_ref = weakref.ref(g, callbacks.append)
        self.assertIs(g_ref(), g)

        del g

        self.assertIsNone(g_ref())
        self.assertEqual(callbacks, [g_ref])

    def test_gc_collects_unstarted_generator_cycle(self):
        class Cycle:
            pass

        cycle = Cycle()
        g = self._f9(cycle)
        cycle.generator = g
        cycle_ref = weakref.ref(cycle)
        g_ref = weakref.ref(g)

        del cycle
        del g
        gc.collect()

        self.assertIsNone(cycle_ref())
        self.assertIsNone(g_ref())

    def test_attributes_after_close_before_resume(self):
        g = self._f1()
        frame = g.gi_frame
        code = g.gi_code
        name = g.__name__
        qualname = g.__qualname__

        g.close()

        self.assertIsNone(g.gi_frame)
        self.assertIs(g.gi_code, code)
        self.assertEqual(g.__name__, name)
        self.assertEqual(g.__qualname__, qualname)
        self.assertIsNotNone(frame)

    @cinder_support.failUnlessJITCompiled
    def _f12(self, g):
        a = yield from g
        return a

    def test_yield_from_generator(self):
        g = self._f12(self._f2())
        self.assertEqual(g.send(None), 1)
        self.assertEqual(g.send(None), 2)
        with self.assertRaises(StopIteration) as exc:
            g.send(None)
        self.assertEqual(exc.exception.value, 3)

    def test_yield_from_iterator(self):
        g = self._f12([1, 2])
        self.assertEqual(g.send(None), 1)
        self.assertEqual(g.send(None), 2)
        with self.assertRaises(StopIteration):
            g.send(None)

    def test_yield_from_forwards_raise_down(self):
        def f():
            try:
                yield 1
            except ValueError:
                return 2  # noqa: B901
            return 3

        g = self._f12(f())
        self.assertEqual(g.send(None), 1)
        with self.assertRaises(StopIteration) as exc:
            g.throw(ValueError)
        self.assertEqual(exc.exception.value, 2)

    def test_yield_from_forwards_raise_up(self):
        def f():
            raise ValueError
            yield 1

        g = self._f12(f())
        with self.assertRaises(ValueError):
            g.send(None)

    def test_yield_from_passes_raise_through(self):
        g = self._f12(self._f2())
        self.assertEqual(g.send(None), 1)
        with self.assertRaises(ValueError):
            g.throw(ValueError)

    def test_yield_from_forwards_close_down(self):
        saw_close = False

        def f():
            nonlocal saw_close
            try:
                yield 1
            except GeneratorExit:
                saw_close = True
                return 2  # noqa: B901

        g = self._f12(f())
        self.assertEqual(g.send(None), 1)
        g.close()
        self.assertTrue(saw_close)

    def test_yield_from_passes_close_through(self):
        g = self._f12(self._f2())
        self.assertEqual(g.send(None), 1)
        g.close()

    def test_assert_on_yield_from_coro(self):
        async def coro():
            pass

        c = coro()
        with self.assertRaises(TypeError) as exc:
            self._f12(c).send(None)
        self.assertEqual(
            str(exc.exception),
            "cannot 'yield from' a coroutine object in a non-coroutine generator",
        )

        # Suppress warning
        c.close()

    def test_gen_freelist(self):
        """Exercise making a JITted generator with gen_data memory off the freelist."""
        # make and dealloc a small coro, which will put its memory area on the freelist
        sc = self.small_coro()
        with self.assertRaises(StopIteration):
            sc.send(None)
        del sc
        # run another coro to verify we didn't put a bad pointer on the freelist
        sc2 = self.small_coro()
        with self.assertRaises(StopIteration):
            sc2.send(None)
        del sc2
        # make a big coro and then deallocate it, bypassing the freelist
        bc = self.big_coro()
        with self.assertRaises(StopIteration):
            bc.send(None)
        del bc

    @cinder_support.failUnlessJITCompiled
    async def big_coro(self):
        # This currently results in a max spill size of ~100, but that could
        # change with JIT register allocation improvements. This test is only
        # testing what it intends to as long as the max spill size of this
        # function is greater than jit::kMinGenSpillWords. Ideally we'd assert
        # that in the test, but neither value is introspectable from Python.
        return dict(  # noqa: C408
            a=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            b=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            c=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            d=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            e=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            f=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            g=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
            h=dict(a=1, b=2, c=3, d=4, e=5, f=6, g=7, h=8, i=9),  # noqa: C408
        )

    @cinder_support.failUnlessJITCompiled
    async def small_coro(self):
        return 1

    def test_generator_globals(self):
        val1 = "a value"
        val2 = "another value"
        gbls = {"A_GLOBAL": val1}

        @with_globals(gbls)
        def gen():
            yield A_GLOBAL  # noqa: F821
            yield A_GLOBAL  # noqa: F821

        g = gen()
        self.assertIs(g.__next__(), val1)
        gbls["A_GLOBAL"] = val2
        del gbls
        self.assertIs(g.__next__(), val2)
        with self.assertRaises(StopIteration):
            g.__next__()

    def test_deopt_at_initial_yield(self):
        @cinder_support.failUnlessJITCompiled
        def gen(a, b):
            yield a
            return a + b  # noqa: B901

        g = gen(3, 8)
        self.assertEqual(_deopt_gen(g), is_jit_compiled(gen))
        self.assertEqual(next(g), 3)
        with self.assertRaises(StopIteration) as cm:
            next(g)
        self.assertEqual(cm.exception.value, 11)

    def test_deopt_at_yield(self):
        @cinder_support.failUnlessJITCompiled
        def gen(a, b):
            yield a
            return a * b  # noqa: B901

        g = gen(5, 9)
        self.assertEqual(next(g), 5)
        self.assertEqual(_deopt_gen(g), is_jit_compiled(gen))
        with self.assertRaises(StopIteration) as cm:
            next(g)
        self.assertEqual(cm.exception.value, 45)

    def test_deopt_at_yield_from(self):
        @cinder_support.failUnlessJITCompiled
        def gen(li):
            yield from iter(li)

        g = gen([2, 4, 6])
        self.assertEqual(next(g), 2)
        self.assertEqual(_deopt_gen(g), is_jit_compiled(gen))
        self.assertEqual(next(g), 4)
        self.assertEqual(next(g), 6)
        with self.assertRaises(StopIteration) as cm:
            next(g)
        self.assertEqual(cm.exception.value, None)

    def test_deopt_at_yield_from_handle_stop_async_iteration(self):
        class BusyWait:
            def __await__(self):
                return iter(["one", "two"])

        class AsyncIter:
            def __init__(self, li):
                self._iter = iter(li)

            async def __anext__(self):
                try:
                    item = next(self._iter)
                except StopIteration:
                    raise StopAsyncIteration

                await BusyWait()
                return item

        class AsyncList:
            def __init__(self, li):
                self._list = li

            def __aiter__(self):
                return AsyncIter(self._list)

        @cinder_support.failUnlessJITCompiled
        async def coro(l1, l2):
            async for i in AsyncList(l1):
                l2.append(i * 2)
            return l2

        li = []
        c = coro([7, 8], li)
        it = iter(c.__await__())
        self.assertEqual(next(it), "one")
        self.assertEqual(li, [])
        self.assertEqual(_deopt_gen(c), is_jit_compiled(coro))
        self.assertEqual(next(it), "two")
        self.assertEqual(li, [])
        self.assertEqual(next(it), "one")
        self.assertEqual(li, [14])
        self.assertEqual(next(it), "two")
        self.assertEqual(li, [14])
        with self.assertRaises(StopIteration) as cm:
            next(it)
        self.assertIs(cm.exception.value, li)
        self.assertEqual(li, [14, 16])

    # TASK(T125856469): Once we support eager execution of coroutines, add
    # tests that deopt while suspended at YieldAndYieldFrom.

    def test_yield_local_and_deopt(self):
        """The JIT must keep a strong reference to a local variable it yields
        and does not use, even though it could just let ownership be transferred
        to the yield receiver. This is because the interpreter would keep a
        strong reference and we may transfer control to the interpreter if we
        deopt the suspended generator."""

        @cinder_support.failUnlessJITCompiled
        def gen_yield_local(obj):
            yield obj

        obj = object()
        g = gen_yield_local(obj)
        del obj
        gc.collect()
        next(g)
        _deopt_gen(g)
        # If things are broken we'll get a use-after-free error from ASAN here.
        with self.assertRaises(StopIteration):
            next(g)


class GeneratorFrameTest(unittest.TestCase):
    @cinder_support.failUnlessJITCompiled
    def gen1(self):
        a = 1
        yield a
        a = 2
        yield a

    def test_access_before_send(self):
        g = self.gen1()
        f = g.gi_frame
        self.assertEqual(next(g), 1)
        self.assertEqual(g.gi_frame, f)
        self.assertEqual(next(g), 2)
        self.assertEqual(g.gi_frame, f)

    def test_access_after_send(self):
        g = self.gen1()
        self.assertEqual(next(g), 1)
        f = g.gi_frame
        self.assertEqual(next(g), 2)
        self.assertEqual(g.gi_frame, f)

    @cinder_support.failUnlessJITCompiled
    def gen2(self):
        me = yield
        f = me.gi_frame
        yield f
        yield 10

    def test_access_while_running(self):
        g = self.gen2()
        next(g)
        f = g.send(g)
        self.assertEqual(f, g.gi_frame)
        next(g)
