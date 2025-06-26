// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <Python.h>

#include "cinderx/Common/util.h"
#include "cinderx/Interpreter/opcode.h"
#include "cinderx/Jit/bitvector.h"
#include "cinderx/Jit/codegen/environ.h"
#include "cinderx/Jit/codegen/frame_asm.h"
#include "cinderx/Jit/codegen/register_preserver.h"
#include "cinderx/Jit/codegen/x86_64.h"
#include "cinderx/Jit/hir/hir.h"
#include "cinderx/Jit/lir/function.h"

#include <asmjit/asmjit.h>

#include <algorithm>
#include <cstddef>
#include <list>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Use a special define to keep it clear why much code changes in 3.12+
#if PY_VERSION_HEX < 0x030C0000
#define SHADOW_FRAMES 1
#endif

namespace jit::codegen {

class NativeGenerator {
 public:
  explicit NativeGenerator(const hir::Function* func);

  NativeGenerator(
      const hir::Function* func,
      void* deopt_trampoline,
      void* deopt_trampoline_generators,
      void* failed_deferred_compile_trampoline);

  void SetJSONOutput(nlohmann::json* json_2) {
    JIT_CHECK(json_2 != nullptr, "expected non-null stream");
    this->json = json_2;
  }

  ~NativeGenerator() {
    if (as_ != nullptr) {
      delete as_;
    }
  }

  std::string GetFunctionName() const;

  // Get the buffer containing the compiled machine code.  The start of this
  // buffer is not guaranteed to be a valid entry point.
  //
  // Note: getVectorcallEntry() **must** be called before this is called.
  std::span<const std::byte> getCodeBuffer() const;

  // Get the entry point of the compiled function if it is called via a
  // vectorcall.
  //
  // Note: This is where the function is actually compiled, it is done the first
  // time this method is called.
  void* getVectorcallEntry();

  // Get the entry point of the compiled function if it is called via a Static
  // Python call.
  void* getStaticEntry();

  int GetCompiledFunctionStackSize() const;
  int GetCompiledFunctionSpillStackSize() const;
  const hir::Function* GetFunction() const {
    return func_;
  }

  CodeRuntime* codeRuntime() const {
    return env_.code_rt;
  }

  bool isGen() const {
    return func_->code->co_flags & kCoFlagsAnyGenerator;
  }

#ifdef __ASM_DEBUG
  const char* GetPyFunctionName() const;
#endif
 private:
  const hir::Function* func_;
  void* code_start_{nullptr};
  void* vectorcall_entry_{nullptr};
  asmjit::x86::Builder* as_{nullptr};
  CodeHolderMetadata metadata_{CodeSection::kHot};
  void* deopt_trampoline_{nullptr};
  void* deopt_trampoline_generators_{nullptr};
  void* const failed_deferred_compile_trampoline_;
  FrameAsm frame_asm_;

  size_t compiled_size_{0};
  int spill_stack_size_{-1};
#if PY_VERSION_HEX < 0x030C0000
  const int frame_header_size_;
#endif
  int max_inline_depth_;

  bool hasStaticEntry() const;
  int calcFrameHeaderSize(const hir::Function* func);
  int calcMaxInlineDepth(const hir::Function* func);
  void generateCode(asmjit::CodeHolder& code);
  void generateFunctionEntry();
  void setupFrameAndSaveCallerRegisters(
#ifdef SHADOW_FRAMES
      asmjit::x86::Gp tstate_reg
#endif
  );
  void generatePrologue(
      asmjit::Label correct_arg_count,
      asmjit::Label native_entry_point);
  bool linkFrameNeedsSpill();
  void generateEpilogue(asmjit::BaseNode* epilogue_cursor);
  void generateDeoptExits(const asmjit::CodeHolder& code);
  void linkDeoptPatchers(const asmjit::CodeHolder& code);
  void generateResumeEntry();
  void generateStaticMethodTypeChecks(asmjit::Label setup_frame);
  void generateStaticEntryPoint(
      asmjit::Label native_entry_point,
      asmjit::Label static_jmp_location);

  FRIEND_TEST(LinearScanAllocatorTest, RegAllocation);
  friend class BackendTest;

  void generateAssemblyBody(const asmjit::CodeHolder& code);

  void generatePrimitiveArgsPrologue();
  void generateArgcountCheckPrologue(asmjit::Label correct_arg_count);

  // If the function returns a primitive, then in the generic (non-static) entry
  // path it needs to box it up.  Do this by generating a small wrapper
  // "function" here that calls the real function and boxes its result.
  //
  // Returns the generic entry cursor and the cursor to the boxed wrapper, if it
  // was generated.
  std::pair<asmjit::BaseNode*, asmjit::BaseNode*> generateBoxedReturnWrapper();

  std::unique_ptr<lir::Function> lir_func_;
  Environ env_;
  nlohmann::json* json{nullptr};
};

// Factory class for creating instances of NativeGenerator that reuse the same
// trampolines.
class NativeGeneratorFactory {
 public:
  NativeGeneratorFactory();

  std::unique_ptr<NativeGenerator> operator()(const hir::Function* func) const;

  DISALLOW_COPY_AND_ASSIGN(NativeGeneratorFactory);

 private:
  void* deopt_trampoline_;
  void* deopt_trampoline_generators_;
  void* failed_deferred_compile_trampoline_;
};

} // namespace jit::codegen
