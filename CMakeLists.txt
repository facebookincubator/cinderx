# (c) Meta Platforms, Inc. and affiliates.

message("=====================================================================")
message("=                            WARNING                                =")
message("=====================================================================")
message("=                                                                   =")
message("= This CMake build is not well supported, and is primarily used for =")
message("= experimentation.  Do not expect this to work on your machine.     =")
message("=====================================================================")
message("")

cmake_minimum_required(VERSION 3.12)
project(_cinderx)

set(PROJECT_SOURCE_DIR ${PROJECT_SOURCE_DIR}/cinderx)

##############################################################################
# PGO (Profile-Guided Optimization) configuration

option(ENABLE_PGO_GENERATE "Build with PGO profile generation" OFF)
option(ENABLE_PGO_USE "Build with PGO profile use" OFF)
set(PGO_PROFILE_FILE "" CACHE STRING "Path to PGO profile data")

if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
  set(MACOS 1)
else()
  set(MACOS 0)
endif()

##############################################################################
# Figure out capabilities / supported features and set compiler flags
# appropriately.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(SHARED_FLAGS "-DPy_BUILD_CORE -DPy_BUILD_CORE_MODULE -fPIC -Wall -Wextra -Wno-c99-designator -Wno-c++11-narrowing -Wno-cast-function-type-mismatch -Wno-deprecated-declarations -Wno-missing-field-initializers -Wno-null-pointer-subtraction -Wno-sign-compare -Wno-unknown-pragmas -Wno-unused-function -Wno-unused-parameter")

macro(set_flag VAR)
  if (DEFINED ${VAR} AND ${${VAR}})
    set(SHARED_FLAGS "-D${VAR} ${SHARED_FLAGS}")
  endif()
endmacro()

set_flag(ENABLE_ADAPTIVE_STATIC_PYTHON)
set_flag(ENABLE_DISASSEMBLER)
set_flag(ENABLE_ELF_READER)
set_flag(ENABLE_EVAL_HOOK)
set_flag(ENABLE_FUNC_EVENT_MODIFY_QUALNAME)
set_flag(ENABLE_GENERATOR_AWAITER)
set_flag(ENABLE_INTERPRETER_LOOP)
set_flag(ENABLE_LAZY_IMPORTS)
set_flag(ENABLE_LIGHTWEIGHT_FRAMES)
set_flag(ENABLE_PARALLEL_GC)
set_flag(ENABLE_PEP523_HOOK)
set_flag(ENABLE_PERF_TRAMPOLINE)
set_flag(ENABLE_SYMBOLIZER)
set_flag(ENABLE_USDT)

set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${SHARED_FLAGS}")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${SHARED_FLAGS}")

##############################################################################
# Apply LTO (Link-Time Optimization) if enabled

option(ENABLE_LTO "Enable Link-Time Optimization (full)" OFF)

if(ENABLE_LTO)
  # Only support Linux
  if(MACOS)
    message(FATAL_ERROR "LTO is only supported on Linux")
  endif()

  message(STATUS "LTO: Enabled (full LTO)")

  # Detect compiler type
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(USING_CLANG TRUE)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(USING_GCC TRUE)
  else()
    message(FATAL_ERROR "LTO is only supported with Clang or GCC compilers")
  endif()

  # For Clang, we need llvm-ar instead of regular ar
  if(USING_CLANG)
    find_program(LLVM_AR llvm-ar)
    if(NOT LLVM_AR)
      message(FATAL_ERROR "llvm-ar is required for LTO with Clang but was not found")
    endif()
    set(CMAKE_AR ${LLVM_AR})
    message(STATUS "LTO: Using llvm-ar: ${CMAKE_AR}")

    set(LTO_FLAG "-flto")
    set(LTO_LINKER_FLAGS "-flto")
  elseif(USING_GCC)
    set(LTO_FLAG "-flto")
    set(LTO_LINKER_FLAGS "-flto -fuse-linker-plugin -ffat-lto-objects")
  endif()

  # Apply flags to all C and C++ compilation
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${LTO_FLAG}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${LTO_FLAG}")
  set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
  set(CMAKE_MODULE_LINKER_FLAGS "${CMAKE_MODULE_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} ${LTO_LINKER_FLAGS}")
endif()

##############################################################################
# Apply PGO flags if enabled

