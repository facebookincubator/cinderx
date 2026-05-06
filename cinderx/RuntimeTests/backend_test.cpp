// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Common/ref.h"
#include "cinderx/Jit/codegen/arch.h"
#include "cinderx/Jit/codegen/autogen.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/gen_asm.h"
#include "cinderx/Jit/codegen/tsan.h"
#include "cinderx/Jit/jit_rt.h"
#include "cinderx/Jit/lir/inliner.h"
#include "cinderx/Jit/lir/instruction.h"
#include "cinderx/Jit/lir/parser.h"
#include "cinderx/Jit/lir/postalloc.h"
#include "cinderx/Jit/lir/postgen.h"
#include "cinderx/Jit/lir/regalloc.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/module_state.h"

#include <regex>
#include <sstream>

#ifdef BUCK_BUILD
#include "tools/cxx/Resources.h"
#endif

using namespace jit;
using namespace jit::lir;

namespace jit::codegen {

class BackendTest : public RuntimeTest {
 public:
  // compile a function without generating prologue and epilogue.
  // the function is self-contained.
  // this function is used to test LIR, rewrite passes, register allocation,
  // and machine code generation.
  void* SimpleCompile(Function* lir_func, int arg_buffer_size = 0) {
    Environ environ;
    InitEnviron(environ);
    PostGenerationRewrite post_gen(lir_func, &environ);
    post_gen.run();

    LinearScanAllocator lsalloc(lir_func);
    lsalloc.run();

    environ.shadow_frames_and_spill_size = lsalloc.getFrameSize();
    environ.changed_regs = lsalloc.getChangedRegs();

    PostRegAllocRewrite post_rewrite(lir_func, &environ);
    post_rewrite.run();

    asmjit::CodeHolder code;
    ICodeAllocator* code_allocator =
        cinderx::getModuleState()->code_allocator.get();
    code.init(code_allocator->asmJitEnvironment());

    arch::Builder as(&code);

    environ.as = &as;

#if defined(CINDER_X86_64)
    as.push(asmjit::x86::rbp);
    as.mov(asmjit::x86::rbp, asmjit::x86::rsp);
#elif defined(CINDER_AARCH64)
    as.stp(arch::fp, arch::lr, asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
    as.mov(arch::fp, asmjit::a64::sp);
#else
    CINDER_UNSUPPORTED
#endif

    auto saved_regs = environ.changed_regs & CALLEE_SAVE_REGS;

#if defined(CINDER_X86_64)
    int saved_regs_size = saved_regs.count() * 8;
#elif defined(CINDER_AARCH64)
    int saved_regs_size = saved_regs.count() * 16;
#else
    CINDER_UNSUPPORTED
    int saved_regs_size = saved_regs.count() * 8;
#endif

    // Allocate stack space for the function's stack.
    // Allocate 8 bytes for the function's stack.
    // If the stack size is not a multiple of 16, add 8 bytes to the stack size.
    // This is to ensure that the stack is aligned to 16 bytes.

    int allocate_stack = std::max(environ.shadow_frames_and_spill_size, 8);
    if ((allocate_stack + saved_regs_size + arg_buffer_size) % 16 != 0) {
      allocate_stack += 8;
    }

#if defined(CINDER_X86_64)
    // Allocate stack space and save the size of the function's stack.
    as.sub(asmjit::x86::rsp, allocate_stack);

    // Push used callee-saved registers.
    std::vector<int> pushed_regs;
    pushed_regs.reserve(saved_regs.count());
    while (!saved_regs.Empty()) {
      as.push(asmjit::x86::gpq(saved_regs.GetFirst().loc));
      pushed_regs.push_back(saved_regs.GetFirst().loc);
      saved_regs.RemoveFirst();
    }

    if (arg_buffer_size > 0) {
      as.sub(asmjit::x86::rsp, arg_buffer_size);
    }

    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    gen.env_ = std::move(environ);
    gen.lir_func_.reset(lir_func);
    gen.generateAssemblyBody(code);

    if (arg_buffer_size > 0) {
      as.add(asmjit::x86::rsp, arg_buffer_size);
    }

    for (auto riter = pushed_regs.rbegin(); riter != pushed_regs.rend();
         ++riter) {
      as.pop(asmjit::x86::gpq(*riter));
    }

    as.leave();
    as.ret();
#elif defined(CINDER_AARCH64)
    // Allocate stack space and save the size of the function's stack.
    JIT_CHECK(allocate_stack % kStackAlign == 0, "unaligned");
    as.sub(asmjit::a64::sp, asmjit::a64::sp, allocate_stack);

    // Push used callee-saved registers, handling GP and FP separately.
    auto gp_regs = saved_regs & ALL_GP_REGISTERS;
    auto vecd_regs = saved_regs & ALL_VECD_REGISTERS;

    std::vector<int> pushed_gp_regs;
    pushed_gp_regs.reserve(gp_regs.count());
    while (!gp_regs.Empty()) {
      as.str(
          asmjit::a64::x(gp_regs.GetFirst().loc),
          asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
      pushed_gp_regs.push_back(gp_regs.GetFirst().loc);
      gp_regs.RemoveFirst();
    }

    std::vector<int> pushed_vecd_regs;
    pushed_vecd_regs.reserve(vecd_regs.count());
    while (!vecd_regs.Empty()) {
      as.str(
          asmjit::a64::d(vecd_regs.GetFirst().loc - VECD_REG_BASE),
          asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
      pushed_vecd_regs.push_back(vecd_regs.GetFirst().loc);
      vecd_regs.RemoveFirst();
    }

    if (arg_buffer_size > 0) {
      JIT_CHECK(arg_buffer_size % kStackAlign == 0, "unaligned");
      as.sub(asmjit::a64::sp, asmjit::a64::sp, arg_buffer_size);
    }

    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    gen.env_ = std::move(environ);
    gen.lir_func_.reset(lir_func);
    gen.generateAssemblyBody(code);

    if (arg_buffer_size > 0) {
      as.add(asmjit::a64::sp, asmjit::a64::sp, arg_buffer_size);
    }

    for (auto riter = pushed_vecd_regs.rbegin();
         riter != pushed_vecd_regs.rend();
         ++riter) {
      as.ldr(
          asmjit::a64::d(*riter - VECD_REG_BASE),
          asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    }

    for (auto riter = pushed_gp_regs.rbegin(); riter != pushed_gp_regs.rend();
         ++riter) {
      as.ldr(
          asmjit::a64::x(*riter), asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    }

    as.mov(asmjit::a64::sp, arch::fp);
    as.ldp(arch::fp, arch::lr, asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    as.ret(arch::lr);
#else
    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    CINDER_UNSUPPORTED
#endif

    as.finalize();

    AllocateResult result = code_allocator->addCode(&code);
    EXPECT_EQ(result.error, asmjit::kErrorOk);
    EXPECT_TRUE(code_allocator->contains(result.addr))
        << "Compiled function should exist within the CodeAllocator";
    gen.lir_func_.release();
    return result.addr;
  }

  void InitEnviron(Environ& environ) {
    for (const auto& loc : ARGUMENT_REGS) {
      environ.arg_locations.push_back(loc);
    }
  }

#if defined(CINDER_AARCH64)
  // Compile pre-allocated LIR (physical registers + stack slots) directly to
  // machine code, bypassing register allocation.  Used for tests that need
  // precise control over which registers and stack slots are used.
  void* CompilePreAllocated(Function* lir_func, int spill_size) {
    Environ environ;
    InitEnviron(environ);

    // Skip PostGenerationRewrite and LinearScanAllocator — the instructions
    // are already in post-alloc form with physical registers and stack slots.
    environ.shadow_frames_and_spill_size = spill_size;
    environ.changed_regs = {};

    PostRegAllocRewrite post_rewrite(lir_func, &environ);
    post_rewrite.run();

    asmjit::CodeHolder code;
    ICodeAllocator* code_allocator =
        cinderx::getModuleState()->code_allocator.get();
    code.init(code_allocator->asmJitEnvironment());

    arch::Builder as(&code);
    environ.as = &as;

    // Prologue: save frame pointer and link register, set up frame.
    as.stp(arch::fp, arch::lr, asmjit::a64::ptr_pre(asmjit::a64::sp, -16));
    as.mov(arch::fp, asmjit::a64::sp);

    // Allocate stack space for spill slots.
    int allocate_stack = spill_size;
    if (allocate_stack % kStackAlign != 0) {
      allocate_stack += kStackAlign - (allocate_stack % kStackAlign);
    }
    as.sub(asmjit::a64::sp, asmjit::a64::sp, allocate_stack);

    NativeGeneratorFactory factory;
    NativeGenerator gen(nullptr, factory);
    gen.env_ = std::move(environ);
    gen.lir_func_.reset(lir_func);
    gen.generateAssemblyBody(code);

    // Epilogue: restore stack and frame pointer, return.
    as.mov(asmjit::a64::sp, arch::fp);
    as.ldp(arch::fp, arch::lr, asmjit::a64::ptr_post(asmjit::a64::sp, 16));
    as.ret(arch::lr);

    as.finalize();

    AllocateResult result = code_allocator->addCode(&code);
    EXPECT_EQ(result.error, asmjit::kErrorOk);
    gen.lir_func_.release();
    return result.addr;
  }
#endif

  void CheckCast(Function* lir_func) {
    auto func =
        (PyObject * (*)(PyObject*, PyTypeObject*)) SimpleCompile(lir_func);

    auto test_noerror = [&](PyObject* a_in, PyTypeObject* b_in) -> void {
      auto ret_test = func(a_in, b_in);
      ASSERT_TRUE(PyErr_Occurred() == nullptr);
      auto ret_jitrt = JITRT_Cast(a_in, b_in);
      ASSERT_TRUE(PyErr_Occurred() == nullptr);
      ASSERT_EQ(ret_test, ret_jitrt);
    };

    auto test_error = [&](PyObject* a_in, PyTypeObject* b_in) -> void {
      auto ret_test = func(a_in, b_in);
      ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
      PyErr_Clear();

      auto ret_jitrt = JITRT_Cast(a_in, b_in);
      ASSERT_TRUE(PyErr_ExceptionMatches(PyExc_TypeError));
      PyErr_Clear();

      ASSERT_EQ(ret_test, ret_jitrt);
    };

    test_noerror(Py_False, &PyBool_Type);
    test_noerror(Py_False, &PyLong_Type);
    test_error(Py_False, &PyUnicode_Type);
  }
};

// This is a test harness for experimenting with backends
TEST_F(BackendTest, SimpleLoadAttr) {
  const char* src = R"(
class User:
  def __init__(self, user_id):
    self._user_id = user_id

def get_user_id(user):
    return user._user_id
)";
  Ref<PyObject> globals(MakeGlobals());
  ASSERT_NE(globals.get(), nullptr) << "Failed creating globals";

  auto locals = Ref<>::steal(PyDict_New());
  ASSERT_NE(locals.get(), nullptr) << "Failed creating locals";

  auto st = Ref<>::steal(PyRun_String(src, Py_file_input, globals, locals));
  ASSERT_NE(st.get(), nullptr) << "Failed executing code";

  // Borrowed from locals
  PyObject* get_user_id = PyDict_GetItemString(locals, "get_user_id");
  ASSERT_NE(get_user_id, nullptr) << "Couldn't get get_user_id function";

  // Borrowed from get_user_id
  // code holds the code object for the function
  // code->co_consts holds the constants referenced by LoadConst
  // code->co_names holds the names referenced by LoadAttr
  PyObject* code = PyFunction_GetCode(get_user_id);
  ASSERT_NE(code, nullptr) << "Couldn't get code for user_id";

  // At this point you could patch user_id->vectorcall with a pointer to
  // your generated code for get_user_id.
  //
  // The HIR should be:
  //
  // fun get_user_id {
  //   bb 0 {
  //     CheckVar a0
  //     t0 = LoadAttr a0 0
  //     CheckExc t0
  //     Incref t0
  //     Return t0
  //   }
  // }

  // Create a user object we can use to call our function
  PyObject* user_klass = PyDict_GetItemString(locals, "User");
  ASSERT_NE(user_klass, nullptr) << "Couldn't get class User";

  auto user_id = Ref<>::steal(PyLong_FromLong(12345));
  ASSERT_NE(user_id.get(), nullptr) << "Couldn't create user id";

  auto user = Ref<>::steal(
      PyObject_CallFunctionObjArgs(user_klass, user_id.get(), nullptr));
  ASSERT_NE(user.get(), nullptr) << "Couldn't create user";

  // Finally, call get_user_id
  auto result = Ref<>::steal(
      PyObject_CallFunctionObjArgs(get_user_id, user.get(), nullptr));
  ASSERT_NE(result.get(), nullptr) << "Failed getting user id";
  ASSERT_TRUE(PyLong_CheckExact(result)) << "Incorrect type returned";
  ASSERT_EQ(PyLong_AsLong(result), PyLong_AsLong(user_id))
      << "Incorrect user id returned";
}

TEST_F(BackendTest, CallCountTest) {
  const char* src = R"(
def foo(x: int) -> int:
  return x + 1

for i in range(30):
  foo(i)
)";

  Ref<> foo = compileAndGet(src, "foo");
  ASSERT_TRUE(PyFunction_Check(foo));

  BorrowedRef<PyCodeObject> code =
      reinterpret_cast<PyFunctionObject*>(foo.get())->func_code;

  auto extra = codeExtra(code);
  ASSERT_NE(extra, nullptr) << "Failed to load code object extra data";
  uint64_t ncalls = Ci_code_extra_get_calls(extra);

  // TASK(T190615535): This is waiting on the 3.12 custom interpreter loop.
  // Once we have that in place, we can start incrementing call counts in 3.12.
  ASSERT_EQ(ncalls, 30);
}

// floating-point arithmetic test
TEST_F(BackendTest, FPArithmetic) {
  double a = 3.12;
  double b = 1.1616;

  auto test = [&](Instruction::Opcode opcode) -> double {
    auto lirfunc = std::make_unique<Function>();
    auto bb = lirfunc->allocateBasicBlock();

    auto pa = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&a)));
    auto fa = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pa));

