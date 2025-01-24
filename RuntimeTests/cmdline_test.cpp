// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/codegen/copy_graph.h"
#include "cinderx/Jit/jit_gdb_support.h"
#include "cinderx/Jit/jit_list.h"
#include "cinderx/Jit/lir/inliner.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/testutil.h"
#include "i386-dis/dis-asm.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream>
#include <string>

// Here we make sure that the JIT specific command line arguments
// are being processed correctly to have the required effect
// on the JIT config

using namespace jit;
using namespace std;

using namespace jit::lir;

class CmdLineTest : public RuntimeTest {
 public:
  CmdLineTest() : RuntimeTest{RuntimeTest::Flags{}} {}
};

int try_flag_and_envvar_effect(
    const wchar_t* flag,
    const char* env_name,
    function<void(void)> reset_vars,
    function<void(void)> conditions_to_check,
    bool enable_JIT = false,
    bool capture_stderr = false,
    bool capture_stdout = false) {
  // Shutdown the JIT so we can start it up again under different conditions.
  _PyJIT_Finalize();

  reset_vars(); // reset variable state before and
  // between flag and cmd line param runs

  int init_status = 0;

  PyObject* jit_xarg_key = nullptr;

  if (enable_JIT) {
    jit_xarg_key = addToXargsDict(L"jit");
  }

  // as env var
  if (nullptr != env_name) {
    if (capture_stderr) {
      testing::internal::CaptureStderr();
    }
    if (capture_stdout) {
      testing::internal::CaptureStdout();
    }

    const char* key = parseAndSetEnvVar(env_name);
    init_status = _PyJIT_Initialize();
    conditions_to_check();
    unsetenv(key);
    if (strcmp(env_name, key)) {
      free((char*)key);
    }
    _PyJIT_Finalize();
    reset_vars();
  }

  if (capture_stderr) {
    testing::internal::CaptureStderr();
  }
  if (capture_stdout) {
    testing::internal::CaptureStdout();
  }
  // sneak in a command line argument
  PyObject* to_remove = addToXargsDict(flag);
  init_status += _PyJIT_Initialize();
  conditions_to_check();
  PyDict_DelItem(PySys_GetXOptions(), to_remove);
  Py_DECREF(to_remove);

  if (nullptr != jit_xarg_key) {
    PyDict_DelItem(PySys_GetXOptions(), jit_xarg_key);
    Py_DECREF(jit_xarg_key);
  }

  _PyJIT_Finalize();
  reset_vars();

  return init_status;
}

