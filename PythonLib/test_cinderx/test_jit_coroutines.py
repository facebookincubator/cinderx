# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-ignore[21]: Pyre doesn't know about _testcapi
import _testcapi
import asyncio
import dis
import sys
import unittest

import cinderx

cinderx.init()
import cinderx.test_support as cinder_support

from cinderx.test_support import skip_unless_jit

# Allow this file to run without CinderX.
if cinderx.is_initialized():
    from .test_compiler.test_static.common import StaticTestBase
else:
    from .test_compiler.common import CompilerTest

    StaticTestBase = CompilerTest

try:
    import cinderjit
except ImportError:
    cinderjit = None


POST_311 = sys.version_info >= (3, 11)


class CoroutinesTest(unittest.TestCase):
    def tearDown(self):
        # This is needed to avoid an "environment changed" error
        asyncio.set_event_loop_policy(None)

    @cinder_support.failUnlessJITCompiled
    async def _f1(self):
        return 1

    @cinder_support.failUnlessJITCompiled
    async def _f1(self):
        return 1

    @cinder_support.failUnlessJITCompiled
    async def _f2(self, await_target):
        return await await_target

    def test_basic_coroutine(self):
        c = self._f2(self._f1())
        with self.assertRaises(StopIteration) as exc:
            c.send(None)
        self.assertEqual(exc.exception.value, 1)

    def test_cannot_await_coro_already_awaiting_on_a_sub_iterator(self):
        class DummyAwaitable:
            def __await__(self):
                return iter([1])

        c = self._f2(DummyAwaitable())
        self.assertEqual(c.send(None), 1)
        with self.assertRaises(RuntimeError) as exc:
            self._f2(c).send(None)
        self.assertEqual(str(exc.exception), "coroutine is being awaited already")

    def test_works_with_asyncio(self):
        asyncio.run(self._f2(asyncio.sleep(0.1)))

    @unittest.skipIf(POST_311, "asyncio.coroutine removed in 3.11")
    def test_pre_async_coroutine(self):
        @asyncio.coroutine
        def _f3():
            yield 1
            return 2

        c = _f3()
        self.assertEqual(c.send(None), 1)
        with self.assertRaises(StopIteration) as exc:
            c.send(None)
        self.assertEqual(exc.exception.value, 2)

    @staticmethod
    @cinder_support.failUnlessJITCompiled
    async def _use_async_with(mgr_type):
        async with mgr_type():
            pass

    def test_bad_awaitable_in_with(self):
        class BadAEnter:
            def __aenter__(self):
                pass

            async def __aexit__(self, exc, ty, tb):
                pass

        class BadAExit:
            async def __aenter__(self):
                pass

            def __aexit__(self, exc, ty, tb):
                pass

        with self.assertRaisesRegex(
            TypeError,
            "'async with' received an object from __aenter__ "
            "that does not implement __await__: NoneType",
        ):
            asyncio.run(self._use_async_with(BadAEnter))
        with self.assertRaisesRegex(
            TypeError,
            "'async with' received an object from __aexit__ "
            "that does not implement __await__: NoneType",
        ):
            asyncio.run(self._use_async_with(BadAExit))

    class FakeFuture:
        def __init__(self, obj):
            self._obj = obj

        def __await__(self):
            i = iter([self._obj])
            self._obj = None
            return i

    @skip_unless_jit("Exercises JIT-specific bug")
    def test_jit_coro_awaits_interp_coro(self):
        @cinderjit.jit_suppress
        async def eager_suspend(suffix):
            await self.FakeFuture("hello, " + suffix)

        @cinder_support.failUnlessJITCompiled
        async def jit_coro():
            await eager_suspend("bob")

        coro = jit_coro()
        v1 = coro.send(None)
        with self.assertRaises(StopIteration):
            coro.send(None)
        self.assertEqual(v1, "hello, bob")

    def assert_already_awaited(self, coro):
        with self.assertRaisesRegex(RuntimeError, "coroutine is being awaited already"):
            asyncio.run(coro)

    def test_already_awaited_coroutine_in_try_except(self):
        """Except blocks should execute when a coroutine is already awaited"""

        async def f():
            await asyncio.sleep(0.1)

        executed_except_block = False

        async def runner():
            nonlocal executed_except_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            except RuntimeError:
                executed_except_block = True
                t.cancel()
                raise

        self.assert_already_awaited(runner())
        self.assertTrue(executed_except_block)

    def test_already_awaited_coroutine_in_try_finally(self):
        """Finally blocks should execute when a coroutine is already awaited"""

        async def f():
            await asyncio.sleep(0.1)

        executed_finally_block = False

        async def runner():
            nonlocal executed_finally_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            finally:
                executed_finally_block = True
                t.cancel()

        self.assert_already_awaited(runner())
        self.assertTrue(executed_finally_block)

    def test_already_awaited_coroutine_in_try_except_finally(self):
        """Except and finally blocks should execute when a coroutine is already
        awaited.
        """

        async def f():
            await asyncio.sleep(0.1)

        executed_except_block = False
        executed_finally_block = False

        async def runner():
            nonlocal executed_except_block, executed_finally_block
            coro = f()
            loop = asyncio.get_running_loop()
            t = loop.create_task(coro)
            try:
                await asyncio.sleep(0)
                await coro
            except RuntimeError:
                executed_except_block = True
                raise
            finally:
                executed_finally_block = True
                t.cancel()

        self.assert_already_awaited(runner())
        self.assertTrue(executed_except_block)
        self.assertTrue(executed_finally_block)