    auto pb = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&b)));
    auto fb = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pb));

    auto sum = bb->allocateInstr(
        opcode, nullptr, OutVReg(OperandBase::kDouble), VReg(fa), VReg(fb));
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_double_return_loc, OperandBase::kDouble},
        VReg(sum));
    bb->allocateInstr(Instruction::kReturn, nullptr);

    // need this because the register allocator assumes the basic blocks
    // end with Return should have one and only one successor.
    auto epilogue = lirfunc->allocateBasicBlock();
    bb->addSuccessor(epilogue);

    auto func = (double (*)())SimpleCompile(lirfunc.get());

    return func();
  };

  ASSERT_DOUBLE_EQ(test(Instruction::kFadd), a + b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFsub), a - b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFmul), a * b);
  ASSERT_DOUBLE_EQ(test(Instruction::kFdiv), a / b);
}

TEST_F(BackendTest, FPCompare) {
  double a = 3.12;
  double b = 1.1616;

  auto test = [&](Instruction::Opcode opcode) -> double {
    auto lirfunc = std::make_unique<Function>();
    auto bb = lirfunc->allocateBasicBlock();

    auto pa = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&a)));
    auto fa = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pa));

    auto pb = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(&b)));
    auto fb = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(pb));

    auto compare =
        bb->allocateInstr(opcode, nullptr, OutVReg(), VReg(fa), VReg(fb));
    bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutPhyReg{arch::reg_general_return_loc},
        VReg(compare));
    bb->allocateInstr(Instruction::kReturn, nullptr);

    // need this because the register allocator assumes the basic blocks
    // end with Return should have one and only one successor.
    auto epilogue = lirfunc->allocateBasicBlock();
    bb->addSuccessor(epilogue);

    auto func = (bool (*)())SimpleCompile(lirfunc.get());

    return func();
  };

  ASSERT_DOUBLE_EQ(test(Instruction::kEqual), a == b);
  ASSERT_DOUBLE_EQ(test(Instruction::kNotEqual), a != b);
  ASSERT_DOUBLE_EQ(test(Instruction::kGreaterThanUnsigned), a > b);
  ASSERT_DOUBLE_EQ(test(Instruction::kLessThanUnsigned), a < b);
  ASSERT_DOUBLE_EQ(test(Instruction::kGreaterThanEqualUnsigned), a >= b);
  ASSERT_DOUBLE_EQ(test(Instruction::kLessThanEqualUnsigned), a <= b);
}

