// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

// clang-format off
// This needs to come first on 3.14+ so it can redefine
// _PyFrame_MakeAndSetFrameObject before it's used by the static inline
// definition of _PyFrame_GetFrameObject in pycore_interpframe.h.
#include "cinderx/UpstreamBorrow/borrowed.h" // @donotremove
// clang-format on

#include "cinderx/Common/code.h"
#include "cinderx/Common/log.h"
#include "cinderx/Common/ref.h"
#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/cinder_opcode.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/compiler.h"
#include "cinderx/Jit/deopt.h"
// NOLINTNEXTLINE(facebook-unused-include-check)
#include "cinderx/Jit/frame.h"
#include "cinderx/Jit/hir/builder.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/hir/printer.h"
#include "cinderx/RuntimeTests/fixtures.h"

#if PY_VERSION_HEX >= 0x030C0000
#include "internal/pycore_frame.h"
#endif

#include <algorithm>

using namespace jit;
using namespace jit::hir;
using namespace jit::codegen;
using jit::kPointerSize;

class ReifyFrameTest : public RuntimeTest {};

static inline Ref<> runInInterpreterViaReify(
    BorrowedRef<PyFunctionObject> func,
    const DeoptMetadata& dm,
    const DeoptFrameMetadata& dfm,
    uint64_t regs[NUM_GP_REGS]) {
#if PY_VERSION_HEX < 0x030C0000
  PyThreadState* tstate = PyThreadState_Get();
  PyCodeObject* code =
      reinterpret_cast<PyCodeObject*>(PyFunction_GetCode(func));
  auto frame = Ref<PyFrameObject>::steal(
      PyFrame_New(tstate, code, PyFunction_GetGlobals(func), nullptr));

  reifyFrame(frame, dm, dfm, regs);

  return Ref<>::steal(PyEval_EvalFrame(frame));
#else
  PyThreadState* tstate = PyThreadState_Get();
  BorrowedRef<PyCodeObject> code = PyFunction_GetCode(func);
  _PyInterpreterFrame* interp_frame =
      Cix_PyThreadState_PushFrame(tstate, jit::jitFrameGetSize(code));
  jit::jitFrameInit(
      tstate,
      interp_frame,
      func,
      code,
      0,
      FRAME_OWNED_BY_THREAD,
      nullptr,
      makeFrameReifier(code));
  if (getConfig().frame_mode == FrameMode::kLightweight) {
    jit::jitFramePopulateFrame(interp_frame);
    jit::jitFrameRemoveReifier(interp_frame);
  }
  reifyFrame(interp_frame, dm, dfm, regs);
  // If we're at the start of the function, push IP past RESUME instruction
#if PY_VERSION_HEX >= 0x030E0000
  if (interp_frame->instr_ptr == _PyCode_CODE(code)) {
    interp_frame->instr_ptr = _PyCode_CODE(code) + code->_co_firsttraceable;
  }
#else
  if (interp_frame->prev_instr == _PyCode_CODE(code) - 1) {
    interp_frame->prev_instr = _PyCode_CODE(code) + code->_co_firsttraceable;
  }
#endif
  // PyEval_EvalFrame seems to steal the frame.
  PyFrameObject* frame_obj = _PyFrame_GetFrameObject(interp_frame);
#if PY_VERSION_HEX >= 0x030E0000
  _Py_Instrument(frameCode(interp_frame), tstate->interp);
#endif
  return Ref<>::steal(PyEval_EvalFrame(frame_obj));
#endif
}