TEST_F(CmdLineTest, BasicFlags) {
  // easy flags that don't interact with one another in tricky ways
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug",
          "PYTHONJITDEBUG",
          []() { g_debug = 0; },
          []() { ASSERT_EQ(g_debug, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-refcount",
          "PYTHONJITDEBUGREFCOUNT",
          []() { g_debug_refcount = 0; },
          []() { ASSERT_EQ(g_debug_refcount, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-inliner",
          "PYTHONJITDEBUGINLINER",
          []() { g_debug_inliner = 0; },
          []() { ASSERT_EQ(g_debug_inliner, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir",
          "PYTHONJITDUMPHIR",
          []() { g_dump_hir = 0; },
          []() { ASSERT_EQ(g_dump_hir, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir-passes",
          "PYTHONJITDUMPHIRPASSES",
          []() { g_dump_hir_passes = 0; },
          []() { ASSERT_EQ(g_dump_hir_passes, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-final-hir",
          "PYTHONJITDUMPFINALHIR",
          []() { g_dump_final_hir = 0; },
          []() { ASSERT_EQ(g_dump_final_hir, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir",
          "PYTHONJITDUMPLIR",
          []() { g_dump_lir = 0; },
          []() { ASSERT_EQ(g_dump_lir, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir-no-origin",
          "PYTHONJITDUMPLIRNOORIGIN",
          []() {
            g_dump_lir = 0;
            g_dump_lir_no_origin = 0;
          },
          []() {
            ASSERT_EQ(g_dump_lir, 1);
            ASSERT_EQ(g_dump_lir_no_origin, 1);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-c-helper",
          "PYTHONJITDUMPCHELPER",
          []() { g_dump_c_helper = 0; },
          []() { ASSERT_EQ(g_dump_c_helper, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disas-funcs",
          "PYTHONJITDISASFUNCS",
          []() { g_dump_asm = 0; },
          []() { ASSERT_EQ(g_dump_asm, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-asm",
          "PYTHONJITDUMPASM",
          []() { g_dump_asm = 0; },
          []() { ASSERT_EQ(g_dump_asm, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-support",
          "PYTHONJITGDBSUPPORT",
          []() {
            g_debug = 0;
            getMutableConfig().gdb.supported = false;
          },
          []() {
            ASSERT_EQ(g_debug, 1);
            ASSERT_TRUE(getConfig().gdb.supported);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-write-elf",
          "PYTHONJITGDBWRITEELF",
          []() {
            g_debug = 0;
            getMutableConfig().gdb.supported = false;
            getMutableConfig().gdb.write_elf_objects = false;
          },
          []() {
            ASSERT_EQ(g_debug, 1);
            ASSERT_TRUE(getConfig().gdb.supported);
            ASSERT_TRUE(getConfig().gdb.write_elf_objects);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-stats",
          "PYTHONJITDUMPSTATS",
          []() { g_dump_stats = 0; },
          []() { ASSERT_EQ(g_dump_stats, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable-lir-inliner",
          "PYTHONJITDISABLELIRINLINER",
          []() { g_disable_lir_inliner = 0; },
          []() { ASSERT_EQ(g_disable_lir_inliner, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable-huge-pages",
          "PYTHONJITDISABLEHUGEPAGES",
          []() {},
          []() { ASSERT_FALSE(getConfig().use_huge_pages); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-enable-jit-list-wildcards",
          "PYTHONJITENABLEJITLISTWILDCARDS",
          []() {},
          []() { ASSERT_TRUE(getConfig().allow_jit_list_wildcards); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-all-static-functions",
          "PYTHONJITALLSTATICFUNCTIONS",
          []() {},
          []() { ASSERT_TRUE(getConfig().compile_all_static_functions); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-perfmap",
          "JIT_PERFMAP",
          []() { perf::jit_perfmap = 0; },
          []() { ASSERT_EQ(perf::jit_perfmap, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-perf-dumpdir=/tmp/",
          "JIT_DUMPDIR=/tmp/",
          []() { perf::perf_jitdump_dir = ""; },
          []() { ASSERT_EQ(perf::perf_jitdump_dir, "/tmp/"); }),
      0);
}

TEST_F(CmdLineTest, JITEnable) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit",
          "PYTHONJIT",
          []() {},
          []() {
            ASSERT_TRUE(isJitUsable());
            ASSERT_EQ(is_intel_syntax(), 0); // default to AT&T syntax
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit=0",
          "PYTHONJIT=0",
          []() {},
          []() { ASSERT_FALSE(isJitUsable()); }),
      0);
}

// start of tests associated with flags the setting of which is dependent upon
// if jit is enabled
TEST_F(CmdLineTest, JITEnabledFlags_ShadowFrame) {
  // Shadow frames don't exist past 3.10.
  auto shadow_mode =
      PY_VERSION_HEX >= 0x030B0000 ? FrameMode::kNormal : FrameMode::kShadow;

  // Flag does nothing when JIT is disabled.
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-shadow-frame",
          "PYTHONJITSHADOWFRAME",
          []() {},
          []() { ASSERT_EQ(getConfig().frame_mode, FrameMode::kNormal); },
          false /* enable_jit */),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-shadow-frame",
          "PYTHONJITSHADOWFRAME",
          []() {},
          [=]() { ASSERT_EQ(getConfig().frame_mode, shadow_mode); },
          true /* enable_jit */),
      0);

  // Explicitly disable it.
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-shadow-frame=0",
          "PYTHONJITSHADOWFRAME=0",
          []() {},
          []() { ASSERT_EQ(getConfig().frame_mode, FrameMode::kNormal); },
          true /* enable_jit */),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MultithreadCompile) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          []() {},
          []() { ASSERT_FALSE(getConfig().multithreaded_compile_test); },
          false),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          []() {},
          []() { ASSERT_TRUE(getConfig().multithreaded_compile_test); },
          true),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MatchLineNumbers) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-match-line-numbers",
          "PYTHONJITLISTMATCHLINENUMBERS",
          []() { jitlist_match_line_numbers(false); },
          []() { ASSERT_FALSE(get_jitlist_match_line_numbers()); },
          false),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-match-line-numbers",
          "PYTHONJITLISTMATCHLINENUMBERS",
          []() { jitlist_match_line_numbers(false); },
          []() { ASSERT_TRUE(get_jitlist_match_line_numbers()); },
          true),
      0);
}

// end of tests associated with flags the setting of which is dependent upon if
// jit is enabled

TEST_F(CmdLineTest, JITEnabledFlags_BatchCompileWorkers) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-batch-compile-workers=21",
          "PYTHONJITBATCHCOMPILEWORKERS=21",
          []() {},
          []() { ASSERT_EQ(getConfig().batch_compile_workers, 21); },
          true),
      0);
}

TEST_F(CmdLineTest, ASMSyntax) {
  // default when nothing defined is AT&T, covered in prvious test
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=intel",
          "PYTHONJITASMSYNTAX=intel",
          []() { set_att_syntax(); },
          []() { ASSERT_EQ(is_intel_syntax(), 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=att",
          "PYTHONJITASMSYNTAX=att",
          []() { set_att_syntax(); },
          []() { ASSERT_EQ(is_intel_syntax(), 0); }),
      0);
}

const wchar_t* makeWideChar(const char* to_convert) {
  const size_t cSize = strlen(to_convert) + 1;
  wchar_t* wide = new wchar_t[cSize];
  mbstowcs(wide, to_convert, cSize);

  return wide;
}

TEST_F(CmdLineTest, JITList) {
  std::string list_file = tmpnam(nullptr);
  ofstream list_file_handle(list_file);
  list_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-list-file=" + list_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLISTFILE=" + list_file).c_str()),
          []() { set_att_syntax(); },
          []() { ASSERT_TRUE(isJitUsable()); }),
      0);

  delete[] xarg;
  filesystem::remove(list_file);
}

TEST_F(CmdLineTest, JITLogFile) {
  std::string log_file = tmpnam(nullptr);
  ofstream log_file_handle(log_file);
  log_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-log-file=" + log_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLOGFILE=" + log_file).c_str()),
          []() { g_log_file = stderr; },
          []() { ASSERT_NE(g_log_file, stderr); }),
      0);

  delete[] xarg;
  filesystem::remove(log_file);
}

TEST_F(CmdLineTest, ExplicitJITDisable) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable",
          "PYTHONJITDISABLE",
          []() {},
          []() { ASSERT_FALSE(isJitUsable()); },
          true),
      0);
}

TEST_F(CmdLineTest, DisplayHelpMessage) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-help",
          nullptr,
          []() {},
          []() {
            ASSERT_TRUE(
                testing::internal::GetCapturedStdout().find(
                    "-X opt : set Cinder JIT-specific option.") !=
                std::string::npos);
          },
          false,
          false,
          true),
      -2);
}