namespace {
double rt_func(
    int a,
    int b,
    int c,
    int d,
    int e,
    double fa,
    double fb,
    double fc,
    double fd,
    double fe,
    double ff,
    double fg,
    double fh,
    double fi,
    int f,
    int g,
    int h,
    double fj) {
  return fj + a + b + c + d + e + fa * fb * fc * fd * fe * ff * fg * fh * fi +
      f + g + h;
}

template <typename... Arg>
struct AllocateOperand;

template <typename Arg, typename... Args>
struct AllocateOperand<Arg, Args...> {
  Instruction* instr;
  explicit AllocateOperand(Instruction* i) : instr(i) {}

  void operator()(Arg arg, Args... args) {
    if constexpr (std::is_same_v<int, Arg>) {
      instr->allocateImmediateInput(arg);
    } else {
      instr->allocateFPImmediateInput(arg);
    }

    (AllocateOperand<Args...>(instr))(args...);
  }
};

template <>
struct AllocateOperand<> {
  Instruction* instr;
  explicit AllocateOperand(Instruction* i) : instr(i) {}

  void operator()() {}
};

template <typename... Ts>
auto getAllocateOperand(Instruction* instr, std::tuple<Ts...>) {
  return AllocateOperand<Ts...>(instr);
}
} // namespace