@unittest.skipUnless(
    cinderx.is_initialized() and hasattr(_testcapi, "TestAwaitedCall"),
    "Tests CinderX eager coroutine dispatch and needs extra support in _testcapi",
)
class EagerCoroutineDispatch(StaticTestBase):
    def tearDown(self):
        # This is needed to avoid an "environment changed" error
        asyncio.set_event_loop_policy(None)

    def _assert_awaited_flag_seen(self, async_f_under_test):
        awaited_capturer = _testcapi.TestAwaitedCall()
        self.assertIsNone(awaited_capturer.last_awaited())
        coro = async_f_under_test(awaited_capturer)
        # TestAwaitedCall doesn't actually return a coroutine. This doesn't
        # matter though because by the time a TypeError is raised we run far
        # enough to know if the awaited flag was passed.
        with self.assertRaisesRegex(
            TypeError, r".*can't be used in 'await' expression"
        ):
            coro.send(None)
        coro.close()
        self.assertTrue(awaited_capturer.last_awaited())
        self.assertIsNone(awaited_capturer.last_awaited())

    def _assert_awaited_flag_not_seen(self, async_f_under_test):
        awaited_capturer = _testcapi.TestAwaitedCall()
        self.assertIsNone(awaited_capturer.last_awaited())
        coro = async_f_under_test(awaited_capturer)
        with self.assertRaises(StopIteration):
            coro.send(None)
        coro.close()
        self.assertFalse(awaited_capturer.last_awaited())
        self.assertIsNone(awaited_capturer.last_awaited())

    @cinder_support.failUnlessJITCompiled
    async def _call_ex(self, t):
        t(*[1])

    @cinder_support.failUnlessJITCompiled
    async def _call_ex_awaited(self, t):
        await t(*[1])

    @cinder_support.failUnlessJITCompiled
    async def _call_ex_kw(self, t):
        t(*[1], **{"2": 3})

    @cinder_support.failUnlessJITCompiled
    async def _call_ex_kw_awaited(self, t):
        await t(*[1], **{"2": 3})

    @cinder_support.failUnlessJITCompiled
    async def _call_method(self, t):
        # https://stackoverflow.com/questions/19476816/creating-an-empty-object-in-python
        o = type("", (), {})()
        o.t = t
        o.t()

    @cinder_support.failUnlessJITCompiled
    async def _call_method_awaited(self, t):
        o = type("", (), {})()
        o.t = t
        await o.t()

    @cinder_support.failUnlessJITCompiled
    async def _vector_call(self, t):
        t()

    @cinder_support.failUnlessJITCompiled
    async def _vector_call_awaited(self, t):
        await t()

    @cinder_support.failUnlessJITCompiled
    async def _vector_call_kw(self, t):
        t(a=1)

    @cinder_support.failUnlessJITCompiled
    async def _vector_call_kw_awaited(self, t):
        await t(a=1)

    def test_call_ex(self):
        self._assert_awaited_flag_not_seen(self._call_ex)

    def test_call_ex_awaited(self):
        self._assert_awaited_flag_seen(self._call_ex_awaited)

    def test_call_ex_kw(self):
        self._assert_awaited_flag_not_seen(self._call_ex_kw)

    def test_call_ex_kw_awaited(self):
        self._assert_awaited_flag_seen(self._call_ex_kw_awaited)

    def test_call_method(self):
        self._assert_awaited_flag_not_seen(self._call_method)

    def test_call_method_awaited(self):
        self._assert_awaited_flag_seen(self._call_method_awaited)

    def test_vector_call(self):
        self._assert_awaited_flag_not_seen(self._vector_call)

    def test_vector_call_awaited(self):
        self._assert_awaited_flag_seen(self._vector_call_awaited)

    def test_vector_call_kw(self):
        self._assert_awaited_flag_not_seen(self._vector_call_kw)

    def test_vector_call_kw_awaited(self):
        self._assert_awaited_flag_seen(self._vector_call_kw_awaited)

    def test_invoke_function(self):
        codestr = f"""
        async def x() -> None:
            pass

        async def await_x() -> None:
            await x()

        # Exercise call path through Ci_PyFunction_CallStatic
        async def await_await_x() -> None:
            await await_x()

        async def call_x() -> None:
            c = x()
        """
        with self.in_module(codestr, name="test_invoke_function") as mod:
            self.assertInBytecode(
                mod.await_x, "INVOKE_FUNCTION", ((("test_invoke_function",), "x"), 0)
            )
            self.assertInBytecode(
                mod.await_await_x,
                "INVOKE_FUNCTION",
                ((("test_invoke_function",), "await_x"), 0),
            )
            self.assertInBytecode(
                mod.call_x, "INVOKE_FUNCTION", ((("test_invoke_function",), "x"), 0)
            )
            mod.x = _testcapi.TestAwaitedCall()
            self.assertIsInstance(mod.x, _testcapi.TestAwaitedCall)
            self.assertIsNone(mod.x.last_awaited())
            coro = mod.await_await_x()
            with self.assertRaisesRegex(
                TypeError, r".*can't be used in 'await' expression"
            ):
                coro.send(None)
            coro.close()
            self.assertTrue(mod.x.last_awaited())
            self.assertIsNone(mod.x.last_awaited())
            coro = mod.call_x()
            with self.assertRaises(StopIteration):
                coro.send(None)
            coro.close()
            self.assertFalse(mod.x.last_awaited())
            if cinderjit and cinderjit.auto_jit_threshold() <= 1:
                self.assertTrue(cinderjit.is_jit_compiled(mod.await_x))
                self.assertTrue(cinderjit.is_jit_compiled(mod.call_x))

    def test_invoke_method(self):
        codestr = f"""
        class X:
            async def x(self) -> None:
                pass

        async def await_x(x: X) -> None:
            await x.x()

        async def call_x(x: X) -> None:
            x.x()
        """
        with self.in_module(codestr, name="test_invoke_method") as mod:
            self.assertInBytecode(
                mod.await_x,
                "INVOKE_METHOD",
                (
                    (
                        (
                            "test_invoke_method",
                            "X",
                        ),
                        "x",
                    ),
                    0,
                ),
            )
            self.assertInBytecode(
                mod.call_x,
                "INVOKE_METHOD",
                (
                    (
                        (
                            "test_invoke_method",
                            "X",
                        ),
                        "x",
                    ),
                    0,
                ),
            )
            awaited_capturer = mod.X.x = _testcapi.TestAwaitedCall()
            self.assertIsNone(awaited_capturer.last_awaited())
            coro = mod.await_x(mod.X())
            with self.assertRaisesRegex(
                TypeError, r".*can't be used in 'await' expression"
            ):
                coro.send(None)
            coro.close()
            self.assertTrue(awaited_capturer.last_awaited())
            self.assertIsNone(awaited_capturer.last_awaited())
            coro = mod.call_x(mod.X())
            with self.assertRaises(StopIteration):
                coro.send(None)
            coro.close()
            self.assertFalse(awaited_capturer.last_awaited())
            if cinderjit and cinderjit.auto_jit_threshold() <= 1:
                self.assertTrue(cinderjit.is_jit_compiled(mod.await_x))
                self.assertTrue(cinderjit.is_jit_compiled(mod.call_x))

        async def y():
            await DummyAwaitable()

    def test_async_yielding(self):
        class DummyAwaitable:
            def __await__(self):
                return iter([1, 2])

        coro = self._vector_call_awaited(DummyAwaitable)
        self.assertEqual(coro.send(None), 1)
        self.assertEqual(coro.send(None), 2)

    @cinder_support.failUnlessJITCompiled
    async def _f4(self):
        "Function must have a docstring, so None is not first constant."
        # Now we need >255 constants, none of which are None. Keyword args will work.
        # And then an awaited async call, where we should get the bytecode
        # CALL_FUNCTION_EX; GET_AWAITABLE; EXTENDED_ARG; LOAD_CONST None; YIELD_FROM
        return await self._f5(
            k000=1,
            k001=1,
            k002=1,
            k003=1,
            k004=1,
            k005=1,
            k006=1,
            k007=1,
            k008=1,
            k009=1,
            k010=1,
            k011=1,
            k012=1,
            k013=1,
            k014=1,
            k015=1,
            k016=1,
            k017=1,
            k018=1,
            k019=1,
            k020=1,
            k021=1,
            k022=1,
            k023=1,
            k024=1,
            k025=1,
            k026=1,
            k027=1,
            k028=1,
            k029=1,
            k030=1,
            k031=1,
            k032=1,
            k033=1,
            k034=1,
            k035=1,
            k036=1,
            k037=1,
            k038=1,
            k039=1,
            k040=1,
            k041=1,
            k042=1,
            k043=1,
            k044=1,
            k045=1,
            k046=1,
            k047=1,
            k048=1,
            k049=1,
            k050=1,
            k051=1,
            k052=1,
            k053=1,
            k054=1,
            k055=1,
            k056=1,
            k057=1,
            k058=1,
            k059=1,
            k060=1,
            k061=1,
            k062=1,
            k063=1,
            k064=1,
            k065=1,
            k066=1,
            k067=1,
            k068=1,
            k069=1,
            k070=1,
            k071=1,
            k072=1,
            k073=1,
            k074=1,
            k075=1,
            k076=1,
            k077=1,
            k078=1,
            k079=1,
            k080=1,
            k081=1,
            k082=1,
            k083=1,
            k084=1,
            k085=1,
            k086=1,
            k087=1,
            k088=1,
            k089=1,
            k090=1,
            k091=1,
            k092=1,
            k093=1,
            k094=1,
            k095=1,
            k096=1,
            k097=1,
            k098=1,
            k099=1,
            k100=1,
            k101=1,
            k102=1,
            k103=1,
            k104=1,
            k105=1,
            k106=1,
            k107=1,
            k108=1,
            k109=1,
            k110=1,
            k111=1,
            k112=1,
            k113=1,
            k114=1,
            k115=1,
            k116=1,
            k117=1,
            k118=1,
            k119=1,
            k120=1,
            k121=1,
            k122=1,
            k123=1,
            k124=1,
            k125=1,
            k126=1,
            k127=1,
            k128=1,
            k129=1,
            k130=1,
            k131=1,
            k132=1,
            k133=1,
            k134=1,
            k135=1,
            k136=1,
            k137=1,
            k138=1,
            k139=1,
            k140=1,
            k141=1,
            k142=1,
            k143=1,
            k144=1,
            k145=1,
            k146=1,
            k147=1,
            k148=1,
            k149=1,
            k150=1,
            k151=1,
            k152=1,
            k153=1,
            k154=1,
            k155=1,
            k156=1,
            k157=1,
            k158=1,
            k159=1,
            k160=1,
            k161=1,
            k162=1,
            k163=1,
            k164=1,
            k165=1,
            k166=1,
            k167=1,
            k168=1,
            k169=1,
            k170=1,
            k171=1,
            k172=1,
            k173=1,
            k174=1,
            k175=1,
            k176=1,
            k177=1,
            k178=1,
            k179=1,
            k180=1,
            k181=1,
            k182=1,
            k183=1,
            k184=1,
            k185=1,
            k186=1,
            k187=1,
            k188=1,
            k189=1,
            k190=1,
            k191=1,
            k192=1,
            k193=1,
            k194=1,
            k195=1,
            k196=1,
            k197=1,
            k198=1,
            k199=1,
            k200=1,
            k201=1,
            k202=1,
            k203=1,
            k204=1,
            k205=1,
            k206=1,
            k207=1,
            k208=1,
            k209=1,
            k210=1,
            k211=1,
            k212=1,
            k213=1,
            k214=1,
            k215=1,
            k216=1,
            k217=1,
            k218=1,
            k219=1,
            k220=1,
            k221=1,
            k222=1,
            k223=1,
            k224=1,
            k225=1,
            k226=1,
            k227=1,
            k228=1,
            k229=1,
            k230=1,
            k231=1,
            k232=1,
            k233=1,
            k234=1,
            k235=1,
            k236=1,
            k237=1,
            k238=1,
            k239=1,
            k240=1,
            k241=1,
            k242=1,
            k243=1,
            k244=1,
            k245=1,
            k246=1,
            k247=1,
            k248=1,
            k249=1,
            k250=1,
            k251=1,
            k252=1,
            k253=1,
            k254=1,
        )

    async def _f5(self, **kw):
        return kw

    def test_awaited_call_extended_arg(self):
        # verify we are actually testing what we intend to
        instrs = dis.get_instructions(self._f4)
        expected_instrs = [
            "CALL_FUNCTION_EX",
            "GET_AWAITABLE",
            "EXTENDED_ARG",
            "LOAD_CONST",
            "YIELD_FROM",
            "RETURN_VALUE",
        ]
        self.assertEqual([i.opname for i in list(instrs)[-6:]], expected_instrs)
        # ok, the function has the expected bytecode; if the bug isn't fixed,
        # JIT compilation wouldn't even have allowed us to get this far.
        # But let's try calling it too:
        self.assertEqual(len(asyncio.run(self._f4())), 255)