if(ENABLE_PGO_GENERATE)
  message(STATUS "PGO: Building with profile generation instrumentation")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-generate")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-generate")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-instr-generate")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-generate")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-generate")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -fprofile-generate")
  else()
    message(WARNING "PGO profile generation is not supported with compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

if(ENABLE_PGO_USE)
  message(STATUS "PGO: Building with profile-guided optimizations")
  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(NOT PGO_PROFILE_FILE)
      message(FATAL_ERROR "PGO_PROFILE_FILE must be set when ENABLE_PGO_USE=ON")
    endif()
    if(NOT EXISTS "${PGO_PROFILE_FILE}")
      message(FATAL_ERROR "PGO profile file does not exist: ${PGO_PROFILE_FILE}")
    endif()
    message(STATUS "PGO: Using profile data from ${PGO_PROFILE_FILE}")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-instr-use=${PGO_PROFILE_FILE}")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-instr-use=${PGO_PROFILE_FILE}")
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
    message(STATUS "PGO: Using GCC profile data from build directory")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fprofile-use -fprofile-correction")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fprofile-use -fprofile-correction")
  else()
    message(WARNING "PGO profile use is not supported with compiler: ${CMAKE_CXX_COMPILER_ID}")
  endif()
endif()

set(GENERATED_HEADER_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GENERATED_HEADER_DIR})

##############################################################################
# Dependencies

include(FetchContent)

########################################
# python

find_package(Python ${PY_VERSION} EXACT COMPONENTS Interpreter Development.Module REQUIRED)

# Some of our files are partially generated from CPython source, and they
# expect to be able to include files relative to Include/internal.
set(Python_INCLUDE_DIRS ${Python_INCLUDE_DIRS} "${Python_INCLUDE_DIRS}/internal")

########################################
# asmjit

set(ASMJIT_STATIC TRUE)

FetchContent_Declare(
  asmjit
  GIT_REPOSITORY https://github.com/asmjit/asmjit
  GIT_TAG cecc73f2979e9704c81a2c2ec79a7475b31c56ac # 2025-May-10
)
FetchContent_MakeAvailable(asmjit)

########################################
# fmt

FetchContent_Declare(
  fmt
  GIT_REPOSITORY https://github.com/fmtlib/fmt
  GIT_TAG 11.2.0 # 2025-May-03
)
FetchContent_MakeAvailable(fmt)

########################################
# parallel-hashmap

FetchContent_Declare(
  parallel-hashmap
  GIT_REPOSITORY https://github.com/greg7mdp/parallel-hashmap
  GIT_TAG 896f1a03e429c45d9fe9638e892fc1da73befadd # 2025-Apr-11
)
FetchContent_MakeAvailable(parallel-hashmap)

########################################
# usdt

FetchContent_Declare(
  usdt
  GIT_REPOSITORY https://github.com/libbpf/usdt
  GIT_TAG f4ea2f524efa80d062f4d586d78daafb83dc7d24 # 2025-Apr-24
)
FetchContent_MakeAvailable(usdt)

# Get directory format for `#include <usdt/usdt.h>` to work.
set(USDT_DIR ${GENERATED_HEADER_DIR}/usdt)
file(MAKE_DIRECTORY ${USDT_DIR})
file(COPY ${usdt_SOURCE_DIR}/usdt.h DESTINATION ${USDT_DIR})

########################################
# zlib

find_package(ZLIB)

##############################################################################
# Build cinderx

include_directories(
  ${PROJECT_SOURCE_DIR}/..
  ${GENERATED_HEADER_DIR}
  ${Python_INCLUDE_DIRS}
  ${parallel-hashmap_SOURCE_DIR}
  ${usdt_SOURCE_DIR}/..
)

########################################
# Common/