TEST_F(BackendTest, ManyArguments) {
  auto args = std::make_tuple(
      1,
      2,
      3,
      4,
      5,
      1.1,
      2.2,
      3.3,
      4.4,
      5.5,
      6.6,
      7.7,
      8.8,
      9.9,
      6,
      7,
      8,
      10.1);

  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  Instruction* call = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(rt_func)));

  std::apply(getAllocateOperand(call, args), args);

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  // need this because the register allocator assumes the basic blocks
  // end with Return should have one and only one successor.
  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  constexpr int kArgBufferSize = 32; // 4 arguments need to pass by stack
  auto func = (double (*)())SimpleCompile(lirfunc.get(), kArgBufferSize);

  double expected = std::apply(rt_func, args);
  double result = func();

  ASSERT_DOUBLE_EQ(result, expected);
}

namespace {
static double add(double a, double b) {
  return a + b;
}
} // namespace

TEST_F(BackendTest, FPMultipleCalls) {
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  double a = 1.1;
  double b = 2.2;
  double c = 3.3;
  double d = 4.4;

  auto loadFP = [&](double* n) {
    auto m1 = bb->allocateInstr(
        Instruction::kMove,
        nullptr,
        OutVReg(),
        Imm(reinterpret_cast<uint64_t>(n)));
    auto m2 = bb->allocateInstr(
        Instruction::kMove, nullptr, OutVReg(OperandBase::kDouble), Ind(m1));
    return m2;
  };

  auto la = loadFP(&a);
  auto lb = loadFP(&b);
  auto sum1 = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(la),
      VReg(lb));

  auto lc = loadFP(&c);
  auto ld = loadFP(&d);
  auto sum2 = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(lc),
      VReg(ld));

  auto sum = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(OperandBase::kDouble),
      Imm(reinterpret_cast<uint64_t>(add)),
      VReg(sum1),
      VReg(sum2));

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_double_return_loc, OperandBase::kDouble},
      VReg(sum));
  bb->allocateInstr(Instruction::kReturn, nullptr);

  auto epilogue = lirfunc->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  auto func = (double (*)())SimpleCompile(lirfunc.get());
  double result = func();

  ASSERT_DOUBLE_EQ(result, a + b + c + d);
}

TEST_F(BackendTest, MoveSequenceOptTest) {
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk(-16),
      PhyReg(arch::reg_scratch_0_loc));
  bb->allocateInstr(
      Instruction::kMove, nullptr, OutStk(-24), PhyReg(ARGUMENT_REGS[1].loc));
  bb->allocateInstr(
      lir::Instruction::kMove,
      nullptr,
      OutStk(-32),
      PhyReg(ARGUMENT_REGS[3].loc));

  auto call = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm(0),
      lir::Stk(-16),
      lir::Stk(-24),
      lir::Stk(-32));
  call->getInput(3)->setLastUse();

  Environ env;
  PostRegAllocRewrite post_rewrite(lirfunc.get(), &env);
  post_rewrite.run();

  /*
  BB %0
  [RBP - 16]:Object = Move RAX:Object
  [RBP - 24]:Object = Move RSI:Object
        RDI:Object = Move RAX:Object
        RDX:Object = Move RCX:Object
                     Call Object
  */
  ASSERT_EQ(bb->getNumInstrs(), 5);
  auto& instrs = bb->instructions();

  auto iter = instrs.begin();

  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*(iter++))->opcode(), Instruction::kCall);
}