TEST_F(ReifyFrameTest, ReifyAtEntry) {
  const char* src = R"(
def test(a, b):
  return a + b
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  uint64_t regs[NUM_GP_REGS];

  auto a = Ref<>::steal(PyLong_FromLong(10));
  ASSERT_NE(a, nullptr);
  regs[ARGUMENT_REGS[0].loc] = reinterpret_cast<uint64_t>(a.get());
  LiveValue a_val{
      PhyLocation{ARGUMENT_REGS[0].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  auto b = Ref<>::steal(PyLong_FromLong(20));
  ASSERT_NE(b, nullptr);
  regs[ARGUMENT_REGS[1].loc] = reinterpret_cast<uint64_t>(b.get());
  LiveValue b_val{
      PhyLocation{ARGUMENT_REGS[1].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  DeoptMetadata dm;
  dm.live_values = {a_val, b_val};
  dm.reason = DeoptReason::kGuardFailure;
  DeoptFrameMetadata dfm;
  dfm.localsplus = {0, 1};
  dfm.cause_instr_idx = BCOffset{0};
  dm.frame_meta = {std::move(dfm)};

  Ref<> result = runInInterpreterViaReify(func, dm, dm.innermostFrame(), regs);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyLong_CheckExact(result));
  ASSERT_EQ(PyLong_AsLong(result), 30);
}

TEST_F(ReifyFrameTest, ReifyMidFunction) {
  const char* src = R"(
def test(a, b):
  return a + b
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  uint64_t regs[NUM_GP_REGS];

  auto a = Ref<>::steal(PyLong_FromLong(10));
  ASSERT_NE(a, nullptr);
  regs[ARGUMENT_REGS[0].loc] = reinterpret_cast<uint64_t>(a.get());
  LiveValue a_val{
      PhyLocation{ARGUMENT_REGS[0].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  auto b = Ref<>::steal(PyLong_FromLong(20));
  ASSERT_NE(b, nullptr);
  regs[ARGUMENT_REGS[1].loc] = reinterpret_cast<uint64_t>(b.get());
  LiveValue b_val{
      PhyLocation{ARGUMENT_REGS[1].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  DeoptMetadata dm;
  dm.live_values = {a_val, b_val};
  dm.reason = DeoptReason::kGuardFailure;
  DeoptFrameMetadata dfm;
  dfm.localsplus = {0, 1};
  dfm.stack = {0, 1};
  // Resuming at BINARY_OP +
#if PY_VERSION_HEX >= 0x030E0000
  // Skip RESUME, LOAD_FAST_BORROW_LOAD_FAST_BORROW
  dfm.cause_instr_idx = BCOffset{4};
#elif PY_VERSION_HEX >= 0x030C0000
  // Skip RESUME, LOAD_FAST, LOAD_FAST
  dfm.cause_instr_idx = BCOffset{6};
#else
  // Skip LOAD_FAST, LOAD_FAST
  dfm.cause_instr_idx = BCOffset{4};
#endif
  dm.frame_meta = {std::move(dfm)};

  Ref<> result = runInInterpreterViaReify(func, dm, dm.innermostFrame(), regs);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyLong_CheckExact(result));
  ASSERT_EQ(PyLong_AsLong(result), 30);
}

TEST_F(ReifyFrameTest, ReifyWithMemoryValues) {
  const char* src = R"(
def test(a, b):
  return a + b
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  uint64_t mem[2];
  uint64_t regs[NUM_GP_REGS];
  regs[arch::reg_frame_pointer_loc.loc] =
      reinterpret_cast<uint64_t>(mem) + sizeof(mem);

  auto a = Ref<>::steal(PyLong_FromLong(10));
  ASSERT_NE(a, nullptr);
  LiveValue a_val{
      PhyLocation{-2 * kPointerSize},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};
  mem[0] = reinterpret_cast<uint64_t>(a.get());

  auto b = Ref<>::steal(PyLong_FromLong(20));
  ASSERT_NE(b, nullptr);
  LiveValue b_val{
      PhyLocation{-kPointerSize},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};
  mem[1] = reinterpret_cast<uint64_t>(b.get());

  DeoptMetadata dm;
  dm.live_values = {a_val, b_val};
  dm.reason = DeoptReason::kGuardFailure;
  DeoptFrameMetadata dfm;
  dfm.localsplus = {0, 1};
  dfm.stack = {0, 1};
  // Resuming at BINARY_OP +
#if PY_VERSION_HEX >= 0x030E0000
  // Skip RESUME, LOAD_FAST_BORROW_LOAD_FAST_BORROW
  dfm.cause_instr_idx = BCOffset{4};
#elif PY_VERSION_HEX >= 0x030C0000
  // Skip RESUME, LOAD_FAST, LOAD_FAST
  dfm.cause_instr_idx = BCOffset{6};
#else
  // Skip LOAD_FAST, LOAD_FAST
  dfm.cause_instr_idx = BCOffset{4};
#endif
  dm.frame_meta = {std::move(dfm)};

  Ref<> result = runInInterpreterViaReify(func, dm, dm.innermostFrame(), regs);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyLong_CheckExact(result));
  ASSERT_EQ(PyLong_AsLong(result), 30);
}

TEST_F(ReifyFrameTest, ReifyInLoop) {
  const char* src = R"(
def test(num):
  fact = 1
  while num > 1:
    fact *= num
    num -= 1
  return fact
)";
  Ref<PyFunctionObject> func(compileAndGet(src, "test"));
  ASSERT_NE(func, nullptr);

  uint64_t regs[NUM_GP_REGS];
  auto num = Ref<>::steal(PyLong_FromLong(3));
  ASSERT_NE(num, nullptr);
  regs[ARGUMENT_REGS[0].loc] = reinterpret_cast<uint64_t>(num.get());
  LiveValue num_val{
      PhyLocation{ARGUMENT_REGS[0].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  auto fact = Ref<>::steal(PyLong_FromLong(20));
  ASSERT_NE(fact, nullptr);
  regs[ARGUMENT_REGS[1].loc] = reinterpret_cast<uint64_t>(fact.get());
  LiveValue fact_val{
      PhyLocation{ARGUMENT_REGS[1].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  auto tmp = Ref<>::steal(PyLong_FromLong(1));
  ASSERT_NE(tmp, nullptr);
  regs[ARGUMENT_REGS[2].loc] = reinterpret_cast<uint64_t>(tmp.get());
  LiveValue tmp_val{
      PhyLocation{ARGUMENT_REGS[2].loc},
      RefKind::kOwned,
      ValueKind::kObject,
      LiveValue::Source::kUnknown};

  DeoptMetadata dm;
  dm.live_values = {num_val, fact_val, tmp_val};
  dm.reason = DeoptReason::kGuardFailure;
  DeoptFrameMetadata dfm;
  dfm.localsplus = {0, 1};
  dfm.stack = {0, 2};
#if PY_VERSION_HEX >= 0x030C0000
  dfm.cause_instr_idx = BCOffset{10};
#else
  dfm.cause_instr_idx = BCOffset{8};
#endif
  dm.frame_meta = {std::move(dfm)};

  Ref<> result = runInInterpreterViaReify(func, dm, dm.innermostFrame(), regs);

  ASSERT_NE(result, nullptr);
  ASSERT_TRUE(PyLong_CheckExact(result));
  ASSERT_EQ(PyLong_AsLong(result), 120);
}

TEST_F(ReifyFrameTest, ReifyStaticCompareWithBool) {
  const char* src = R"(
import cinderx
from __static__ import size_t, unbox

def test(x, y):
    x1: size_t = unbox(x)
    y1: size_t = unbox(y)

    if x1 > y1:
        return True
    return False
)";
  Ref<PyFunctionObject> func(compileStaticAndGet(src, "test"));
  if (PyErr_Occurred()) {
    PyErr_Print();
  }
  ASSERT_NE(func, nullptr);

  uint64_t regs[NUM_GP_REGS];

  for (int i = 0; i < 2; i++) {
    regs[ARGUMENT_REGS[0].loc] = i;
    LiveValue a_val{
        PhyLocation{ARGUMENT_REGS[0].loc},
        RefKind::kUncounted,
        ValueKind::kBool,
        LiveValue::Source::kUnknown};

    PyCodeObject* code =
        reinterpret_cast<PyCodeObject*>(PyFunction_GetCode(func));
#if PY_VERSION_HEX <= 0x030C0000
    const int jump_index = 18;
    const int pop_instr_offset = 4;
#elif PY_VERSION_HEX < 0x030E0000
    const int jump_index = 32;
    const int pop_instr_offset = 4;
#else
    const int jump_index = 42;
    const int pop_instr_offset = 2;
#endif
    ASSERT_EQ(
        PyBytes_AS_STRING(PyCode_GetCode(code))[jump_index + pop_instr_offset],
        (char)POP_JUMP_IF_ZERO);

    DeoptMetadata dm;
    dm.live_values = {a_val};
    dm.reason = DeoptReason::kGuardFailure;
    DeoptFrameMetadata dfm;
    dfm.localsplus = {0};
    dfm.stack = {0};
    dfm.cause_instr_idx = BCOffset(jump_index);
    dm.frame_meta = {std::move(dfm)};

    Ref<> result =
        runInInterpreterViaReify(func, dm, dm.innermostFrame(), regs);

    ASSERT_NE(result, nullptr);
    ASSERT_TRUE(PyBool_Check(result));
    ASSERT_EQ(result, i ? Py_True : Py_False);
  }
}

class DeoptStressTest : public RuntimeTest {
 public:
  void runTest(
      const char* src,
      PyObject** args,
      Py_ssize_t nargs,
      PyObject* expected) {
    Ref<PyFunctionObject> funcobj(compileAndGet(src, "test"));
    ASSERT_NE(funcobj, nullptr);
    std::unique_ptr<Function> irfunc(buildHIR(funcobj));
    irfunc->reifier =
        ThreadedRef<>::create(makeFrameReifier(funcobj->func_code).get());
    auto guards = insertDeopts(*irfunc);
    jit::Compiler::runPasses(*irfunc, PassConfig::kAllExceptInliner);
    auto delete_one_deopt = [&](const DeoptMetadata& deopt_meta) {
      auto it = guards.find(deopt_meta.nonce);
      JIT_CHECK(it != guards.end(), "No guard for nonce {}", deopt_meta.nonce);
      it->second->unlink();
      delete it->second;
      guards.erase(it);
    };
    Runtime* ngen_rt = Runtime::get();
    auto pyfunc = reinterpret_cast<PyFunctionObject*>(funcobj.get());
    while (!guards.empty()) {
      NativeGenerator gen(irfunc.get());
      auto jitfunc = reinterpret_cast<vectorcallfunc>(gen.getVectorcallEntry());
      ASSERT_NE(jitfunc, nullptr);
      ngen_rt->setGuardFailureCallback(delete_one_deopt);
      auto res =
          jitfunc(reinterpret_cast<PyObject*>(pyfunc), args, nargs, nullptr);
      ngen_rt->clearGuardFailureCallback();
      if ((res == nullptr) ||
          (PyObject_RichCompareBool(res, expected, Py_EQ) < 1)) {
        dumpDebuggingOutput(*irfunc, res, expected);
        FAIL();
      }
      Py_XDECREF(res);
    }
  }

 private:
  std::unordered_map<int, Instr*> insertDeopts(Function& irfunc) {
    std::unordered_map<int, Instr*> guards;
    Register* reg = irfunc.env.AllocateRegister();
    int next_nonce{0};
    for (auto& block : irfunc.cfg.blocks) {
      bool has_periodic_tasks =
          std::any_of(block.begin(), block.end(), [](auto& instr) {
            return instr.IsRunPeriodicTasks();
          });
      if (has_periodic_tasks) {
        // skip blocks that depend on the contents of the eval breaker
        continue;
      }
      for (auto it = block.begin(); it != block.end();) {
        auto& instr = *it++;
        if (instr.getDominatingFrameState() != nullptr) {
          // Nothing defines reg, so it will be null initialized and the guard
          // will fail, thus causing deopt.
          auto guard = Guard::create(reg);
          guard->InsertBefore(instr);
          auto nonce = next_nonce++;
          guard->set_nonce(nonce);
          guards[nonce] = guard;
        }
      }
    }
    return guards;
  }

  void dumpDebuggingOutput(
      const Function& irfunc,
      PyObject* actual,
      PyObject* expected) {
    auto expected_str = Ref<>::steal(PyObject_ASCII(expected));
    ASSERT_NE(expected_str, nullptr);
    std::cerr << "Expected: " << PyUnicode_AsUTF8(expected_str) << '\n';
    std::cerr << "Actual: ";
    if (actual != nullptr) {
      auto actual_str = Ref<>::steal(PyObject_ASCII(actual));
      ASSERT_NE(actual_str, nullptr);
      std::cerr << PyUnicode_AsUTF8(actual_str) << '\n';
    } else {
      std::cerr << "nullptr";
    }
    std::cerr << '\n';
    std::cerr << "HIR of failed function:\n";
    std::cerr << HIRPrinter().ToString(irfunc) << '\n';
    std::cerr << "Disassembly:\n";
    // Recompile so we get the annotated disassembly
    bool old_dump_asm = true;
    std::swap(jit::getMutableConfig().log.dump_asm, old_dump_asm);
    NativeGenerator gen(&irfunc);
    gen.getVectorcallEntry();
    jit::getMutableConfig().log.dump_asm = old_dump_asm;
    std::cerr << '\n';
    std::cerr << "Python traceback: ";
    PyErr_Print();
    std::cerr << '\n';
  }
};

TEST_F(DeoptStressTest, BinaryOps) {
  const char* src = R"(
def test(a, b, c):
  return a + b + c
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  auto arg3 = Ref<>::steal(PyLong_FromLong(300));
  PyObject* args[] = {arg1, arg2, arg3};
  auto result = Ref<>::steal(PyLong_FromLong(600));
  runTest(src, args, 3, result);
}

TEST_F(DeoptStressTest, InPlaceOps) {
  const char* src = R"(
def test(a, b, c):
  res = 0
  res += a
  res += b
  res += c
  return res
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  auto arg3 = Ref<>::steal(PyLong_FromLong(300));
  PyObject* args[] = {arg1, arg2, arg3};
  auto result = Ref<>::steal(PyLong_FromLong(600));
  runTest(src, args, 3, result);
}

TEST_F(DeoptStressTest, BasicForLoop) {
  const char* src = R"(
def test(n):
  res = 1
  for i in range(1, n + 1):
    res *= i
  return res
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(120));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, NestedForLoops) {
  const char* src = R"(
def test():
  vals = [10, 20, 30]
  ret = 0
  for x in vals:
    for y in vals:
      for z in vals:
        ret += x + y + z
  return ret
)";
  auto result = Ref<>::steal(PyLong_FromLong(1620));
  runTest(src, nullptr, 0, result);
}

TEST_F(DeoptStressTest, NestedWhileLoops) {
  const char* src = R"(
def test():
  vals = [10, 20, 30]
  ret = 0
  x = 0
  while x < len(vals):
    y = 0
    while y < len(vals):
      z = 0
      while z < len(vals):
        ret += vals[x] + vals[y] + vals[z]
        z += 1
      y += 1
    x += 1
  return ret
)";
  auto result = Ref<>::steal(PyLong_FromLong(1620));
  runTest(src, nullptr, 0, result);
}

TEST_F(DeoptStressTest, CallInstanceMethod) {
  const char* src = R"(
class Accum:
  def __init__(self):
    self.val = 1

  def mul(self, x):
    self.val *= x

def test(n):
  acc = Accum()
  for x in range(1, n + 1):
    acc.mul(x)
  return acc.val
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(120));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallMethodDescr) {
  const char* src = R"(
def test(n):
  nums = []
  for x in range(n + 1):
    nums.append(x)
  return sum(nums)
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(15));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, NestedCallMethods) {
  const char* src = R"(
class Counter:
  def __init__(self):
    self.val = 0

  def get(self):
    val = self.val
    self.val += 1
    return val

def test(n):
  c = Counter()
  nums = []
  for x in range(n + 1):
    nums.append(c.get())
  return sum(nums)
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(15));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallClassMethod) {
  const char* src = R"(
class BinOps:
  @classmethod
  def mul(cls, x, y):
    return x * y

def test(n):
  acc = 1
  for x in range(1, n + 1):
    acc = BinOps.mul(acc, x)
  return acc
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(120));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallStaticMethod) {
  const char* src = R"(
class BinOps:
  @staticmethod
  def mul(x, y):
    return x * y

def test(n):
  acc = 1
  for x in range(1, n + 1):
    acc = BinOps.mul(acc, x)
  return acc
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(120));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallModuleMethod) {
  const char* src = R"(
import functools

def abc(y):
  return y * y
def test(n):
  acc = 1
  for x in range(1, n + 1):
    acc += functools._unwrap_partial(abc)(x)
  return acc
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(56));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallDescriptor) {
  const char* src = R"(
class Multiplier:
  def __call__(self, *args, **kwargs):
    acc = 1
    for arg in args:
      acc *= arg
    return acc

class Descr:
  def __get__(self, obj, typ):
    return Multiplier()

class Methods:
  mul = Descr()

def test(n):
  acc = 1
  m = Methods()
  for x in range(1, n + 1):
    acc = m.mul(acc, x)
  return acc
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(120));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, CallDescriptor2) {
  const char* src = R"(
class C:
  def _get_func(self):
    def f(*args):
      return args[0] + args[1]
    return f

  a_method = property(_get_func)

def test(x, y):
  c = C()
  return c.a_method(x, y)
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  PyObject* args[] = {arg1, arg2};
  auto result = Ref<>::steal(PyLong_FromLong(300));
  runTest(src, args, 2, result);
}