file(GLOB_RECURSE COMMON_SOURCES ${PROJECT_SOURCE_DIR}/Common/*.cpp)
add_library(common ${COMMON_SOURCES})
target_link_libraries(common PRIVATE asmjit::asmjit fmt::fmt ZLIB::ZLIB)

########################################
# UpstreamBorrow/

if (${PY_VERSION} EQUAL 3.12 OR ${PY_VERSION} EQUAL 3.14 OR ${PY_VERSION} EQUAL 3.15)
  set(BORROWED_C ${PROJECT_SOURCE_DIR}/UpstreamBorrow/borrowed-${PY_VERSION}.gen_cached.c)
else()
  set(BORROWED_C "${GENERATED_HEADER_DIR}/dummy-borrowed.c")
  file(WRITE ${BORROWED_C} "")
endif()

add_library(borrowed ${BORROWED_C})

########################################
# CachedProperties/

file(GLOB_RECURSE CACHED_PROPERTY_SOURCES ${PROJECT_SOURCE_DIR}/CachedProperties/*.c)
add_library(cached-properties ${CACHED_PROPERTY_SOURCES})
target_link_libraries(cached-properties PRIVATE common borrowed)

########################################
# Interpreter/

set(INTERP_SOURCES ${PROJECT_SOURCE_DIR}/Interpreter/cinder_opcode.c ${PROJECT_SOURCE_DIR}/Interpreter/iter_helpers.c)

if (${PY_VERSION} EQUAL 3.12 OR ${PY_VERSION} EQUAL 3.14 OR ${PY_VERSION} EQUAL 3.15)
  set(SOURCE_CINDERX_OPCODE_TARGETS_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinderx_opcode_targets.h)
  set(SOURCE_CINDER_OPCODE_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode.h)
  set(SOURCE_CINDER_OPCODE_IDS_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode_ids.h)
  set(SOURCE_CINDER_OPCODE_METADATA_H ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/cinder_opcode_metadata.h)

  if (${ENABLE_INTERPRETER_LOOP})
    set(INTERP_SOURCES ${INTERP_SOURCES} ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/interpreter.c)
  endif()
endif()

# Generate cinderx/Interpreter header files.

set(CINDERX_OPCODE_TARGETS_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinderx_opcode_targets.h)
set(CINDER_OPCODE_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode.h)
set(CINDER_OPCODE_IDS_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode_ids.h)
set(CINDER_OPCODE_METADATA_H ${GENERATED_HEADER_DIR}/cinderx/Interpreter/cinder_opcode_metadata.h)

file(READ ${SOURCE_CINDERX_OPCODE_TARGETS_H} FILE_CONTENTS)
file(WRITE ${CINDERX_OPCODE_TARGETS_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_IDS_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_IDS_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_METADATA_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_METADATA_H} "${FILE_CONTENTS}")

file(READ ${SOURCE_CINDER_OPCODE_H} FILE_CONTENTS)
file(WRITE ${CINDER_OPCODE_H} "${FILE_CONTENTS}")

add_library(interpreter ${INTERP_SOURCES})
target_include_directories(
  interpreter
  PUBLIC
  ${PROJECT_SOURCE_DIR}/Interpreter/${PY_VERSION}/Includes
)

########################################
# Immortalize/

file(GLOB_RECURSE IMMORTALIZE_SOURCES ${PROJECT_SOURCE_DIR}/Immortalize/*.cpp)
add_library(immortalize ${IMMORTALIZE_SOURCES})
target_link_libraries(immortalize PRIVATE asmjit::asmjit fmt::fmt)

########################################
# StaticPython/

file(GLOB_RECURSE STATIC_PYTHON_SOURCES ${PROJECT_SOURCE_DIR}/StaticPython/*.c)
add_library(static-python ${STATIC_PYTHON_SOURCES})
target_link_libraries(static-python PRIVATE fmt::fmt asmjit::asmjit common borrowed)

########################################
# Jit/

file(GLOB_RECURSE JIT_SOURCES ${PROJECT_SOURCE_DIR}/Jit/*.cpp ${PROJECT_SOURCE_DIR}/Jit/*.c)
list(FILTER JIT_SOURCES EXCLUDE REGEX ".*\.gen_cached\.c")
if (${PY_VERSION} EQUAL 3.12 OR ${PY_VERSION} EQUAL 3.14 OR ${PY_VERSION} EQUAL 3.15)
  list(APPEND JIT_SOURCES "${PROJECT_SOURCE_DIR}/Jit/generators_borrowed_${PY_VERSION}.gen_cached.c")
endif()

add_library(jit ${JIT_SOURCES})
target_link_libraries(jit PRIVATE asmjit::asmjit interpreter fmt::fmt)

########################################
# ParallelGC/

if (${ENABLE_PARALLEL_GC})
  file(GLOB_RECURSE PARALLEL_GC_SOURCES ${PROJECT_SOURCE_DIR}/ParallelGC/*.c)
else()
  set(PARALLEL_GC_SOURCES "${GENERATED_HEADER_DIR}/dummy-parallel-gc.c")
  file(WRITE ${PARALLEL_GC_SOURCES} "")
endif()
add_library(parallel-gc ${PARALLEL_GC_SOURCES})

########################################
# _cinderx.cpp

set(SOURCES
  # Manually listed out to not accidentally include stuff in .git.
  ${PROJECT_SOURCE_DIR}/_cinderx.cpp
  ${PROJECT_SOURCE_DIR}/_cinderx-lib.cpp
  ${PROJECT_SOURCE_DIR}/async_lazy_value.cpp
  ${PROJECT_SOURCE_DIR}/module_state.cpp
  ${PROJECT_SOURCE_DIR}/module_c_state.cpp
  ${PROJECT_SOURCE_DIR}/python_runtime.cpp
)

add_library(${PROJECT_NAME} SHARED ${SOURCES})

target_link_libraries(
  ${PROJECT_NAME}
  PRIVATE
  borrowed cached-properties common immortalize interpreter jit parallel-gc static-python
  asmjit::asmjit fmt::fmt)

# macOS doesn't allow depending on other shared libraries by default.
if (${MACOS})
  target_link_options(${PROJECT_NAME} PRIVATE -undefined dynamic_lookup)
endif()

set_target_properties(${PROJECT_NAME} PROPERTIES PREFIX "")
set_target_properties(${PROJECT_NAME} PROPERTIES SUFFIX "")
set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME "_cinderx.so")