TEST_F(BackendTest, MoveSequenceOpt2Test) {
  // OptimizeMoveSequence should not set reg operands that are also output
  auto lirfunc = std::make_unique<Function>();
  auto bb = lirfunc->allocateBasicBlock();

  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutStk(-16),
      PhyReg(arch::reg_general_return_loc));

  bb->allocateInstr(
      Instruction::kAdd,
      nullptr,
      OutPhyReg(arch::reg_general_return_loc),
      PhyReg(ARGUMENT_REGS[1].loc),
      lir::Stk(-16));

  Environ env;
  PostRegAllocRewrite post_rewrite(lirfunc.get(), &env);
  post_rewrite.run();

  /*
  BB %0
  [RBP - 16]:Object = Move RAX:Object
        RAX:Object = Add RSI:Object, [RBP - 16]:Object
  */
  ASSERT_EQ(bb->getNumInstrs(), 2);
  auto& instrs = bb->instructions();

  auto iter = instrs.begin();

  ASSERT_EQ((*(iter++))->opcode(), Instruction::kMove);
  ASSERT_EQ((*iter)->opcode(), Instruction::kAdd);
  ASSERT_EQ((*iter)->getInput(1)->type(), OperandBase::kStack);
}

TEST_F(BackendTest, CastTest) {
  // constants used to print out error
  static const char* errmsg = "expected '%s', got '%s'";

  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();
  auto bb3 = lirfunc->allocateBasicBlock();
  auto bb4 = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  // BB 1 : Py_TYPE(ob) == (tp)
  auto a =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto b =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));

  auto a_tp = bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(a, offsetof(PyObject, ob_type)));
  auto eq1 = bb1->allocateInstr(
      Instruction::kEqual, nullptr, OutVReg(), VReg(a_tp), VReg(b));
  bb1->allocateInstr(Instruction::kCondBranch, nullptr, VReg(eq1));
  bb1->addSuccessor(bb3); // true
  bb1->addSuccessor(bb2); // false

  // BB2 : PyType_IsSubtype(Py_TYPE(ob), (tp))
  auto subtype = bb2->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(PyType_IsSubtype)),
      VReg(a_tp),
      VReg(b));
  bb2->allocateInstr(Instruction::kCondBranch, nullptr, VReg(subtype));
  bb2->addSuccessor(bb3); // true
  bb2->addSuccessor(bb4); // false

  // BB3 : return object
  bb3->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(a));
  bb3->allocateInstr(Instruction::kReturn, nullptr);
  bb3->addSuccessor(epilogue);

  // BB4 : return null
  auto a_name = bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(a_tp, offsetof(PyTypeObject, tp_name)));
  auto b_name = bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(),
      Ind(b, offsetof(PyTypeObject, tp_name)));
  bb4->allocateInstr(
      Instruction::kCall,
      nullptr,
      Imm(reinterpret_cast<uint64_t>(PyErr_Format)),
      Imm(reinterpret_cast<uint64_t>(PyExc_TypeError)),
      Imm(reinterpret_cast<uint64_t>(errmsg)),
      VReg(b_name),
      VReg(a_name));
  auto nll = bb4->allocateInstr(Instruction::kMove, nullptr, OutVReg(), Imm(0));
  bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(nll));
  bb4->allocateInstr(Instruction::kReturn, nullptr);
  bb4->addSuccessor(epilogue);

  CheckCast(lirfunc.get());
}

TEST_F(BackendTest, ParserStringInputTest) {
  auto lir_str = fmt::format(R"(Function:
BB %0 - succs: %4
        %1:Object = Move "hello"
        Return %1:Object

BB %4 - preds: %0

)");
  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  auto func = (char* (*)())SimpleCompile(parsed_func.get());
  std::string ret = func();
  ASSERT_EQ(ret, "hello");
}

TEST_F(BackendTest, ParserMultipleStringInputTest) {
  auto lir_str = fmt::format(R"(Function:
BB %0 - succs: %8
        %1:Object = Move "hello1"
        %2:Object = Move "hello2"
        %3:Object = Move "hello3"
        %4:Object = Move "hello4"
        %5:Object = Move "hello5"
        %6:Object = Move "hello6"
                    Return %1:Object

BB %8 - preds: %0

)");
  Parser parser;
  auto parsed_func = parser.parse(lir_str);
  auto func = (char* (*)())SimpleCompile(parsed_func.get());
  std::string ret = func();
  ASSERT_EQ(ret, "hello1");
}

#if CINDER_JIT_TSAN_ENABLED

TEST_F(BackendTest, TsanMovePreservesBehaviorAndFlags) {
  // Pseudo-code:
  //   lhs = 1
  //   rhs = 1
  //   cmp(1, 1)
  //   loaded = *src    // TSAN read instrumentation
  //   dst_addr = &dst
  //   *dst_addr = loaded    // TSAN write instrumentation
  //   return zero_flag_is_set ? loaded : 0
  //
  // kMove has FlagEffects::kNone, so TSAN instrumentation must preserve the
  // flags from cmp until BranchZ.
  constexpr uint64_t expected = 0x1122334455667788ULL;
  uint64_t src = expected;
  uint64_t dst = 0;

  auto lirfunc = std::make_unique<Function>();
  auto bb0 = lirfunc->allocateBasicBlock();
  auto bb_taken = lirfunc->allocateBasicBlock();
  auto bb_not_taken = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  auto lhs = bb0->allocateInstr(Instruction::kMove, nullptr, OutVReg(), Imm{1});
  auto rhs = bb0->allocateInstr(Instruction::kMove, nullptr, OutVReg(), Imm{1});
  bb0->allocateInstr(Instruction::kCmp, nullptr, VReg(lhs), VReg(rhs));

  auto loaded = bb0->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutVReg(OperandBase::k64bit),
      MemImm{&src, OperandBase::k64bit});
  bb0->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{R10},
      Imm{reinterpret_cast<uint64_t>(&dst)});
  bb0->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutInd{R10, 0, OperandBase::k64bit},
      VReg{loaded});
  bb0->allocateInstr(Instruction::kBranchZ, nullptr, Lbl{bb_taken});
  bb0->addSuccessor(bb_taken);
  bb0->addSuccessor(bb_not_taken);

  bb_taken->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg{loaded});
  bb_taken->allocateInstr(Instruction::kReturn, nullptr);
  bb_taken->addSuccessor(epilogue);

  bb_not_taken->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      Imm{0});
  bb_not_taken->allocateInstr(Instruction::kReturn, nullptr);
  bb_not_taken->addSuccessor(epilogue);

  auto func = reinterpret_cast<uint64_t (*)()>(SimpleCompile(lirfunc.get()));
  EXPECT_EQ(expected, func());
  EXPECT_EQ(expected, dst);
}