TEST_F(DeoptStressTest, Closures) {
  const char* src = R"(
def test(n):
  x = n
  def inc():
    x += 1
  x += 10
  return x
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(5));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(15));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, StoreSubscr) {
  const char* src = R"(
def test(x, y):
  d = {'x': 1, 'y': 2}
  d['x'] = x
  d['y'] = y
  return d['x'] + d['y']
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  PyObject* args[] = {arg1, arg2};
  auto result = Ref<>::steal(PyLong_FromLong(300));
  runTest(src, args, 2, result);
}

TEST_F(DeoptStressTest, LoadStoreAttr) {
  const char* src = R"(
class Container:
  pass

def test(x, y, z):
  c = Container()
  c.x = x
  c.y = y
  c.z = z
  return c.x + c.y + c.z
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(100));
  auto arg2 = Ref<>::steal(PyLong_FromLong(200));
  auto arg3 = Ref<>::steal(PyLong_FromLong(300));
  PyObject* args[] = {arg1, arg2, arg3};
  auto result = Ref<>::steal(PyLong_FromLong(600));
  runTest(src, args, 3, result);
}

TEST_F(DeoptStressTest, BuildSlice) {
  const char* src = R"(
def test(n):
  vals = list(range(n))
  res = 0
  x = int(n / 2)
  for x in vals[0:x]:
    res += x
  return res
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(10));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(10));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, Conditionals) {
  const char* src = R"(
def test(n):
  res = 0
  res += n
  if n > 0:
    res += n
  return res
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(10));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(20));
  runTest(src, args, 1, result);
}

TEST_F(DeoptStressTest, Inliner) {
  const char* src = R"(
def bar(n):
  return n + 1

def test(n):
  res = 0
  res += bar(n)
  return res
)";
  auto arg1 = Ref<>::steal(PyLong_FromLong(10));
  PyObject* args[] = {arg1};
  auto result = Ref<>::steal(PyLong_FromLong(11));
  getMutableConfig().hir_opts.inliner = true;
  runTest(src, args, 1, result);
}

using DeoptTest = RuntimeTest;

TEST_F(DeoptTest, ValueKind) {
  EXPECT_EQ(deoptValueKind(TCBool), ValueKind::kBool);

  EXPECT_EQ(deoptValueKind(TCInt8), ValueKind::kSigned);
  EXPECT_EQ(deoptValueKind(TCInt8 | TNullptr), ValueKind::kSigned);

  EXPECT_EQ(deoptValueKind(TCUInt32), ValueKind::kUnsigned);
  EXPECT_EQ(deoptValueKind(TCUInt32 | TNullptr), ValueKind::kUnsigned);

  EXPECT_EQ(deoptValueKind(TLong), ValueKind::kObject);
  EXPECT_EQ(deoptValueKind(TNullptr), ValueKind::kObject);
}
