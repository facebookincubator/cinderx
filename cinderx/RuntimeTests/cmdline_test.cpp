// Copyright (c) Meta Platforms, Inc. and affiliates.

#include <gtest/gtest.h>

#include "cinderx/Jit/generators_rt.h"
#include "cinderx/Jit/perf_jitdump.h"
#include "cinderx/Jit/pyjit.h"
#include "cinderx/RuntimeTests/fixtures.h"
#include "cinderx/RuntimeTests/testutil.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

// Here we make sure that the JIT specific command line arguments
// are being processed correctly to have the required effect
// on the JIT config

using namespace jit;
using namespace jit::lir;

class CmdLineTest : public RuntimeTest {
 public:
  CmdLineTest() : RuntimeTest{RuntimeTest::Flags{}} {}
};

int try_flag_and_envvar_effect(
    const wchar_t* flag,
    const char* env_name,
    std::function<void(void)> reset_vars,
    std::function<void(void)> conditions_to_check,
    bool capture_stderr = false,
    bool capture_stdout = false) {
  // Shutdown the JIT so we can start it up again under different conditions.
  jit::finalize();

#if PY_VERSION_HEX >= 0x030C0000
  jit::shutdown_jit_genobject_type();
#endif

  reset_vars(); // reset variable state before and
  // between flag and cmd line param runs

  int init_status = 0;

  // as env var
  if (nullptr != env_name) {
    if (capture_stderr) {
      testing::internal::CaptureStderr();
    }
    if (capture_stdout) {
      testing::internal::CaptureStdout();
    }

    std::string key = parseAndSetEnvVar(env_name);
    init_status = jit::initialize();
    conditions_to_check();
    unsetenv(key.c_str());
    jit::finalize();
#if PY_VERSION_HEX >= 0x030C0000
    jit::shutdown_jit_genobject_type();
#endif
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
  init_status += jit::initialize();
  conditions_to_check();
  PyDict_DelItem(PySys_GetXOptions(), to_remove);
  Py_DECREF(to_remove);

  jit::finalize();
  reset_vars();

  return init_status;
}

TEST_F(CmdLineTest, BasicFlags) {
  // easy flags that don't interact with one another in tricky ways
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug",
          "PYTHONJITDEBUG",
          []() { getMutableConfig().log.debug = false; },
          []() { ASSERT_TRUE(getConfig().log.debug); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-refcount",
          "PYTHONJITDEBUGREFCOUNT",
          []() { getMutableConfig().log.debug_refcount = false; },
          []() { ASSERT_TRUE(getConfig().log.debug_refcount); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-debug-inliner",
          "PYTHONJITDEBUGINLINER",
          []() { getMutableConfig().log.debug_inliner = false; },
          []() { ASSERT_TRUE(getConfig().log.debug_inliner); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir",
          "PYTHONJITDUMPHIR",
          []() { getMutableConfig().log.dump_hir_initial = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_initial); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-hir-passes",
          "PYTHONJITDUMPHIRPASSES",
          []() { getMutableConfig().log.dump_hir_passes = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_passes); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-final-hir",
          "PYTHONJITDUMPFINALHIR",
          []() { getMutableConfig().log.dump_hir_final = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_hir_final); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir",
          "PYTHONJITDUMPLIR",
          []() { getMutableConfig().log.dump_lir = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_lir); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-lir-origin",
          "PYTHONJITDUMPLIRORIGIN",
          []() {
            getMutableConfig().log.dump_lir = false;
            getMutableConfig().log.lir_origin = false;
          },
          []() {
            ASSERT_TRUE(getConfig().log.dump_lir);
            ASSERT_TRUE(getConfig().log.lir_origin);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-asm",
          "PYTHONJITDUMPASM",
          []() { getMutableConfig().log.dump_asm = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_asm); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-support",
          "PYTHONJITGDBSUPPORT",
          []() {
            getMutableConfig().log.debug = false;
            getMutableConfig().gdb.supported = false;
          },
          []() {
            ASSERT_TRUE(getMutableConfig().log.debug);
            ASSERT_TRUE(getConfig().gdb.supported);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-gdb-write-elf",
          "PYTHONJITGDBWRITEELF",
          []() {
            getMutableConfig().log.debug = false;
            getMutableConfig().gdb.supported = false;
            getMutableConfig().gdb.write_elf_objects = false;
          },
          []() {
            ASSERT_TRUE(getConfig().log.debug);
            ASSERT_TRUE(getConfig().gdb.supported);
            ASSERT_TRUE(getConfig().gdb.write_elf_objects);
          }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-dump-stats",
          "PYTHONJITDUMPSTATS",
          []() { getMutableConfig().log.dump_stats = false; },
          []() { ASSERT_TRUE(getConfig().log.dump_stats); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable-lir-inliner",
          "PYTHONJITDISABLELIRINLINER",
          []() { getMutableConfig().lir_opts.inliner = 0; },
          []() { ASSERT_EQ(getConfig().lir_opts.inliner, 1); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-huge-pages=0",
          "PYTHONJITHUGEPAGES=0",
          []() {},
          []() { ASSERT_FALSE(getConfig().use_huge_pages); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-huge-pages=1",
          "PYTHONJITHUGEPAGES=1",
          []() {},
          []() { ASSERT_TRUE(getConfig().use_huge_pages); }),
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
          L"jit-all",
          "PYTHONJITALL",
          []() {},
          []() {
            ASSERT_TRUE(isJitUsable());
            ASSERT_EQ(getConfig().compile_after_n_calls, 0);
            ASSERT_EQ(
                getConfig().asm_syntax,
                AsmSyntax::ATT); // default to AT&T syntax
          }),
      0);
}

// start of tests associated with flags the setting of which is dependent upon
// if jit is enabled
TEST_F(CmdLineTest, JITEnabledFlags_ShadowFrame) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-shadow-frame",
          "PYTHONJITSHADOWFRAME",
          []() {},
          []() {
            // Shadow frames don't exist past 3.10.
            if constexpr (PY_VERSION_HEX < 0x030C0000) {
              ASSERT_EQ(getConfig().frame_mode, FrameMode::kShadow);
            }
          }),
      0);

  // Explicitly disable it.
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-shadow-frame=0",
          "PYTHONJITSHADOWFRAME=0",
          []() {},
          []() { ASSERT_NE(getConfig().frame_mode, FrameMode::kShadow); }),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MultithreadCompile) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-multithreaded-compile-test",
          "PYTHONJITMULTITHREADEDCOMPILETEST",
          []() {},
          []() { ASSERT_TRUE(getConfig().multithreaded_compile_test); }),
      0);
}