TEST_F(BackendTest, TsanMoveRelaxedUsesAtomicAccesses) {
  // Pseudo-code:
  //   value = 0
  //   rdi = expected
  //   atomic_relaxed_store(&value, rdi)
  //   byte = atomic_relaxed_load(&byte_src)
  //   atomic_relaxed_store((uint8_t*)&byte_dst, byte)
  //   return atomic_relaxed_load(&value)
  //
  // kMoveRelaxed TSAN helpers replace the memory access, so the original mov
  // must not run a second load/store.
  constexpr uint64_t expected = 0x8877665544332211ULL;
  uint64_t value = 0;
  uint8_t byte_src = 0x07;
  uint16_t byte_dst = 0xAA00;

  auto lirfunc = std::make_unique<Function>();
  auto bb0 = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  // RDI is also TSAN's address argument; the store must still use its value.
  bb0->allocateInstr(
      Instruction::kMove, nullptr, OutPhyReg{RDI}, Imm{expected});
  bb0->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutMemImm{&value, OperandBase::k64bit},
      PhyReg{RDI});

  auto byte = bb0->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutVReg(OperandBase::k8bit),
      MemImm{&byte_src, OperandBase::k8bit});
  bb0->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutMemImm{&byte_dst, OperandBase::k8bit},
      VReg(byte));

  auto word = bb0->allocateInstr(
      Instruction::kMoveRelaxed,
      nullptr,
      OutVReg(OperandBase::k64bit),
      MemImm{&value, OperandBase::k64bit});
  bb0->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg{word});
  bb0->allocateInstr(Instruction::kReturn, nullptr);
  bb0->addSuccessor(epilogue);

  auto func = reinterpret_cast<uint64_t (*)()>(SimpleCompile(lirfunc.get()));
  EXPECT_EQ(func(), expected);
  EXPECT_EQ(value, expected);
  EXPECT_EQ(byte_dst, 0xAA00 | 0x0007);
}

#endif // CINDER_JIT_TSAN_ENABLED

TEST_F(BackendTest, SplitBasicBlockTest) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();
  auto bb3 = lirfunc->allocateBasicBlock();
  auto bb4 = lirfunc->allocateBasicBlock();
  auto epilogue = lirfunc->allocateBasicBlock();

  auto r1 =
      bb1->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  bb1->allocateInstr(Instruction::kCondBranch, nullptr, VReg(r1));
  bb1->addSuccessor(bb2);
  bb1->addSuccessor(bb3);

  auto r2 = bb2->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r1), Imm(8));
  bb2->addSuccessor(bb4);

  auto r3 = bb3->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r1), Imm(8));
  auto r4 = bb3->allocateInstr(
      Instruction::kAdd, nullptr, OutVReg(), VReg(r3), Imm(8));
  bb3->addSuccessor(bb4);

  auto r5 = bb4->allocateInstr(
      Instruction::kPhi,
      nullptr,
      OutVReg(),
      Lbl(bb2),
      VReg(r2),
      Lbl(bb3),
      VReg(r4));
  bb4->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(r5));
  bb4->allocateInstr(Instruction::kReturn, nullptr);
  bb4->addSuccessor(epilogue);

  // split blocks and then test that function output is still correct
  auto bb_new = bb1->splitBefore(r1);
  bb_new->splitBefore(r1); // test that bb_new is valid
  bb2->splitBefore(r2); // test fixupPhis
  auto bb_nullptr = bb2->splitBefore(r3); // test instruction not in block
  ASSERT_EQ(bb_nullptr, nullptr);
  bb3->splitBefore(r4); // test split in middle of block

  auto func = (uint64_t (*)(int64_t))SimpleCompile(lirfunc.get());

  ASSERT_EQ(func(0), 16);
  ASSERT_EQ(func(1), 9);
}

TEST_F(BackendTest, InlineJITRTCastTest) {
  Function caller;
  auto bb = caller.allocateBasicBlock();
  auto r1 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto r2 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));
  auto call_instr = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(JITRT_Cast)),
      VReg(r1),
      VReg(r2));
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call_instr));
  bb->allocateInstr(Instruction::kReturn, nullptr);
  auto epilogue = caller.allocateBasicBlock();
  bb->addSuccessor(epilogue);
  LIRInliner inliner{&caller, call_instr};
  inliner.inlineCall();

  // Check that caller LIR is as expected.
  auto expected_caller = fmt::format(
      R"(Function:
BB %0 - succs: %8
       %1:Object = LoadArg 0(0x0):64bit
       %2:Object = LoadArg 1(0x1):64bit

BB %8 - preds: %0 - succs: %10 %9
      %15:Object = Move [%1:Object + 0x8]:Object
      %16:Object = Equal %15:Object, %2:Object
                   CondBranch %16:Object

BB %9 - preds: %8 - succs: %10 %11
      %18:Object = Call {0}({0:#x}):Object, %15:Object, %2:Object
                   CondBranch %18:Object

BB %11 - preds: %9 - succs: %12
      %22:Object = Move [%15:Object + 0x18]:Object
      %23:Object = Move [%2:Object + 0x18]:Object
                   Call {1}({1:#x}):Object, {2}({2:#x}):Object, string_literal, %23:Object, %22:Object
      %25:Object = Move 0(0x0):Object

BB %10 - preds: %8 %9 - succs: %12

BB %12 - preds: %10 %11 - succs: %7
      %28:Object = Phi (BB%10, %1:Object), (BB%11, %25:Object)

BB %7 - preds: %12 - succs: %6
       %3:Object = Move %28:Object
{3:>16} = Move %3:Object
                   Return

BB %6 - preds: %7

)",
      reinterpret_cast<uint64_t>(PyType_IsSubtype),
      reinterpret_cast<uint64_t>(PyErr_Format),
      reinterpret_cast<uint64_t>(PyExc_TypeError),
      fmt::format("{}:Object", arch::reg_general_return_loc.toString()));
  std::stringstream ss;
  caller.sortBasicBlocks();
  ss << caller;
  // Replace the string literal address
  std::regex reg(R"(\d+\(0x[0-9a-fA-F]+\):Object, %23:Object, %22:Object)");
  std::string caller_str =
      regex_replace(ss.str(), reg, "string_literal, %23:Object, %22:Object");
  ASSERT_EQ(expected_caller, caller_str);

  // Test execution of caller
  CheckCast(&caller);
}

TEST_F(BackendTest, PostgenJITRTCastTest) {
  auto caller = std::make_unique<Function>();
  auto bb = caller->allocateBasicBlock();
  auto r1 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(0));
  auto r2 =
      bb->allocateInstr(Instruction::kLoadArg, nullptr, OutVReg(), Imm(1));
  auto call_instr = bb->allocateInstr(
      Instruction::kCall,
      nullptr,
      OutVReg(),
      Imm(reinterpret_cast<uint64_t>(JITRT_Cast)),
      VReg(r1),
      VReg(r2));
  bb->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc},
      VReg(call_instr));
  bb->allocateInstr(Instruction::kReturn, nullptr);
  auto epilogue = caller->allocateBasicBlock();
  bb->addSuccessor(epilogue);

  Environ environ;
  InitEnviron(environ);
  PostGenerationRewrite post_gen(caller.get(), &environ);
  post_gen.run();

  // Check that caller LIR is as expected.
  auto expected_caller = fmt::format(
      R"(Function:
BB %0 - succs: %8
       %1:Object = Bind {0}:Object
       %2:Object = Bind {1}:Object

BB %8 - preds: %0 - succs: %10 %9
      %15:Object = Move [%1:Object + 0x8]:Object
      %16:Object = Equal %15:Object, %2:Object
                   CondBranch %16:Object

BB %9 - preds: %8 - succs: %10 %11
)"
      R"(      %18:Object = Call {2}({2:#x}):Object, %15:Object, %2:Object
)"
      R"(                   CondBranch %18:Object

BB %11 - preds: %9 - succs: %12
      %22:Object = Move [%15:Object + 0x18]:Object
      %23:Object = Move [%2:Object + 0x18]:Object
)"
      R"(                   Call {3}({3:#x}):Object, {4}({4:#x}):Object, string_literal, %23:Object, %22:Object
)"
      R"(      %25:Object = Move 0(0x0):Object

BB %10 - preds: %8 %9 - succs: %12

BB %12 - preds: %10 %11 - succs: %7
      %28:Object = Phi (BB%10, %1:Object), (BB%11, %25:Object)

BB %7 - preds: %12 - succs: %6
       %3:Object = Move %28:Object
{5:>16} = Move %3:Object
                   Return

BB %6 - preds: %7

)",
      ARGUMENT_REGS[0],
      ARGUMENT_REGS[1],
      reinterpret_cast<uint64_t>(PyType_IsSubtype),
      reinterpret_cast<uint64_t>(PyErr_Format),
      reinterpret_cast<uint64_t>(PyExc_TypeError),
      fmt::format("{}:Object", arch::reg_general_return_loc.toString()));
  std::stringstream ss;
  caller->sortBasicBlocks();
  ss << *caller;
  // Replace the string literal address
  std::regex reg(R"(\d+\(0x[0-9a-fA-F]+\):Object, %23:Object, %22:Object)");
  std::string caller_str =
      regex_replace(ss.str(), reg, "string_literal, %23:Object, %22:Object");
  ASSERT_EQ(expected_caller, caller_str);
}

TEST_F(BackendTest, ParserErrorFromExpectTest) {
  // Test throw from expect
  Parser parser;
  parser.parse(R"(Function:
BB %0
)");
  try {
    // Bad basic block header
    parser.parse(R"(Function:
BB %0 %3
)");
    FAIL();
  } catch (ParserException&) {
  }

  try {
    // Dupicate ID
    parser.parse(R"(Function:
BB %0
%1:Object = Bind RDI:Object
%1:Object
)");
    FAIL();
  } catch (ParserException&) {
  }
}