TEST_F(CmdLineTest, JITEnabledFlags_MatchLineNumbers) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-list-match-line-numbers",
          "PYTHONJITLISTMATCHLINENUMBERS",
          []() { getMutableConfig().jit_list.match_line_numbers = false; },
          []() { ASSERT_TRUE(getConfig().jit_list.match_line_numbers); }),
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
          []() { ASSERT_EQ(getConfig().batch_compile_workers, 21); }),
      0);
}

TEST_F(CmdLineTest, ASMSyntax) {
  // default when nothing defined is AT&T, covered in prvious test
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=intel",
          "PYTHONJITASMSYNTAX=intel",
          []() { getMutableConfig().asm_syntax = AsmSyntax::ATT; },
          []() { ASSERT_EQ(getConfig().asm_syntax, AsmSyntax::Intel); }),
      0);

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-asm-syntax=att",
          "PYTHONJITASMSYNTAX=att",
          []() { getMutableConfig().asm_syntax = AsmSyntax::ATT; },
          []() { ASSERT_EQ(getConfig().asm_syntax, AsmSyntax::ATT); }),
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
  std::ofstream list_file_handle(list_file);
  list_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-list-file=" + list_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLISTFILE=" + list_file).c_str()),
          []() { getMutableConfig().asm_syntax = AsmSyntax::ATT; },
          []() { ASSERT_TRUE(isJitUsable()); }),
      0);

  delete[] xarg;
  std::filesystem::remove(list_file);
}

TEST_F(CmdLineTest, JITLogFile) {
  std::string log_file = tmpnam(nullptr);
  std::ofstream log_file_handle(log_file);
  log_file_handle.close();
  const wchar_t* xarg =
      makeWideChar(const_cast<char*>(("jit-log-file=" + log_file).c_str()));

  ASSERT_EQ(
      try_flag_and_envvar_effect(
          xarg,
          const_cast<char*>(("PYTHONJITLOGFILE=" + log_file).c_str()),
          []() { getMutableConfig().log.output_file = stderr; },
          []() { ASSERT_NE(getConfig().log.output_file, stderr); }),
      0);

  delete[] xarg;
  std::filesystem::remove(log_file);
}

TEST_F(CmdLineTest, ExplicitJITDisable) {
  ASSERT_EQ(
      try_flag_and_envvar_effect(
          L"jit-disable",
          "PYTHONJITDISABLE",
          []() {},
          []() { ASSERT_FALSE(isJitUsable()); }),
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
          false /* capture_stderr */,
          true /* capture_stdout */),
      -2);
}