TEST_F(BackendTest, ParserErrorFromMapGetTest) {
  // Test throw from map_get_throw
  Parser parser;
  try {
    // Invalid opcode
    parser.parse(R"(Function:
BB %0
%1:Object = InvalidInstruction
)");
    FAIL();
  } catch (ParserException&) {
  }
  try {
    // Missing basic block
    parser.parse(R"(Function:
BB %0 - succs: %2
Return 0(0x0):Object
BB %1
)");
    FAIL();
  } catch (ParserException&) {
  }
}

#if defined(CINDER_AARCH64)
// This test uses CompilePreAllocated to construct the exact instruction
// sequence the buggy register allocator would emit:
//   1. Store a 64-bit pointer in X19 and a 32-bit flag in X21
//   2. Swap X19↔X21 using X13 as temp with kObject-width moves
//   3. Return X21 (which should hold the original 64-bit pointer)
//
// With the fix (k64bit moves): the pointer is preserved in full.
// Without the fix (k32bit moves): upper 32 bits are zeroed.
TEST_F(BackendTest, RegSwapPreserves64BitPointers) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();

  // Use caller-saved registers that don't clash with argument registers
  // or the scratch register (X13). X9 and X10 are available.
  constexpr auto kReg_A = X9;
  constexpr auto kReg_B = X10;

  // BB1: Move arg0 (64-bit pointer) to X19, arg1 (32-bit flag) to X21.
  // Then perform a 3-register swap X19↔X21 using X13 (scratch) as temp,
  // emitting with kObject width — this is what the FIXED regalloc emits.
  // (The buggy version would use k32bit for the second edge, truncating.)
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{ARGUMENT_REGS[0], DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k32bit});

  // Swap X19↔X21 via X13, using k64bit (the fix) — preserves all 64 bits.
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_scratch_0_loc, DataType::kObject},
      PhyReg{kReg_A, DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::kObject},
      PhyReg{arch::reg_scratch_0_loc, DataType::kObject});

  bb1->allocateInstr(Instruction::kBranch, nullptr, Lbl{bb2});
  bb1->addSuccessor(bb2);

  // BB2: Return X21 (should hold the original 64-bit pointer from X19).
  bb2->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});

  auto func = (uint64_t (*)(uint64_t, uint64_t))CompilePreAllocated(
      lirfunc.release(), 16);
  ASSERT_NE(func, nullptr);

  // The pointer has non-zero upper 32 bits.
  // If the swap truncated it, upper bits would be zero.
  constexpr uint64_t kPtr = 0xDEADBEEFCAFEBABEULL;
  constexpr uint64_t kFlag = 42;
  uint64_t result = func(kPtr, kFlag);
  EXPECT_EQ(result, kPtr) << "Register swap truncated 64-bit pointer: got 0x"
                          << std::hex << result << ", expected 0x" << kPtr;

  // Also verify the upper 32 bits survived.
  EXPECT_EQ(result & 0xFFFFFFFF00000000ULL, 0xDEADBEEF00000000ULL)
      << "Upper 32 bits of pointer destroyed during swap: got 0x" << std::hex
      << result;
}

// Negative test: verify that k32bit swap moves DO truncate 64-bit values.
// This confirms the bug pattern — if this test ever passes, the k32bit
// codegen changed and the fix in rewriteLIREmitCopies may need revisiting.
TEST_F(BackendTest, RegSwapK32bitTruncates64BitValues) {
  auto lirfunc = std::make_unique<Function>();
  auto bb1 = lirfunc->allocateBasicBlock();
  auto bb2 = lirfunc->allocateBasicBlock();

  constexpr auto kReg_A = X9;
  constexpr auto kReg_B = X10;

  // Same setup as RegSwapPreserves64BitPointers, but swap uses k32bit
  // (the buggy data type that the unfixed regalloc would emit).
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::kObject},
      PhyReg{ARGUMENT_REGS[0], DataType::kObject});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{ARGUMENT_REGS[1], DataType::k32bit});

  // Swap using k32bit — this SHOULD truncate the 64-bit pointer.
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_scratch_0_loc, DataType::k32bit},
      PhyReg{kReg_A, DataType::k32bit});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_A, DataType::k32bit},
      PhyReg{kReg_B, DataType::k32bit});
  bb1->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{kReg_B, DataType::k32bit},
      PhyReg{arch::reg_scratch_0_loc, DataType::k32bit});

  bb1->allocateInstr(Instruction::kBranch, nullptr, Lbl{bb2});
  bb1->addSuccessor(bb2);

  bb2->allocateInstr(
      Instruction::kMove,
      nullptr,
      OutPhyReg{arch::reg_general_return_loc, DataType::kObject},
      PhyReg{kReg_B, DataType::kObject});

  auto func = (uint64_t (*)(uint64_t, uint64_t))CompilePreAllocated(
      lirfunc.release(), 16);
  ASSERT_NE(func, nullptr);

  constexpr uint64_t kPtr = 0xDEADBEEFCAFEBABEULL;
  uint64_t result = func(kPtr, 42);

  // The k32bit swap SHOULD truncate — upper 32 bits should be zero.
  // This confirms the bug pattern exists in the codegen layer.
  EXPECT_NE(result, kPtr)
      << "k32bit swap should NOT preserve full 64-bit value";
  EXPECT_EQ(result & 0xFFFFFFFF00000000ULL, 0ULL)
      << "k32bit swap should zero upper 32 bits, got 0x" << std::hex << result;
}

#endif // CINDER_AARCH64

} // namespace jit::codegen
